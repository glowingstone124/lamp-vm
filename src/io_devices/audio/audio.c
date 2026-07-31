#include "audio.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#include "../dma/dma_ring.h"
#include "../iommu/iommu_mmio_register.h"
#include "../pcie/pcie.h"
#include "../../interrupt.h"
#include "../../runtime_log.h"
#include "../../../include/lampvm/device_abi.h"

#define AUDIO_POLL_BUDGET 32u
#define AUDIO_HOST_QUEUE_LIMIT (LAMP_AUDIO_RATE * LAMP_AUDIO_FRAME_BYTES)

typedef struct audio_state {
    vm_dma_ring_t ring;
    PciFunction *pci_function;
    SDL_AudioStream *stream;
    uint32_t status;
    uint32_t irq_status;
    uint32_t irq_enable;
    uint32_t queued_bytes;
    uint32_t completed_descs;
    uint8_t sdl_audio_initialized;
} audio_state_t;

static uint64_t audio_set_lo32(uint64_t old_value, uint32_t lo) {
    return (old_value & 0xFFFFFFFF00000000ull) | (uint64_t)lo;
}

static uint64_t audio_set_hi32(uint64_t old_value, uint32_t hi) {
    return (old_value & 0x00000000FFFFFFFFull) | ((uint64_t)hi << 32);
}

static void audio_sync_backend_status(audio_state_t *audio) {
    if (audio->stream) {
        audio->status &= ~LAMP_AUDIO_STATUS_BACKEND_UNAVAILABLE;
    } else {
        audio->status |= LAMP_AUDIO_STATUS_BACKEND_UNAVAILABLE;
        audio->queued_bytes = 0u;
    }
}

static void audio_raise_irq(VM *vm, audio_state_t *audio, uint32_t bits) {
    const uint32_t new_bits = bits & ~audio->irq_status;
    audio->irq_status |= bits;
    if ((new_bits & audio->irq_enable) != 0u && audio->pci_function) {
        pci_notify_irq(vm, audio->pci_function);
    }
}

static void audio_set_error(VM *vm, audio_state_t *audio, uint32_t status_bit) {
    audio->status |= status_bit;
    audio_raise_irq(vm, audio, LAMP_AUDIO_IRQ_ERROR);
}

static void audio_reset(audio_state_t *audio) {
    vm_dma_ring_reset(&audio->ring);
    audio->status = LAMP_AUDIO_STATUS_READY;
    audio->irq_status = 0u;
    audio->irq_enable = 0u;
    audio->queued_bytes = 0u;
    audio->completed_descs = 0u;
    audio_sync_backend_status(audio);
}

static uint32_t audio_reg_read(audio_state_t *audio, uint32_t offset) {
    if (offset == LAMP_AUDIO_REG_QUEUED_BYTES && audio->stream) {
        const int queued = SDL_GetAudioStreamQueued(audio->stream);
        audio->queued_bytes = queued > 0 ? (uint32_t)queued : 0u;
    }
    switch (offset) {
        case LAMP_AUDIO_REG_MAGIC: return LAMP_AUDIO_MAGIC;
        case LAMP_AUDIO_REG_VERSION: return LAMP_AUDIO_VERSION;
        case LAMP_AUDIO_REG_CAPS:
            return LAMP_AUDIO_CAP_PLAYBACK | LAMP_AUDIO_CAP_DMA_RING |
                   LAMP_AUDIO_CAP_COMPLETION_RING | LAMP_AUDIO_CAP_MSI |
                   LAMP_AUDIO_CAP_IOMMU;
        case LAMP_AUDIO_REG_STATUS: return audio->status;
        case LAMP_AUDIO_REG_RATE: return LAMP_AUDIO_RATE;
        case LAMP_AUDIO_REG_CHANNELS: return LAMP_AUDIO_CHANNELS;
        case LAMP_AUDIO_REG_SAMPLE_BITS: return LAMP_AUDIO_SAMPLE_BITS;
        case LAMP_AUDIO_REG_FRAME_BYTES: return LAMP_AUDIO_FRAME_BYTES;
        case LAMP_AUDIO_REG_SUBMIT_BASE_LO: return (uint32_t)audio->ring.submit_iova;
        case LAMP_AUDIO_REG_SUBMIT_BASE_HI: return (uint32_t)(audio->ring.submit_iova >> 32);
        case LAMP_AUDIO_REG_SUBMIT_COUNT: return audio->ring.submit_count;
        case LAMP_AUDIO_REG_SUBMIT_HEAD: return audio->ring.submit_head;
        case LAMP_AUDIO_REG_SUBMIT_TAIL: return audio->ring.submit_tail;
        case LAMP_AUDIO_REG_COMPLETE_BASE_LO: return (uint32_t)audio->ring.complete_iova;
        case LAMP_AUDIO_REG_COMPLETE_BASE_HI: return (uint32_t)(audio->ring.complete_iova >> 32);
        case LAMP_AUDIO_REG_COMPLETE_COUNT: return audio->ring.complete_count;
        case LAMP_AUDIO_REG_COMPLETE_HEAD: return audio->ring.complete_head;
        case LAMP_AUDIO_REG_COMPLETE_TAIL: return audio->ring.complete_tail;
        case LAMP_AUDIO_REG_IRQ_STATUS: return audio->irq_status;
        case LAMP_AUDIO_REG_IRQ_ENABLE: return audio->irq_enable;
        case LAMP_AUDIO_REG_QUEUED_BYTES: return audio->queued_bytes;
        case LAMP_AUDIO_REG_COMPLETED_DESCS: return audio->completed_descs;
        default: return 0u;
    }
}

static void audio_config_count(VM *vm, audio_state_t *audio,
                               uint32_t *count_field, uint32_t value) {
    if (!vm_dma_ring_count_valid(value)) {
        *count_field = 0u;
        audio_set_error(vm, audio, LAMP_AUDIO_STATUS_RING_ERROR);
        return;
    }
    *count_field = value;
}

static void audio_reg_write(VM *vm, audio_state_t *audio,
                            uint32_t offset, uint32_t value) {
    switch (offset) {
        case LAMP_AUDIO_REG_SUBMIT_BASE_LO:
            audio->ring.submit_iova = audio_set_lo32(audio->ring.submit_iova, value);
            break;
        case LAMP_AUDIO_REG_SUBMIT_BASE_HI:
            audio->ring.submit_iova = audio_set_hi32(audio->ring.submit_iova, value);
            break;
        case LAMP_AUDIO_REG_SUBMIT_COUNT:
            audio_config_count(vm, audio, &audio->ring.submit_count, value);
            audio->ring.submit_head = 0u;
            audio->ring.submit_tail = 0u;
            break;
        case LAMP_AUDIO_REG_SUBMIT_TAIL:
            if (!vm_dma_ring_set_submit_tail(&audio->ring, value)) {
                audio_set_error(vm, audio, LAMP_AUDIO_STATUS_RING_ERROR);
            }
            break;
        case LAMP_AUDIO_REG_COMPLETE_BASE_LO:
            audio->ring.complete_iova = audio_set_lo32(audio->ring.complete_iova, value);
            break;
        case LAMP_AUDIO_REG_COMPLETE_BASE_HI:
            audio->ring.complete_iova = audio_set_hi32(audio->ring.complete_iova, value);
            break;
        case LAMP_AUDIO_REG_COMPLETE_COUNT:
            audio_config_count(vm, audio, &audio->ring.complete_count, value);
            audio->ring.complete_head = 0u;
            audio->ring.complete_tail = 0u;
            break;
        case LAMP_AUDIO_REG_COMPLETE_HEAD:
            if (!vm_dma_ring_set_complete_head(&audio->ring, value)) {
                audio_set_error(vm, audio, LAMP_AUDIO_STATUS_RING_ERROR);
            } else {
                audio->status &= ~LAMP_AUDIO_STATUS_COMPLETION_FULL;
            }
            break;
        case LAMP_AUDIO_REG_COMMAND:
            if ((value & LAMP_AUDIO_CMD_RESET) != 0u) {
                audio_reset(audio);
                break;
            }
            if ((value & LAMP_AUDIO_CMD_DISABLE) != 0u) {
                audio->status &= ~LAMP_AUDIO_STATUS_RUNNING;
            }
            if ((value & LAMP_AUDIO_CMD_ENABLE) != 0u) {
                if (vm_dma_ring_ready(&audio->ring)) {
                    audio->status &= ~(LAMP_AUDIO_STATUS_RING_ERROR |
                                       LAMP_AUDIO_STATUS_COMPLETION_FULL |
                                       LAMP_AUDIO_STATUS_DMA_FAULT);
                    audio->status |= LAMP_AUDIO_STATUS_RUNNING;
                } else {
                    audio_set_error(vm, audio, LAMP_AUDIO_STATUS_RING_ERROR);
                }
            }
            break;
        case LAMP_AUDIO_REG_IRQ_ENABLE:
            audio->irq_enable = value &
                (LAMP_AUDIO_IRQ_COMPLETION | LAMP_AUDIO_IRQ_ERROR);
            break;
        case LAMP_AUDIO_REG_IRQ_ACK:
            audio->irq_status &= ~value;
            if (audio->pci_function && audio->irq_status == 0u) {
                audio->pci_function->status &= (uint16_t)~PCI_STATUS_INTX;
            }
            break;
        default:
            break;
    }
}

static uint32_t audio_bar_read32(VM *vm, PciFunction *function,
                                 uint32_t bar_index, uint32_t offset) {
    audio_state_t *audio = (audio_state_t *)function->cookie;
    (void)vm;
    if (!audio || bar_index != 0u) {
        return 0xFFFFFFFFu;
    }
    return audio_reg_read(audio, offset);
}

static void audio_bar_write32(VM *vm, PciFunction *function,
                              uint32_t bar_index, uint32_t offset, uint32_t value) {
    audio_state_t *audio = (audio_state_t *)function->cookie;
    if (audio && bar_index == 0u) {
        audio_reg_write(vm, audio, offset, value);
    }
}

static uint32_t audio_desc_status(VM *vm, const lamp_dma_desc_t *desc,
                                  const uint8_t **payload_out) {
    const uint32_t allowed_flags = LAMP_DMA_DESC_F_IRQ | LAMP_DMA_DESC_F_END;
    const uint64_t iova = (uint64_t)desc->addr_lo |
                          ((uint64_t)desc->addr_hi << 32);
    uint64_t pa = 0u;
    if (desc->length == 0u || desc->length > LAMP_AUDIO_MAX_BUFFER_BYTES ||
        (desc->length % LAMP_AUDIO_FRAME_BYTES) != 0u ||
        (desc->flags & ~allowed_flags) != 0u ||
        desc->reserved0 != 0u || desc->reserved1 != 0u || desc->reserved2 != 0u) {
        return LAMP_DMA_COMPLETION_BAD_DESC;
    }
    if (!vm_iommu_translate_dma_ex(vm, IOMMU_DEV_AUDIO, iova, desc->length,
                                   IOMMU_DMA_READ, &pa) ||
        pa >= (uint64_t)vm->memory_size ||
        desc->length > (uint64_t)vm->memory_size - pa) {
        return LAMP_DMA_COMPLETION_DMA_FAULT;
    }
    *payload_out = &vm->memory[(size_t)pa];
    return LAMP_DMA_COMPLETION_OK;
}

void audio_poll(VM *vm) {
    audio_state_t *audio;
    uint32_t processed = 0u;
    uint32_t want_irq = 0u;
    if (!vm || !(audio = (audio_state_t *)vm->audio) ||
        (audio->status & LAMP_AUDIO_STATUS_RUNNING) == 0u) {
        return;
    }

    while (processed < AUDIO_POLL_BUDGET &&
           vm_dma_ring_submission_available(&audio->ring)) {
        lamp_dma_desc_t desc;
        const uint8_t *payload = NULL;
        uint32_t completion_status;
        uint32_t completed_bytes = 0u;

        if (!vm_dma_ring_completion_space(&audio->ring)) {
            audio_set_error(vm, audio, LAMP_AUDIO_STATUS_COMPLETION_FULL);
            break;
        }
        if (!vm_dma_ring_peek(vm, IOMMU_DEV_AUDIO, &audio->ring, &desc)) {
            audio_set_error(vm, audio, LAMP_AUDIO_STATUS_DMA_FAULT);
            break;
        }

        completion_status = audio_desc_status(vm, &desc, &payload);
        if (completion_status == LAMP_DMA_COMPLETION_OK && audio->stream) {
            const int queued = SDL_GetAudioStreamQueued(audio->stream);
            if (queued >= 0 && (uint32_t)queued + desc.length > AUDIO_HOST_QUEUE_LIMIT) {
                audio->queued_bytes = (uint32_t)queued;
                break;
            }
            if (queued < 0 || !SDL_PutAudioStreamData(audio->stream, payload,
                                                       (int)desc.length)) {
                completion_status = LAMP_DMA_COMPLETION_BACKEND_ERROR;
                audio_sync_backend_status(audio);
            } else {
                completed_bytes = desc.length;
                audio->queued_bytes = (uint32_t)queued + desc.length;
            }
        } else if (completion_status == LAMP_DMA_COMPLETION_OK) {
            /* A null sink keeps the guest queue live on headless hosts. */
            completed_bytes = desc.length;
        }

        if (completion_status == LAMP_DMA_COMPLETION_BAD_DESC) {
            audio->status |= LAMP_AUDIO_STATUS_RING_ERROR;
        } else if (completion_status == LAMP_DMA_COMPLETION_DMA_FAULT) {
            audio->status |= LAMP_AUDIO_STATUS_DMA_FAULT;
        } else if (completion_status == LAMP_DMA_COMPLETION_BACKEND_ERROR) {
            audio->status |= LAMP_AUDIO_STATUS_BACKEND_UNAVAILABLE;
        }

        if (!vm_dma_ring_complete(vm, IOMMU_DEV_AUDIO, &audio->ring,
                                  desc.cookie, completion_status, completed_bytes)) {
            audio_set_error(vm, audio, LAMP_AUDIO_STATUS_DMA_FAULT);
            vm_dma_ring_consume(&audio->ring);
            break;
        }
        vm_dma_ring_consume(&audio->ring);
        audio->completed_descs++;
        processed++;
        if ((desc.flags & LAMP_DMA_DESC_F_IRQ) != 0u) {
            want_irq = 1u;
        }
    }

    if (want_irq) {
        audio_raise_irq(vm, audio, LAMP_AUDIO_IRQ_COMPLETION);
    }
}

int audio_device_init(VM *vm) {
    audio_state_t *audio;
    PciFunction *function;
    if (!vm || !vm->pcie || vm->audio) {
        return -1;
    }
    audio = calloc(1, sizeof(*audio));
    if (!audio) {
        return -1;
    }
    function = pci_register_function(vm, 3u, 0u, LAMP_PCI_VENDOR_ID,
                                     LAMP_PCI_AUDIO_DEVICE_ID,
                                     PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, 0u);
    if (!function) {
        free(audio);
        return -1;
    }
    audio->pci_function = function;
    audio_reset(audio);
    pci_configure_bar(vm, function, 0u, LAMP_AUDIO_BAR0_SIZE, 0u, 0u,
                      audio_bar_read32, audio_bar_write32, NULL, audio);
    (void)pci_add_pm_capability(function);
    (void)pci_add_msi_capability(function);
    (void)pci_add_express_capability(function, 0u);
    pci_set_irq_pin(function, 1u, INT_AUDIO);
    vm->audio = audio;
    return 0;
}

int audio_host_start(VM *vm) {
    audio_state_t *audio = vm ? (audio_state_t *)vm->audio : NULL;
    SDL_AudioSpec spec;
    if (!audio || audio->stream) {
        return audio && audio->stream ? 0 : -1;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        audio_sync_backend_status(audio);
        return -1;
    }
    audio->sdl_audio_initialized = 1u;
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = (int)LAMP_AUDIO_CHANNELS;
    spec.freq = (int)LAMP_AUDIO_RATE;
    audio->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                               &spec, NULL, NULL);
    if (!audio->stream || !SDL_ResumeAudioStreamDevice(audio->stream)) {
        if (audio->stream) {
            SDL_DestroyAudioStream(audio->stream);
        }
        audio->stream = NULL;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio->sdl_audio_initialized = 0u;
        audio_sync_backend_status(audio);
        return -1;
    }
    audio_sync_backend_status(audio);
    VM_RUNTIME_LOG("SDL3 audio ready: 48000 Hz S16LE stereo\n");
    return 0;
}

void audio_host_stop(VM *vm) {
    audio_state_t *audio = vm ? (audio_state_t *)vm->audio : NULL;
    if (!audio) {
        return;
    }
    if (audio->stream) {
        SDL_DestroyAudioStream(audio->stream);
    }
    audio->stream = NULL;
    if (audio->sdl_audio_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio->sdl_audio_initialized = 0u;
    }
    audio_sync_backend_status(audio);
}

void audio_device_shutdown(VM *vm) {
    audio_state_t *audio;
    if (!vm || !(audio = (audio_state_t *)vm->audio)) {
        return;
    }
    audio_host_stop(vm);
    free(audio);
    vm->audio = NULL;
}

# PCI Audio and DMA Rings

## Device v1

The Lamp PCM endpoint is PCI function `00:03.0`, vendor/device `1DB7:3000`,
class `04:01:00`. It has a 4 KiB BAR0, PM/MSI/PCIe capabilities, and one fixed
playback format:

- 48,000 sample frames per second;
- signed 16-bit little-endian samples;
- two interleaved channels;
- four bytes per stereo frame;
- at most 65,536 bytes per descriptor.

BAR0 reports magic `LPA1`, version 1, fixed format fields, status and
capabilities. It also contains submission-ring and completion-ring base/count/
head/tail registers, `ENABLE`/`DISABLE`/`RESET`, interrupt status/enable/ack,
queued-byte telemetry, and a completed-descriptor counter. Exact offsets and
bits are shared in `include/lampvm/device_abi.h`.

## Reusable DMA ring ABI

Both rings have a power-of-two entry count from 2 through 256 and leave one
slot empty to distinguish full from empty. Ring memory and PCM buffers are
IOVAs in the audio device's IOMMU domain (`IOMMU_DEV_AUDIO = 2`).

Each 32-byte submission descriptor contains:

| Field | Meaning |
|---|---|
| `addr_lo`, `addr_hi` | PCM buffer IOVA |
| `length` | Byte count, nonzero and a multiple of four |
| `flags` | `IRQ` and/or `END` |
| `cookie` | Opaque value returned in the completion |
| three reserved words | Must be zero |

Each 16-byte completion contains the cookie, status, consumed byte count, and
a monotonically increasing device sequence. Completion status is one of
`OK`, `BAD_DESC`, `DMA_FAULT`, or `BACKEND_ERROR`.

Ownership follows the usual producer/consumer convention:

1. The guest fills a submission entry, advances `SUBMIT_TAIL`, and writes the
   new tail to BAR0.
2. The device reads through the IOMMU, consumes entries up to that tail, and
   advances `SUBMIT_HEAD`.
3. The device writes completion entries and advances `COMPLETE_TAIL`.
4. The guest reaps completions, advances `COMPLETE_HEAD`, then acknowledges
   the MSI status bit.

The host implementation in `src/io_devices/dma/` supplies reusable ring
validation, IOMMU descriptor access, ownership transitions, and completion
publication. The guest implementation in `kernel/src/dma_ring.c` supplies
reusable IOVA setup, producer full checks, submission, and completion reaping.

## SDL3 backend

Valid PCM payloads are copied into an SDL3 audio stream opened as S16LE,
stereo, 48 kHz. SDL owns any conversion needed by the selected host device.
Audio and video initialize independent SDL subsystems, so closing the display
does not accidentally tear down audio first. This is the same SDL3 path on
Linux and macOS; there is no CoreAudio-specific renderer or driver code.

The host applies bounded queue backpressure at roughly one second of PCM. If
SDL audio cannot be opened (including a deliberately headless run), the
device advertises `BACKEND_UNAVAILABLE` but operates as a null sink and still
returns successful completions. This prevents a guest playback queue from
deadlocking solely because the host lacks an audio device.

## Kernel driver

`kernel/src/audio.c` validates the PCI device and format, allocates 16-entry
submission/completion rings, maps them through the paged IOMMU domain, enables
the device, and reaps MSI completions. `audio_submit_pcm()` submits a caller-
owned interleaved stereo buffer; that buffer must remain valid until completion.

The initial driver deliberately does not play a boot tone and does not yet
expose a userspace `/dev/audio` API. Capture, runtime format negotiation,
mixing, volume, underrun events, scatter/gather payloads, and power-management
state transitions remain future work.

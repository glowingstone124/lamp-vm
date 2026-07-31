#ifndef VM_ETHER_H
#define VM_ETHER_H

#include <stdint.h>

/* MMIO register offsets from ETHER_MMIO_BASE */
enum {
    ETHER_OFF_TX_LEN  = 0x00,   /* W: write length to trigger TX DMA */
    ETHER_OFF_TX_LO   = 0x04,   /* R/W: TX buffer phys addr low */
    ETHER_OFF_RX_LEN  = 0x08,   /* R: pending RX length (0 = none) */
    ETHER_OFF_RX_LO   = 0x0C,   /* R/W: RX buffer phys addr low */
    ETHER_OFF_STATUS  = 0x10,   /* R: bit0=link bit1=rx_ready */
    ETHER_OFF_MAC_LO  = 0x14,   /* R: MAC address low 32 bits */
    ETHER_OFF_MAC_HI  = 0x18,   /* R: MAC address high 16 bits */
    ETHER_MMIO_SIZE   = 0x1C
};

/* STATUS register bits */
#define ETHER_STATUS_LINK      0x01u
#define ETHER_STATUS_RX_READY  0x02u

/* Default MAC: 02:00:00:00:00:01 (locally administered unicast) */
#define ETHER_MAC_BYTE0 0x02u
#define ETHER_MAC_BYTE1 0x00u
#define ETHER_MAC_BYTE2 0x00u
#define ETHER_MAC_BYTE3 0x00u
#define ETHER_MAC_BYTE4 0x00u
#define ETHER_MAC_BYTE5 0x01u

#define ETHER_MTU 2048u

/* Lamp virtual PCI Ethernet endpoint at 00:01.0. */
#define ETHER_PCI_DEVICE_ID 0x1000u
#define ETHER_PCI_BAR_SIZE  0x1000u

/* Forward declaration */
typedef struct VM VM;
typedef struct ether_backend ether_backend_t;

/* Public API called from vm.c */
int  ether_init(VM *vm, ether_backend_t *backend);
void ether_shutdown(VM *vm);
void ether_poll(VM *vm);

#endif

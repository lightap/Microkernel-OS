#ifndef VIRTIO_H
#define VIRTIO_H

#include "types.h"
#include "pci.h"

/* ===== VirtIO PCI Vendor/Device IDs ===== */
#define VIRTIO_PCI_VENDOR       0x1AF4
/* Modern (non-transitional) device IDs: 0x1040 + device_type */
#define VIRTIO_PCI_DEV_GPU      0x1050  /* type 16 */
/* Transitional device IDs: 0x1000 - 0x103F */
#define VIRTIO_PCI_DEV_TRANS_LO 0x1000
#define VIRTIO_PCI_DEV_TRANS_HI 0x103F

/* ===== VirtIO PCI Capability Types ===== */
#define VIRTIO_PCI_CAP_COMMON   1   /* Common configuration */
#define VIRTIO_PCI_CAP_NOTIFY   2   /* Notifications */
#define VIRTIO_PCI_CAP_ISR      3   /* ISR access */
#define VIRTIO_PCI_CAP_DEVICE   4   /* Device-specific config */
#define VIRTIO_PCI_CAP_PCI_CFG  5   /* PCI config access */

/* ===== VirtIO Device Status Bits ===== */
#define VIRTIO_STATUS_RESET         0
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FAILED        128

/* ===== Virtqueue Descriptor Flags ===== */
#define VRING_DESC_F_NEXT       1   /* Descriptor continues via 'next' */
#define VRING_DESC_F_WRITE      2   /* Device writes (vs reads) */

/* ===== Virtqueue Sizes ===== */
#define VIRTQ_MAX_SIZE  256

/* VirtIO 1.0+ Feature Bits (from spec §2.2) */
#define VIRTIO_F_INDIRECT_DESC      (1 << 28)   // Indirect descriptors supported
#define VIRTIO_F_EVENT_IDX          (1 << 29)   // Use event index
#define VIRTIO_F_VERSION_1          (1 << 32)   // Modern VirtIO 1.0+ device

/* Example: features we want to negotiate (add more as needed) */
#define WANTED_FEATURES ((1ULL << VIRTIO_F_INDIRECT_DESC) | \
                         (1ULL << VIRTIO_F_EVENT_IDX)     | \
                         (1ULL << VIRTIO_F_VERSION_1))


/* VirtIO standard feature bits we want to accept (add more as needed) */
#define WANTED_FEATURES_LO ( \
    (1U << 28) | /* VIRTIO_F_INDIRECT_DESC */ \
    (1U << 29)   /* VIRTIO_F_EVENT_IDX */     \
)

#define WANTED_FEATURES_HI ( \
    (1U << 0)    /* VIRTIO_F_VERSION_1 = bit 32 */ \
)
/* ===== Virtqueue Descriptor ===== */
typedef struct {
    uint64_t addr;      /* Physical address of buffer */
    uint32_t len;       /* Length of buffer */
    uint16_t flags;     /* VRING_DESC_F_* */
    uint16_t next;      /* Next descriptor index if flags & NEXT */
} __attribute__((packed)) virtq_desc_t;

/* ===== Virtqueue Available Ring ===== */
typedef struct {
    uint16_t flags;
    uint16_t idx;       /* Where driver puts next entry (mod queue_size) */
    uint16_t ring[];    /* Array of descriptor chain heads */
} __attribute__((packed)) virtq_avail_t;

/* ===== Virtqueue Used Ring Element ===== */
typedef struct {
    uint32_t id;        /* Index of start of used descriptor chain */
    uint32_t len;       /* Total bytes written by device */
} __attribute__((packed)) virtq_used_elem_t;

/* ===== Virtqueue Used Ring ===== */
typedef struct {
    uint16_t flags;
    uint16_t idx;       /* Where device puts next entry (mod queue_size) */
    virtq_used_elem_t ring[];
} __attribute__((packed)) virtq_used_t;

/* ===== Virtqueue (driver-side bookkeeping) ===== */
typedef struct {
    uint16_t        size;           /* Number of descriptors */
    uint16_t        free_head;      /* Head of free descriptor list */
    uint16_t        num_free;       /* Number of free descriptors */
    uint16_t        last_used_idx;  /* Last seen used->idx */

    virtq_desc_t*   desc;           /* Descriptor table (phys == virt, identity mapped) */
    virtq_avail_t*  avail;          /* Available ring */
    virtq_used_t*   used;           /* Used ring */

    /* Physical addresses for device programming */
    uint32_t        desc_phys;
    uint32_t        avail_phys;
    uint32_t        used_phys;

    /* Notify offset for this queue (multiplied by notify_off_multiplier) */
    uint32_t        notify_offset;
} virtq_t;

/* ===== VirtIO Common Configuration (MMIO layout) ===== */
/* Offsets into the common config structure */
#define VIRTIO_COMMON_DFSELECT      0x00    /* uint32 - device feature select */
#define VIRTIO_COMMON_DF            0x04    /* uint32 - device feature bits */
#define VIRTIO_COMMON_GFSELECT      0x08    /* uint32 - guest feature select */
#define VIRTIO_COMMON_GF            0x0C    /* uint32 - guest feature bits */
#define VIRTIO_COMMON_MSIX          0x10    /* uint16 - MSI-X config vector */
#define VIRTIO_COMMON_NUMQ          0x12    /* uint16 - number of queues */
#define VIRTIO_COMMON_STATUS        0x14    /* uint8  - device status */
#define VIRTIO_COMMON_CFGGEN        0x15    /* uint8  - config generation */
#define VIRTIO_COMMON_QSELECT       0x16    /* uint16 - queue select */
#define VIRTIO_COMMON_QSIZE         0x18    /* uint16 - queue size */
#define VIRTIO_COMMON_QMSIX         0x1A    /* uint16 - queue MSI-X vector */
#define VIRTIO_COMMON_QENABLE       0x1C    /* uint16 - queue enable */
#define VIRTIO_COMMON_QNOTIFYOFF    0x1E    /* uint16 - queue notify offset */
#define VIRTIO_COMMON_QDESC_LO      0x20    /* uint32 - descriptor table addr lo */
#define VIRTIO_COMMON_QDESC_HI      0x24    /* uint32 - descriptor table addr hi */
#define VIRTIO_COMMON_QAVAIL_LO     0x28    /* uint32 - available ring addr lo */
#define VIRTIO_COMMON_QAVAIL_HI     0x2C    /* uint32 - available ring addr hi */
#define VIRTIO_COMMON_QUSED_LO      0x30    /* uint32 - used ring addr lo */
#define VIRTIO_COMMON_QUSED_HI      0x34    /* uint32 - used ring addr hi */

/* ===== VirtIO Device State ===== */
typedef struct {
    pci_device_t*   pci;            /* PCI device handle */

    /* MMIO virtual addresses for each capability region */
    volatile uint8_t* common_cfg;   /* Common configuration */
    volatile uint8_t* notify_base;  /* Notification base */
    volatile uint8_t* isr_cfg;      /* ISR status */
    volatile uint8_t* device_cfg;   /* Device-specific configuration */

    uint32_t        notify_off_mul; /* Notification offset multiplier */
    uint8_t         irq;            /* IRQ line */

    /* Virtqueues */
    virtq_t         queues[4];      /* Up to 4 queues */
    uint16_t        num_queues;
} virtio_dev_t;

/* ===== VirtIO Transport API ===== */

/* Find and initialize a VirtIO device by PCI device ID.
 * Returns true on success, fills out dev structure. */
bool virtio_init(virtio_dev_t* dev, uint16_t pci_device_id);

/* Initialize with a specific PCI device pointer (for multi-device scenarios) */
bool virtio_init_pci(virtio_dev_t* dev, pci_device_t* pci);

/* Set up a specific virtqueue (allocates memory, programs device) */
bool virtio_setup_queue(virtio_dev_t* dev, uint16_t queue_idx);

/* Allocate a descriptor chain and submit a request.
 * out_sg/in_sg: scatter-gather arrays of {physical_addr, length} pairs.
 * Returns descriptor head index, or -1 on failure. */
int virtio_send(virtio_dev_t* dev, uint16_t queue_idx,
                uint32_t out_addr, uint32_t out_len,
                uint32_t in_addr, uint32_t in_len);

/* Notify the device about new available buffers */
void virtio_notify(virtio_dev_t* dev, uint16_t queue_idx);

/* Poll for completed requests. Returns true if something completed. */
bool virtio_poll(virtio_dev_t* dev, uint16_t queue_idx);

/* Wait (busy-poll) until queue has a used entry */
void virtio_wait(virtio_dev_t* dev, uint16_t queue_idx);

/* Read/write ISR status (clears interrupt) */
uint8_t virtio_isr_status(virtio_dev_t* dev);

/* Finalize driver: set DRIVER_OK */
void virtio_driver_ok(virtio_dev_t* dev);

/* Get number of available queues */
uint16_t virtio_num_queues(virtio_dev_t* dev);


bool virtio_init_features(virtio_dev_t* dev, uint16_t pci_device_id,
                          uint32_t wanted_features);
#endif

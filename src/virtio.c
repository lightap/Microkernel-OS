#include "virtio.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "vga.h"
#include "serial.h"
#include "heap.h"

/*
 * VirtIO Modern PCI Transport Implementation
 *
 * This implements the VirtIO 1.0+ PCI transport layer which uses
 * PCI capabilities to locate MMIO configuration regions. This is
 * required for modern-only devices like virtio-gpu.
 *
 * Memory model: The kernel uses identity mapping (phys == virt)
 * for all RAM allocated through PMM, which simplifies virtqueue
 * address translation.
 */

/* Virtual address region for mapping VirtIO MMIO BARs */
//#define VIRTIO_MMIO_VBASE   0x24000000  /* Above the GUI FB region */
#define VIRTIO_MMIO_VBASE 0x30000000
/* PCI capability header ID for vendor-specific */
#define PCI_CAP_VENDOR      0x09

/* Track how much MMIO space we've mapped */
static uint32_t mmio_next_vaddr = VIRTIO_MMIO_VBASE;

/* ===== MMIO Read/Write Helpers ===== */

static inline uint8_t mmio_read8(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint8_t*)(base + off);
}
static inline uint16_t mmio_read16(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint16_t*)(base + off);
}
static inline uint32_t mmio_read32(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}
static inline void mmio_write8(volatile uint8_t* base, uint32_t off, uint8_t val) {
    *(volatile uint8_t*)(base + off) = val;
}
static inline void mmio_write16(volatile uint8_t* base, uint32_t off, uint16_t val) {
    *(volatile uint16_t*)(base + off) = val;
}
static inline void mmio_write32(volatile uint8_t* base, uint32_t off, uint32_t val) {
    *(volatile uint32_t*)(base + off) = val;
}

/* ===== Map a PCI BAR region into virtual memory ===== */
static volatile uint8_t* map_bar_region(pci_device_t* pci, uint8_t bar_idx,
                                         uint32_t offset, uint32_t min_size) {
    uint32_t bar_val = pci->bar[bar_idx];

    /* Memory BAR (not I/O) */
    if (bar_val & 1) {
        serial_printf("virtio: BAR%d is I/O, expected MMIO\n", bar_idx);
        return NULL;
    }

    uint32_t bar_phys = bar_val & 0xFFFFFFF0;
    if (bar_phys == 0) {
        serial_printf("virtio: BAR%d is zero\n", bar_idx);
        return NULL;
    }

    /* Map the BAR region into our MMIO virtual address space */
    uint32_t map_phys = (bar_phys + offset) & ~0xFFF;  /* Page-align down */
    uint32_t page_offset = (bar_phys + offset) & 0xFFF;
    uint32_t map_size = ((min_size + page_offset) + 0xFFF) & ~0xFFF;

    uint32_t vaddr = mmio_next_vaddr;
//    paging_map_range(vaddr, map_phys, map_size, PAGE_PRESENT | PAGE_WRITE);
//    paging_map_range(vaddr, map_phys, map_size, PAGE_PRESENT | PAGE_WRITE | PAGE_CACHE_DISABLE);
   // mmio_next_vaddr += map_size;
  
paging_map_range(vaddr, map_phys, map_size, PAGE_PRESENT | PAGE_WRITE | PAGE_CACHE_DISABLE);

if (vaddr) {
    serial_printf("Mapped %u bytes at v=%x (phys=%x)\n", map_size, vaddr, map_phys);

    volatile uint8_t* base = (volatile uint8_t*)vaddr;
    uint8_t read_val = base[0];
    serial_printf("  Test read[0] = %02x\n", read_val);

    base[0] = 0xAA;                // test write
    uint8_t write_back = base[0];
    serial_printf("  Test write/read-back = %02x\n", write_back);
}
__asm__ volatile ("invlpg (%0)" ::"r"(vaddr) : "memory");
__asm__ volatile ("invlpg (%0)" ::"r"((char*)vaddr + map_size - 1) : "memory");
    /* Flush TLB for the new mapping */
//__asm__ volatile ("invlpg (%0)" ::"r"(vaddr) : "memory");
mmio_next_vaddr += map_size + 0x10000;
    return (volatile uint8_t*)(vaddr + page_offset);
}

/* ===== Read PCI Config Space (byte-level for capability walking) ===== */
static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read(bus, slot, func, offset & 0xFC);
    return (val >> ((offset & 3) * 8)) & 0xFF;
}

/* ===== Walk PCI Capabilities and Find VirtIO Structures ===== */
static bool parse_capabilities(virtio_dev_t* dev) {
    pci_device_t* pci = dev->pci;

    /* Check if device has capabilities (Status register bit 4) */
    uint32_t status_cmd = pci_read(pci->bus, pci->slot, pci->func, 0x04);
    uint16_t status = (status_cmd >> 16) & 0xFFFF;
    if (!(status & (1 << 4))) {
        serial_printf("virtio: PCI device has no capabilities\n");
        return false;
    }

    /* Get capabilities pointer (offset 0x34) */
    uint8_t cap_ptr = pci_read8(pci->bus, pci->slot, pci->func, 0x34);
    cap_ptr &= 0xFC;  /* Must be dword-aligned */

    bool found_common = false, found_notify = false;
    bool found_isr = false, found_device = false;
    (void)found_device;  /* device_cfg is optional */

    while (cap_ptr != 0) {
        uint8_t cap_id = pci_read8(pci->bus, pci->slot, pci->func, cap_ptr);
        uint8_t cap_next = pci_read8(pci->bus, pci->slot, pci->func, cap_ptr + 1);

        if (cap_id == PCI_CAP_VENDOR) {
            /* VirtIO PCI capability structure:
             *   +0: cap_id (0x09)
             *   +1: cap_next
             *   +2: cap_len
             *   +3: cfg_type (VIRTIO_PCI_CAP_*)
             *   +4: bar
             *   +5: padding[3]
             *   +8: offset (uint32)
             *   +12: length (uint32)
             *   For notify cap, +16: notify_off_multiplier (uint32)
             */
            uint8_t cfg_type = pci_read8(pci->bus, pci->slot, pci->func, cap_ptr + 3);
            uint8_t bar_idx  = pci_read8(pci->bus, pci->slot, pci->func, cap_ptr + 4);
            uint32_t offset  = pci_read(pci->bus, pci->slot, pci->func, cap_ptr + 8);
            uint32_t length  = pci_read(pci->bus, pci->slot, pci->func, cap_ptr + 12);

            serial_printf("virtio: cap type=%d bar=%d off=%x len=%x\n",
                          cfg_type, bar_idx, offset, length);

            switch (cfg_type) {
              /*  case VIRTIO_PCI_CAP_COMMON:
                    dev->common_cfg = map_bar_region(pci, bar_idx, offset, length);
                    found_common = (dev->common_cfg != NULL);
                    break;

                case VIRTIO_PCI_CAP_NOTIFY:
                    dev->notify_base = map_bar_region(pci, bar_idx, offset, length);
                    dev->notify_off_mul = pci_read(pci->bus, pci->slot, pci->func,
                                                    cap_ptr + 16);
                    found_notify = (dev->notify_base != NULL);
                    serial_printf("virtio: notify_off_multiplier=%u\n", dev->notify_off_mul);
                    break;
*/

case VIRTIO_PCI_CAP_COMMON:
    dev->common_cfg = map_bar_region(pci, bar_idx, offset, length);
    if (!dev->common_cfg) {
        serial_printf("ERROR: common_cfg mapping FAILED (off=%x len=%x)\n", offset, length);
        return false;
    }
    serial_printf("common_cfg mapped OK at %p (expecting %x bytes)\n", dev->common_cfg, length);
    found_common = true;
    break;

case VIRTIO_PCI_CAP_NOTIFY:
    dev->notify_base = map_bar_region(pci, bar_idx, offset, length);
    if (!dev->notify_base) {
        serial_printf("ERROR: notify_base mapping FAILED\n");
        return false;
    }
    dev->notify_off_mul = pci_read(pci->bus, pci->slot, pci->func, cap_ptr + 16);
    serial_printf("notify OK at %p mul=%u\n", dev->notify_base, dev->notify_off_mul);
    found_notify = true;
    break;
            }
        }

        cap_ptr = cap_next & 0xFC;
    }

    if (!found_common) serial_printf("virtio: WARNING - no common cfg cap\n");
 //   if (!found_notify) serial_printf("virtio: WARNING - no notify cap\n");
    if (!found_isr)    serial_printf("virtio: WARNING - no ISR cap\n");

    return found_common /*&& found_notify*/;
}

/* ===== Enable PCI Bus Mastering ===== */
static void pci_enable_bus_master(pci_device_t* pci) {
    uint32_t cmd = pci_read(pci->bus, pci->slot, pci->func, 0x04);
    cmd |= (1 << 2);   /* Bus Master Enable */
    cmd |= (1 << 1);   /* Memory Space Enable */
    pci_write(pci->bus, pci->slot, pci->func, 0x04, cmd);
}

/* ===== Initialize VirtIO Device ===== */
bool virtio_init(virtio_dev_t* dev, uint16_t pci_device_id) {
    memset(dev, 0, sizeof(*dev));

    /* Find the PCI device */
    dev->pci = pci_find_device(VIRTIO_PCI_VENDOR, pci_device_id);
    if (!dev->pci) {
        /* Also try transitional IDs for GPU (subsystem device = 16) */
        for (uint16_t id = VIRTIO_PCI_DEV_TRANS_LO; id <= VIRTIO_PCI_DEV_TRANS_HI; id++) {
            dev->pci = pci_find_device(VIRTIO_PCI_VENDOR, id);
            if (dev->pci) {
                /* Check subsystem ID to verify it's a GPU */
                uint32_t subsys = pci_read(dev->pci->bus, dev->pci->slot,
                                           dev->pci->func, 0x2C);
                uint16_t subsys_dev = (subsys >> 16) & 0xFFFF;
                if (subsys_dev == 16) break;  /* GPU type */
                dev->pci = NULL;
            }
        }
        if (!dev->pci) {
            serial_printf("virtio: device %x not found on PCI\n", pci_device_id);
            return false;
        }
    }

    serial_printf("virtio: found device %x:%x at %d:%d.%d\n",
                  dev->pci->vendor_id, dev->pci->device_id,
                  dev->pci->bus, dev->pci->slot, dev->pci->func);

    dev->irq = dev->pci->irq_line;

    /* Enable bus mastering for DMA */
    pci_enable_bus_master(dev->pci);

    /* Parse PCI capabilities to find MMIO regions */
    if (!parse_capabilities(dev)) {
        serial_printf("virtio: failed to parse PCI capabilities\n");
        return false;
    }

    /* === Device Initialization Sequence (VirtIO 1.0 spec §3.1.1) === */

    /* 1. Reset the device */
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);

    /* Small delay to let device reset */
    for (volatile int i = 0; i < 10000; i++) {}

    /* 2. Set ACKNOWLEDGE status bit */
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* 3. Set DRIVER status bit */
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 4. Negotiate features */
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_DFSELECT, 0);
    uint32_t features_lo = mmio_read32(dev->common_cfg, VIRTIO_COMMON_DF);
    serial_printf("virtio: device features[0-31]=%08x\n", features_lo);

    mmio_write32(dev->common_cfg, VIRTIO_COMMON_DFSELECT, 1);
    uint32_t features_hi = mmio_read32(dev->common_cfg, VIRTIO_COMMON_DF);
    serial_printf("virtio: device features[32-63]=%08x\n", features_hi);

    /* Accept only wanted features */
    uint32_t accepted_lo = features_lo & WANTED_FEATURES_LO;
    uint32_t accepted_hi = features_hi & WANTED_FEATURES_HI;

    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GFSELECT, 0);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, accepted_lo);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GFSELECT, 1);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, accepted_hi);

    serial_printf("virtio: accepted features[0-31]=%08x  [32-63]=%08x\n",
                  accepted_lo, accepted_hi);

    /* Accept VERSION_1 (bit 0 of high word = feature bit 32) if offered */
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GFSELECT, 1);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, features_hi & 1);

    serial_printf("virtio: device features[1]=%x\n", features_hi);

    /* Accept VERSION_1 if offered (bit 0 of word 1 = feature bit 32) */
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GFSELECT, 1);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, features_hi & 1);

    /* 5. Set FEATURES_OK */
    uint8_t s = mmio_read8(dev->common_cfg, VIRTIO_COMMON_STATUS);
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, s | VIRTIO_STATUS_FEATURES_OK);

    /* 6. Verify FEATURES_OK is still set */
    s = mmio_read8(dev->common_cfg, VIRTIO_COMMON_STATUS);
    if (!(s & VIRTIO_STATUS_FEATURES_OK)) {
        serial_printf("virtio: device rejected features\n");
        mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    /* Read number of queues */
    dev->num_queues = mmio_read16(dev->common_cfg, VIRTIO_COMMON_NUMQ);
    serial_printf("virtio: device has %u queues\n", dev->num_queues);

    /* Disable MSI-X (use legacy IRQ) */
    mmio_write16(dev->common_cfg, VIRTIO_COMMON_MSIX, 0xFFFF);

    return true;
}

/* Initialize with a specific PCI device (for when multiple devices share the same ID) */
bool virtio_init_pci(virtio_dev_t* dev, pci_device_t* pci) {
    memset(dev, 0, sizeof(*dev));
    dev->pci = pci;

    serial_printf("virtio: init_pci device %x:%x at %d:%d.%d\n",
                  pci->vendor_id, pci->device_id,
                  pci->bus, pci->slot, pci->func);

    dev->irq = pci->irq_line;

    /* Enable bus mastering for DMA */
    pci_enable_bus_master(pci);

    /* Parse PCI capabilities to find MMIO regions */
    if (!parse_capabilities(dev)) {
        serial_printf("virtio: failed to parse PCI capabilities\n");
        return false;
    }

    /* === Device Initialization Sequence (VirtIO 1.0 spec §3.1.1) === */

    /* 1. Reset the device */
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
    for (volatile int i = 0; i < 10000; i++) {}

    /* 2. Set ACKNOWLEDGE status bit */
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* 3. Set DRIVER status bit */
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 4. Negotiate features — only accept VERSION_1 */
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_DFSELECT, 0);
    uint32_t features_lo = mmio_read32(dev->common_cfg, VIRTIO_COMMON_DF);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_DFSELECT, 1);
    uint32_t features_hi = mmio_read32(dev->common_cfg, VIRTIO_COMMON_DF);

    serial_printf("virtio: features[lo]=%08x [hi]=%08x\n", features_lo, features_hi);

    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GFSELECT, 0);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, 0);  /* No low features needed */
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GFSELECT, 1);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, features_hi & 1);  /* VERSION_1 */

    /* 5. Set FEATURES_OK */
    uint8_t s = mmio_read8(dev->common_cfg, VIRTIO_COMMON_STATUS);
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, s | VIRTIO_STATUS_FEATURES_OK);

    /* 6. Verify FEATURES_OK is still set */
    s = mmio_read8(dev->common_cfg, VIRTIO_COMMON_STATUS);
    if (!(s & VIRTIO_STATUS_FEATURES_OK)) {
        serial_printf("virtio: device rejected features\n");
        mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    /* Read number of queues */
    dev->num_queues = mmio_read16(dev->common_cfg, VIRTIO_COMMON_NUMQ);
    serial_printf("virtio: device has %u queues\n", dev->num_queues);

    /* Disable MSI-X (use legacy IRQ) */
    mmio_write16(dev->common_cfg, VIRTIO_COMMON_MSIX, 0xFFFF);

    return true;
}

/* ===== Set Up a Virtqueue ===== */
bool virtio_setup_queue(virtio_dev_t* dev, uint16_t queue_idx) {
    if (queue_idx >= dev->num_queues || queue_idx >= 4) return false;

    /* Select the queue */
    mmio_write16(dev->common_cfg, VIRTIO_COMMON_QSELECT, queue_idx);

    /* Read maximum queue size */
    uint16_t max_size = mmio_read16(dev->common_cfg, VIRTIO_COMMON_QSIZE);
    if (max_size == 0) {
        serial_printf("virtio: queue %u not available\n", queue_idx);
        return false;
    }

    /* Use a reasonable size (power of 2, at most VIRTQ_MAX_SIZE) */
    uint16_t qsize = max_size;
    if (qsize > VIRTQ_MAX_SIZE) qsize = VIRTQ_MAX_SIZE;

    /* Write chosen queue size */
    mmio_write16(dev->common_cfg, VIRTIO_COMMON_QSIZE, qsize);

    serial_printf("virtio: setting up queue %u, size=%u\n", queue_idx, qsize);

    virtq_t* vq = &dev->queues[queue_idx];
    vq->size = qsize;

    /*
     * Allocate memory for the virtqueue structures.
     * We need:
     *   - Descriptor table: 16 bytes * qsize
     *   - Available ring:   6 + 2 * qsize bytes
     *   - Used ring:        6 + 8 * qsize bytes
     *
     * All must be physically contiguous and page-aligned.
     * Since our kernel is identity-mapped, PMM pages work directly.
     */
    uint32_t desc_bytes  = sizeof(virtq_desc_t) * qsize;
    uint32_t avail_bytes = 6 + 2 * qsize;
    uint32_t used_bytes  = 6 + 8 * qsize;
    uint32_t total_bytes = desc_bytes + avail_bytes + used_bytes;
    uint32_t num_pages   = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Allocate contiguous pages */
    uint8_t* base = NULL;
    if (num_pages == 1) {
        base = (uint8_t*)pmm_alloc_page();
    } else {
        /* Allocate multiple pages - try to get contiguous ones */
        base = (uint8_t*)pmm_alloc_page();
        if (base) {
            bool contiguous = true;
            uint32_t base_addr = (uint32_t)base;
            for (uint32_t i = 1; i < num_pages; i++) {
                void* p = pmm_alloc_page();
                if ((uint32_t)p != base_addr + i * PAGE_SIZE) {
                    /* Not contiguous - free and try with kmalloc */
                    contiguous = false;
                    pmm_free_page(p);
                    break;
                }
            }
            if (!contiguous) {
                /* Free first page, fall back to large kmalloc (may not be aligned) */
                pmm_free_page(base);
                base = (uint8_t*)kmalloc(total_bytes + PAGE_SIZE);
                if (base) {
                    /* Page-align */
                    base = (uint8_t*)(((uint32_t)base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
                }
            }
        }
    }

    if (!base) {
        serial_printf("virtio: failed to allocate memory for queue %u\n", queue_idx);
        return false;
    }

    memset(base, 0, total_bytes);

    /* Lay out structures in allocated memory */
    vq->desc  = (virtq_desc_t*)base;
    vq->avail = (virtq_avail_t*)(base + desc_bytes);
    vq->used  = (virtq_used_t*)(base + desc_bytes + avail_bytes);

    /* Physical addresses (identity mapped) */
    vq->desc_phys  = (uint32_t)vq->desc;
    vq->avail_phys = (uint32_t)vq->avail;
    vq->used_phys  = (uint32_t)vq->used;

    /* Initialize free descriptor chain */
    for (uint16_t i = 0; i < qsize - 1; i++) {
        vq->desc[i].next = i + 1;
    }
    vq->desc[qsize - 1].next = 0xFFFF;
    vq->free_head = 0;
    vq->num_free = qsize;
    vq->last_used_idx = 0;

    /* Tell the device where our queue structures are */
    mmio_write16(dev->common_cfg, VIRTIO_COMMON_QSELECT, queue_idx);

    mmio_write32(dev->common_cfg, VIRTIO_COMMON_QDESC_LO, vq->desc_phys);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_QDESC_HI, 0);

    mmio_write32(dev->common_cfg, VIRTIO_COMMON_QAVAIL_LO, vq->avail_phys);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_QAVAIL_HI, 0);

    mmio_write32(dev->common_cfg, VIRTIO_COMMON_QUSED_LO, vq->used_phys);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_QUSED_HI, 0);

    /* Disable MSI-X for this queue */
    mmio_write16(dev->common_cfg, VIRTIO_COMMON_QMSIX, 0xFFFF);

    /* Read the notification offset for this queue */
    vq->notify_offset = mmio_read16(dev->common_cfg, VIRTIO_COMMON_QNOTIFYOFF);

    /* Enable the queue */
    mmio_write16(dev->common_cfg, VIRTIO_COMMON_QENABLE, 1);

    serial_printf("virtio: queue %u ready (desc=%x avail=%x used=%x notify_off=%u)\n",
                  queue_idx, vq->desc_phys, vq->avail_phys, vq->used_phys,
                  vq->notify_offset);

    return true;
}

/* ===== Submit a Request (out buffer + in buffer) ===== */
int virtio_send(virtio_dev_t* dev, uint16_t queue_idx,
                uint32_t out_addr, uint32_t out_len,
                uint32_t in_addr, uint32_t in_len) {
    virtq_t* vq = &dev->queues[queue_idx];

    /* Need at least 2 descriptors (out + in), or 1 if in_len == 0 */
    uint16_t needed = (in_len > 0) ? 2 : 1;
    if (vq->num_free < needed) {
        serial_printf("virtio: queue %u full (%u free, need %u)\n",
                      queue_idx, vq->num_free, needed);
        return -1;
    }

    /* Allocate output descriptor */
    uint16_t head = vq->free_head;
    uint16_t idx = head;

    vq->desc[idx].addr  = (uint64_t)out_addr;
    vq->desc[idx].len   = out_len;
    vq->desc[idx].flags = (in_len > 0) ? VRING_DESC_F_NEXT : 0;

    if (in_len > 0) {
        uint16_t next_idx = vq->desc[idx].next;
        vq->desc[next_idx].addr  = (uint64_t)in_addr;
        vq->desc[next_idx].len   = in_len;
        vq->desc[next_idx].flags = VRING_DESC_F_WRITE;  /* Device writes to this */

        vq->free_head = vq->desc[next_idx].next;
        vq->num_free -= 2;
    } else {
        vq->free_head = vq->desc[idx].next;
        vq->num_free -= 1;
    }

    /* Add to available ring */
    uint16_t avail_idx = vq->avail->idx % vq->size;
    vq->avail->ring[avail_idx] = head;

    /* Memory barrier before updating avail->idx */
    __asm__ volatile ("mfence" ::: "memory");

    vq->avail->idx++;

    return (int)head;
}

/* ===== Notify Device ===== */
void virtio_notify(virtio_dev_t* dev, uint16_t queue_idx) {
    virtq_t* vq = &dev->queues[queue_idx];
    uint32_t offset = vq->notify_offset * dev->notify_off_mul;
    mmio_write16(dev->notify_base, offset, queue_idx);
}

/* ===== Poll for Completion ===== */
bool virtio_poll(virtio_dev_t* dev, uint16_t queue_idx) {
    virtq_t* vq = &dev->queues[queue_idx];

    /* Memory barrier to see device's writes */
    __asm__ volatile ("mfence" ::: "memory");

    if (vq->last_used_idx == vq->used->idx)
        return false;

    /* Process used entries and free descriptors */
    while (vq->last_used_idx != vq->used->idx) {
        uint16_t used_slot = vq->last_used_idx % vq->size;
        uint32_t desc_head = vq->used->ring[used_slot].id;

        /* Walk the descriptor chain and free all descriptors */
        uint16_t di = (uint16_t)desc_head;
        while (1) {
            uint16_t next = vq->desc[di].next;
            bool has_next = vq->desc[di].flags & VRING_DESC_F_NEXT;

            /* Return to free list */
            vq->desc[di].next = vq->free_head;
            vq->free_head = di;
            vq->num_free++;

            if (!has_next) break;
            di = next;
        }

        vq->last_used_idx++;
    }

    return true;
}

/* ===== Wait for Completion (Busy Poll) ===== */
// src/virtio.c (or wherever virtio_wait is)

void virtio_wait(virtio_dev_t* dev, uint16_t queue_idx) {
    uint32_t start_ticks = timer_get_ticks();
    
    // Wait for up to 50 ticks (500ms)
    while (!virtio_poll(dev, queue_idx)) {
        if (timer_get_ticks() - start_ticks > 50) {
            serial_printf("virtio: TIMEOUT waiting on queue %u after 500ms\n", queue_idx);
            return;
        }
        
        // Let other tasks (like the GUI!) run while the GPU is busy
        task_yield(); 
    }
}

/* ===== Read ISR Status ===== */
uint8_t virtio_isr_status(virtio_dev_t* dev) {
    if (!dev->isr_cfg) return 0;
    return mmio_read8(dev->isr_cfg, 0);
}

/* ===== Initialize VirtIO Device with Extra Features ===== */
bool virtio_init_features(virtio_dev_t* dev, uint16_t pci_device_id,
                          uint32_t extra_features_lo) {
    memset(dev, 0, sizeof(*dev));

    /* Find the PCI device (same logic as virtio_init) */
    dev->pci = pci_find_device(VIRTIO_PCI_VENDOR, pci_device_id);
    if (!dev->pci) {
        for (uint16_t id = VIRTIO_PCI_DEV_TRANS_LO; id <= VIRTIO_PCI_DEV_TRANS_HI; id++) {
            dev->pci = pci_find_device(VIRTIO_PCI_VENDOR, id);
            if (dev->pci) {
                uint32_t subsys = pci_read(dev->pci->bus, dev->pci->slot,
                                           dev->pci->func, 0x2C);
                uint16_t subsys_dev = (subsys >> 16) & 0xFFFF;
                if (subsys_dev == 16) break;
                dev->pci = NULL;
            }
        }
        if (!dev->pci) {
            serial_printf("virtio: device %x not found on PCI\n", pci_device_id);
            return false;
        }
    }

    serial_printf("virtio: found device %x:%x at %d:%d.%d\n",
                  dev->pci->vendor_id, dev->pci->device_id,
                  dev->pci->bus, dev->pci->slot, dev->pci->func);

    dev->irq = dev->pci->irq_line;
    pci_enable_bus_master(dev->pci);

    if (!parse_capabilities(dev)) {
        serial_printf("virtio: failed to parse PCI capabilities\n");
        return false;
    }

    /* Device init sequence */
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
    for (volatile int i = 0; i < 10000; i++) {}
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Negotiate features — include extra_features_lo */
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_DFSELECT, 0);
    uint32_t features_lo = mmio_read32(dev->common_cfg, VIRTIO_COMMON_DF);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_DFSELECT, 1);
    uint32_t features_hi = mmio_read32(dev->common_cfg, VIRTIO_COMMON_DF);

    serial_printf("virtio: device features[0-31]=%08x [32-63]=%08x\n",
                  features_lo, features_hi);

    uint32_t accepted_lo = features_lo & (WANTED_FEATURES_LO | extra_features_lo);
    uint32_t accepted_hi = features_hi & WANTED_FEATURES_HI;

    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GFSELECT, 0);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, accepted_lo);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GFSELECT, 1);
    mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, accepted_hi);

    serial_printf("virtio: accepted features[0-31]=%08x  [32-63]=%08x\n",
                  accepted_lo, accepted_hi);

    /* FEATURES_OK */
    uint8_t s = mmio_read8(dev->common_cfg, VIRTIO_COMMON_STATUS);
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, s | VIRTIO_STATUS_FEATURES_OK);
    s = mmio_read8(dev->common_cfg, VIRTIO_COMMON_STATUS);
    if (!(s & VIRTIO_STATUS_FEATURES_OK)) {
        serial_printf("virtio: device rejected features\n");
        mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    dev->num_queues = mmio_read16(dev->common_cfg, VIRTIO_COMMON_NUMQ);
    serial_printf("virtio: device has %u queues\n", dev->num_queues);
    mmio_write16(dev->common_cfg, VIRTIO_COMMON_MSIX, 0xFFFF);

    return true;
}

/* ===== Set DRIVER_OK ===== */
void virtio_driver_ok(virtio_dev_t* dev) {
    uint8_t s = mmio_read8(dev->common_cfg, VIRTIO_COMMON_STATUS);
    mmio_write8(dev->common_cfg, VIRTIO_COMMON_STATUS, s | VIRTIO_STATUS_DRIVER_OK);
    serial_printf("virtio: device status = %x (DRIVER_OK)\n",
                  mmio_read8(dev->common_cfg, VIRTIO_COMMON_STATUS));
}

uint16_t virtio_num_queues(virtio_dev_t* dev) {
    return dev->num_queues;
}

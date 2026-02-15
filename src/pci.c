#include "pci.h"
#include "vga.h"

static pci_device_t devices[PCI_MAX_DEVICES];
static uint32_t device_count = 0;

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11)
                    | ((uint32_t)func << 8) | (offset & 0xFC);
    __asm__ volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)PCI_CONFIG_ADDR));
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"((uint16_t)PCI_CONFIG_DATA));
    return ret;
}

void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11)
                    | ((uint32_t)func << 8) | (offset & 0xFC);
    __asm__ volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)PCI_CONFIG_ADDR));
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"((uint16_t)PCI_CONFIG_DATA));
}

const char* pci_class_name(uint8_t cc, uint8_t sc) {
    switch (cc) {
        case 0x00: return "Legacy";
        case 0x01:
            switch (sc) {
                case 0x00: return "SCSI Controller";
                case 0x01: return "IDE Controller";
                case 0x05: return "ATA Controller";
                case 0x06: return "SATA Controller";
                case 0x08: return "NVMe Controller";
                default:   return "Storage Controller";
            }
        case 0x02:
            switch (sc) {
                case 0x00: return "Ethernet Controller";
                case 0x80: return "Network Controller";
                default:   return "Network Controller";
            }
        case 0x03:
            switch (sc) {
                case 0x00: return "VGA Controller";
                case 0x01: return "XGA Controller";
                default:   return "Display Controller";
            }
        case 0x04: return "Multimedia Controller";
        case 0x05: return "Memory Controller";
        case 0x06:
            switch (sc) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x04: return "PCI-PCI Bridge";
                case 0x80: return "Other Bridge";
                default:   return "Bridge Device";
            }
        case 0x07: return "Communication Controller";
        case 0x08: return "System Peripheral";
        case 0x09: return "Input Device";
        case 0x0C:
            switch (sc) {
                case 0x03: return "USB Controller";
                case 0x05: return "SMBus Controller";
                default:   return "Serial Bus Controller";
            }
        case 0x0D: return "Wireless Controller";
        case 0xFF: return "Unassigned";
        default:   return "Unknown Device";
    }
}

static void scan_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t reg0 = pci_read(bus, slot, func, 0);
    uint16_t vendor = reg0 & 0xFFFF;
    uint16_t dev_id = (reg0 >> 16) & 0xFFFF;
    if (vendor == 0xFFFF) return;
    if (device_count >= PCI_MAX_DEVICES) return;

    pci_device_t* d = &devices[device_count];
    d->active    = true;
    d->bus       = bus;
    d->slot      = slot;
    d->func      = func;
    d->vendor_id = vendor;
    d->device_id = dev_id;

    uint32_t reg2 = pci_read(bus, slot, func, 0x08);
    d->class_code = (reg2 >> 24) & 0xFF;
    d->subclass   = (reg2 >> 16) & 0xFF;
    d->prog_if    = (reg2 >> 8) & 0xFF;

    uint32_t reg3 = pci_read(bus, slot, func, 0x0C);
    d->header_type = (reg3 >> 16) & 0xFF;

    uint32_t regI = pci_read(bus, slot, func, 0x3C);
    d->irq_line = regI & 0xFF;

    /* Read BARs */
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read(bus, slot, func, 0x10 + i * 4);

    device_count++;
}

void pci_init(void) {
    device_count = 0;
    memset(devices, 0, sizeof(devices));

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read(bus, slot, 0, 0);
            if ((reg0 & 0xFFFF) == 0xFFFF) continue;
            scan_device(bus, slot, 0);
            /* Check multifunction */
            uint32_t reg3 = pci_read(bus, slot, 0, 0x0C);
            if ((reg3 >> 16) & 0x80) {
                for (uint8_t func = 1; func < 8; func++)
                    scan_device(bus, slot, func);
            }
        }
    }
}

pci_device_t* pci_find_device(uint16_t vendor, uint16_t device) {
    for (uint32_t i = 0; i < device_count; i++)
        if (devices[i].vendor_id == vendor && devices[i].device_id == device)
            return &devices[i];
    return NULL;
}

pci_device_t* pci_find_device_nth(uint16_t vendor, uint16_t device, int n) {
    int found = 0;
    for (uint32_t i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor && devices[i].device_id == device) {
            if (found == n) return &devices[i];
            found++;
        }
    }
    return NULL;
}

pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (uint32_t i = 0; i < device_count; i++)
        if (devices[i].class_code == class_code && devices[i].subclass == subclass)
            return &devices[i];
    return NULL;
}

uint32_t pci_device_count(void) { return device_count; }

void pci_list(void) {
    kprintf("  BUS:SL.FN  VENDOR:DEV   CLASS  IRQ  DESCRIPTION\n");
    kprintf("  ---------  ----------   -----  ---  -----------\n");
    for (uint32_t i = 0; i < device_count; i++) {
        pci_device_t* d = &devices[i];
        kprintf("  %d:%d.%d    ", d->bus, d->slot, d->func);
        terminal_print_hex(d->vendor_id);
        kprintf(":");
        terminal_print_hex(d->device_id);
        kprintf("  %x:%x  ", d->class_code, d->subclass);
        kprintf(" %-3u  %s\n", d->irq_line, pci_class_name(d->class_code, d->subclass));
    }
    kprintf("  Total: %u devices\n", device_count);
}

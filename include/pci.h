#ifndef PCI_H
#define PCI_H

#include "types.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_MAX_DEVICES 64

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  header_type;
    uint8_t  irq_line;
    uint32_t bar[6];
    bool     active;
} pci_device_t;

void         pci_init(void);
uint32_t     pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void         pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
pci_device_t* pci_find_device(uint16_t vendor, uint16_t device);
pci_device_t* pci_find_device_nth(uint16_t vendor, uint16_t device, int n);
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass);
uint32_t     pci_device_count(void);
void         pci_list(void);
const char*  pci_class_name(uint8_t class_code, uint8_t subclass);

#endif

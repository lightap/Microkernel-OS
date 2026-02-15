#include "bga.h"
#include "pci.h"
#include "paging.h"

/* BGA I/O ports */
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

/* BGA register indices */
#define VBE_DISPI_INDEX_ID      0
#define VBE_DISPI_INDEX_XRES    1
#define VBE_DISPI_INDEX_YRES    2
#define VBE_DISPI_INDEX_BPP     3
#define VBE_DISPI_INDEX_ENABLE  4
#define VBE_DISPI_INDEX_BANK    5
#define VBE_DISPI_INDEX_VIRT_W  6
#define VBE_DISPI_INDEX_VIRT_H  7

/* BGA enable flags */
#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40

/* BGA ID range */
#define VBE_DISPI_ID0           0xB0C0
#define VBE_DISPI_ID5           0xB0C5

/* QEMU/Bochs VGA PCI IDs */
#define BGA_PCI_VENDOR          0x1234
#define BGA_PCI_DEVICE          0x1111

/* Virtual address where we map the framebuffer (512MB mark) */
#define FB_VIRT_BASE            0x20000000

static uint32_t fb_phys = 0;
static uint8_t* fb_virt = NULL;
static uint16_t cur_w = 0, cur_h = 0;
static bool     detected = false;

static void bga_write(uint16_t reg, uint16_t val) {
    outw(VBE_DISPI_IOPORT_INDEX, reg);
    outw(VBE_DISPI_IOPORT_DATA, val);
}

static uint16_t bga_read(uint16_t reg) {
    outw(VBE_DISPI_IOPORT_INDEX, reg);
    return inw(VBE_DISPI_IOPORT_DATA);
}

bool bga_available(void) {
    if (detected) return true;

    /* Check BGA ID register */
    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    if (id < VBE_DISPI_ID0 || id > VBE_DISPI_ID5)
        return false;

    /* Find VGA device on PCI for framebuffer address */
    pci_device_t* dev = pci_find_device(BGA_PCI_VENDOR, BGA_PCI_DEVICE);
    if (!dev) dev = pci_find_class(0x03, 0x00);
    if (!dev) return false;

    fb_phys = dev->bar[0] & 0xFFFFFFF0;
    if (fb_phys == 0) return false;

    detected = true;
    return true;
}

bool bga_set_mode(uint16_t width, uint16_t height, uint8_t bpp) {
    if (!bga_available()) return false;

    /* Disable first */
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);

    /* Set resolution */
    bga_write(VBE_DISPI_INDEX_XRES, width);
    bga_write(VBE_DISPI_INDEX_YRES, height);
    bga_write(VBE_DISPI_INDEX_BPP, bpp);

    /* Enable with linear framebuffer */
    bga_write(VBE_DISPI_INDEX_ENABLE,
              VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    /* Verify */
    if (bga_read(VBE_DISPI_INDEX_XRES) != width ||
        bga_read(VBE_DISPI_INDEX_YRES) != height)
        return false;

    cur_w = width;
    cur_h = height;

    /* Map framebuffer physical memory to virtual address */
    uint32_t fb_size = (uint32_t)width * height * (bpp / 8);
    paging_map_range(FB_VIRT_BASE, fb_phys, fb_size, PAGE_PRESENT | PAGE_WRITE);
    fb_virt = (uint8_t*)FB_VIRT_BASE;

    return true;
}

void bga_disable(void) {
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    cur_w = 0;
    cur_h = 0;
}

uint8_t* bga_get_fb(void)     { return fb_virt; }
uint16_t bga_get_width(void)  { return cur_w; }
uint16_t bga_get_height(void) { return cur_h; }

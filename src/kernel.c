/*
 * MicroKernel v0.7.0 — A real x86 microkernel
 *
 * Ring 0 kernel: scheduler, IPC, VMM, IRQ routing, syscalls
 * Ring 3 servers: console, VFS, disk, network
 * Ring 3 user: shell, editor, games
 *
 * Communication between all components goes through synchronous IPC.
 */

#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "serial.h"
#include "rtc.h"
#include "pmm.h"
#include "heap.h"
#include "paging.h"
#include "task.h"
#include "ipc.h"
#include "ramfs.h"
#include "speaker.h"
#include "cpuid.h"
#include "pci.h"
#include "env.h"
#include "syscall.h"
#include "login.h"
#include "net.h"
#include "shell.h"
#include "image.h"
#include "procfs.h"
#include "ata.h"
#include "fat16.h"
#include "ntfs.h"
#include "server.h"
#include "elf.h"
#include "virtio_input.h"

#define MULTIBOOT_MAGIC 0x2BADB002
#define HEAP_SIZE       (128 * 1024 * 1024)   /* 128MB heap */

extern uint32_t _kernel_end;

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device, cmdline;
    uint32_t mods_count, mods_addr;
};

struct multiboot_module {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t reserved;
};

static struct multiboot_info* saved_mbi = NULL;

static void ok(const char* msg) {
    terminal_print_colored("  [", 0x07);
    terminal_print_colored("OK", 0x0A);
    terminal_print_colored("] ", 0x07);
    kprintf("%s\n", msg);
}

static void boot_logo(void) {
    terminal_clear();
    terminal_print_colored("\n", 0x0F);
    terminal_print_colored("   __  __ _               _  __                    _\n", 0x0B);
    terminal_print_colored("  |  \\/  (_) ___ _ __ ___| |/ /___ _ __ _ __   ___| |\n", 0x0B);
    terminal_print_colored("  | |\\/| | |/ __| '__/ _ \\ ' // _ \\ '__| '_ \\ / _ \\ |\n", 0x03);
    terminal_print_colored("  | |  | | | (__| | | (_) | . \\  __/ |  | | | |  __/ |\n", 0x03);
    terminal_print_colored("  |_|  |_|_|\\___|_|  \\___/|_|\\_\\___|_|  |_| |_|\\___|_|\n", 0x01);
    terminal_print_colored("                                            v0.7.0\n\n", 0x08);
}

static void create_default_fs(void) {
    ramfs_create("/bin", RAMFS_DIR);
    ramfs_create("/etc", RAMFS_DIR);
    ramfs_create("/home", RAMFS_DIR);
    ramfs_create("/home/root", RAMFS_DIR);
    ramfs_create("/home/user", RAMFS_DIR);
    ramfs_create("/tmp", RAMFS_DIR);
    ramfs_create("/var", RAMFS_DIR);
    ramfs_create("/var/log", RAMFS_DIR);
    ramfs_create("/dev", RAMFS_DIR);
    ramfs_create("/proc", RAMFS_DIR);
    ramfs_create("/proc/net", RAMFS_DIR);
    ramfs_create("/disk", RAMFS_DIR);
    ramfs_create("/disk2", RAMFS_DIR);

    const char* motd = "Welcome to MicroKernel v0.7.0!\n"
                        "A real microkernel: ring 3 servers, synchronous IPC, per-process address spaces.\n"
                        "Type 'help' for commands, 'services' for server list.\n";
    ramfs_write("/etc/motd", motd, strlen(motd));

    const char* readme = "MicroKernel v0.7.0 - True x86 Microkernel Architecture\n"
                          "Kernel (ring 0): scheduler, IPC, VMM, IRQ routing\n"
                          "Servers (ring 3): console, VFS, ATA disk, network\n"
                          "User (ring 3): shell, editor, games, screensavers\n";
    ramfs_write("/etc/readme", readme, strlen(readme));

    const char* hosts = "127.0.0.1  localhost\n10.0.2.15  microkernel\n";
    ramfs_write("/etc/hosts", hosts, strlen(hosts));

    const char* script = "# System info script\necho === System Report ===\nuname -a\nuptime\nmem\nps\nservices\necho === Done ===\n";
    ramfs_write("/bin/sysreport.sh", script, strlen(script));
}

static int load_multiboot_modules(struct multiboot_info* mbi) {
    if (!mbi || !(mbi->flags & (1 << 3)) || mbi->mods_count == 0)
        return 0;

    struct multiboot_module* mods = (struct multiboot_module*)(uint32_t)mbi->mods_addr;
    int loaded = 0;

    kprintf("    %u module(s) found, max file size: %u KB\n",
            mbi->mods_count, (uint32_t)(RAMFS_MAX_DATA / 1024));

    for (uint32_t i = 0; i < mbi->mods_count; i++) {
        uint32_t start = mods[i].mod_start;
        uint32_t end   = mods[i].mod_end;
        uint32_t size  = end - start;

        if (size == 0) continue;
        if (size > RAMFS_MAX_DATA) {
            kprintf("    Module %u: %u KB > limit, skipped\n", i, size / 1024);
            continue;
        }

        const char* modname = (const char*)(uint32_t)mods[i].cmdline;
        char fname[64];

        if (modname && *modname) {
            const char* base = modname;
            for (const char* p = modname; *p; p++)
                if (*p == '/' || *p == '\\') base = p + 1;
            strcpy(fname, "/home/root/");
            int flen = strlen(fname);
            strncpy(fname + flen, base, sizeof(fname) - flen - 1);
            fname[sizeof(fname) - 1] = '\0';
        } else {
            strcpy(fname, "/home/root/file");
            fname[15] = '0' + i;
            fname[16] = '\0';
            const uint8_t* d = (const uint8_t*)(uint32_t)start;
            if (size >= 8 && d[0] == 137 && d[1] == 'P') strcat(fname, ".png");
            else if (size >= 2 && d[0] == 'B' && d[1] == 'M') strcat(fname, ".bmp");
            else strcat(fname, ".dat");
        }

        int32_t ret = ramfs_write(fname, (const char*)(uint32_t)start, size);
        if (ret >= 0) {
            kprintf("    Loaded: %s (%u KB)\n", fname, size / 1024);
            loaded++;
        }
    }
    return loaded;
}

void enable_fpu(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2); // Clear EM (Emulation)
    cr0 |= (1 << 1);  // Set MP (Monitor Coprocessor)
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (3 << 9);  // Set OSFXSR and OSXMMEXCPT (SSE support)
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}
void kernel_main(uint32_t magic, struct multiboot_info* mbi) {



    /* ============================================================
     * Phase 1: Core hardware — ring 0 kernel initialization
     * ============================================================ */
    terminal_init();
    boot_logo();
    saved_mbi = mbi;

    if (magic != MULTIBOOT_MAGIC) {
        terminal_print_colored("  [FAIL] Not Multiboot!\n", 0x0C);
        return;
    }
    ok("Multiboot verified");

    serial_init(COM1);
    serial_printf("MicroKernel v0.7.0 booting...\n");
    ok("Serial (COM1 @ 38400)");

    cpuid_init();
    {
        cpu_info_t* ci = cpuid_get_info();
        const char* brand = ci->brand;
        while (*brand == ' ') brand++;
        kprintf("  [");
        terminal_print_colored("OK", 0x0A);
        kprintf("] CPU: %s\n", brand);
    }

    /* GDT with TSS — enables ring 3 ↔ ring 0 transitions */
    gdt_init();
    ok("GDT + TSS (ring 0/3 transitions)");

    idt_init();
    ok("IDT + PIC (IRQ routing)");

    // Core FPU/SSE Initialization
    __asm__ volatile (
        "clts\n"
        "mov %%cr0, %%eax\n"
        "and $0xFFFFFFFB, %%eax\n" // Clear EM
        "or $0x00000022, %%eax\n"  // Set MP, NE
        "mov %%eax, %%cr0\n"
        "mov %%cr4, %%eax\n"
        "or $0x00000600, %%eax\n"  // Set OSFXSR, OSXMMEXCPT
        "mov %%eax, %%cr4\n"
        "finit\n"                  // Initialize FPU
        : : : "eax"
    );

    timer_init(100);
    ok("PIT timer (100 Hz)");

    keyboard_init();
    ok("PS/2 keyboard");

    rtc_init();
    {
        rtc_time_t t; rtc_read(&t);
        kprintf("  [");
        terminal_print_colored("OK", 0x0A);
        kprintf("] RTC (%u/%u/%u %d:%d:%d)\n", t.month, t.day, t.year, t.hour, t.minute, t.second);
    }

    /* ============================================================
     * Phase 2: Memory management — per-process address spaces
     * ============================================================ */
    uint32_t mem_kb = 64 * 1024;
    if (mbi->flags & 0x1) mem_kb = mbi->mem_upper + 1024;

    pmm_init(mem_kb);
    kprintf("  [");
    terminal_print_colored("OK", 0x0A);
    kprintf("] Physical memory (%u MB, %u MB free)\n", mem_kb/1024, pmm_get_free_pages()*4/1024);

    uint32_t heap_start = ((uint32_t)&_kernel_end + 0xFFF) & ~0xFFF;
    if (saved_mbi && (saved_mbi->flags & (1 << 3)) && saved_mbi->mods_count > 0) {
        struct multiboot_module* mods = (struct multiboot_module*)(uint32_t)saved_mbi->mods_addr;
        for (uint32_t i = 0; i < saved_mbi->mods_count; i++) {
            if (mods[i].mod_end > heap_start)
                heap_start = (mods[i].mod_end + 0xFFF) & ~0xFFF;
        }
    }
    heap_init((void*)heap_start, HEAP_SIZE);
    pmm_reserve_range(heap_start, HEAP_SIZE);
    kprintf("  [");
    terminal_print_colored("OK", 0x0A);
    kprintf("] Kernel heap (%u MB at %x)\n", HEAP_SIZE/(1024*1024), heap_start);

    paging_init(mem_kb);
    ok("Paging (per-process address spaces)");

    /* ============================================================
     * Phase 3: Microkernel subsystems
     * ============================================================ */
    task_init();
    ok("Task scheduler (preemptive, ring 0/3)");

    ipc_init();
    ok("Synchronous IPC (blocking send/receive)");

    syscall_init();
    ok("Syscall interface (INT 0x80, ring 3 safe)");

    env_init();
    ok("Environment variables");

    login_init();
    ok("User accounts (root/user/guest)");

    /* ============================================================
     * Phase 4: Filesystem and modules
     * ============================================================ */
    ramfs_init();
    create_default_fs();
    procfs_init();
    image_create_test();
    int nmods = load_multiboot_modules(saved_mbi);
    kprintf("  [");
    terminal_print_colored("OK", 0x0A);
    kprintf("] RAM filesystem (%u files", ramfs_file_count());
    if (nmods > 0) kprintf(", %d loaded from boot", nmods);
    kprintf(")\n");

    /* ============================================================
     * Phase 5: Hardware detection
     * ============================================================ */
    pci_init();
    if (virtio_gpu_available()) {
    /* Check if 3D is available */
    bool has_3d = virgl_available();
    kprintf(" [OK] VirtIO-GPU (%s)\n", has_3d ? "3D virgl" : "2D only");
}
    if (virtio_input_available()) {
        kprintf("  [");
        terminal_print_colored("OK", 0x0A);
        kprintf("] VirtIO-Input (tablet/mouse)\n");
    }
    kprintf("  [");
    terminal_print_colored("OK", 0x0A);
    kprintf("] PCI bus (%u devices)\n", pci_device_count());

    net_init();
    if (net_is_available()) {
        terminal_print_colored("  [", 0x07);
        terminal_print_colored("OK", 0x0A);
        terminal_print_colored("] ", 0x07);
        kprintf("Network (RTL8139)\n");
    } else {
        terminal_print_colored("  [", 0x07);
        terminal_print_colored("--", 0x08);
        terminal_print_colored("] ", 0x07);
        kprintf("Network (no NIC)\n");
    }

    ok("PC speaker");

    /* ATA disk detection */
    ata_init();
    if (ata_drive_count() == 0) {
        kprintf("  [");
        terminal_print_colored("--", 0x08);
        kprintf("] ATA disk (none)\n");
    }

    for (int drv = 0; drv < ATA_MAX_DRIVES; drv++) {
        if (!ata_drive_present(drv)) continue;
        ata_drive_t* d = ata_get_drive_n(drv);
        const char* mnt = (drv == 0) ? "/disk" : "/disk2";
        kprintf("  [");
        terminal_print_colored("OK", 0x0A);
        kprintf("] ATA %s: %s (%u MB)\n", drv == 0 ? "hda" : "hdb", d->model, d->size_mb);

        if (fat16_mount_drive(drv)) {
            fat16_info_t* fi = fat16_get_info();
            kprintf("  [");
            terminal_print_colored("OK", 0x0A);
            kprintf("] FAT16 at %s: %s (%u MB)\n", mnt, fi->volume_label,
                    (fi->total_clusters * fi->cluster_size) / (1024 * 1024));
        } else if (ntfs_mount_drive(drv)) {
            ntfs_info_t* ni = ntfs_get_info();
            kprintf("  [");
            terminal_print_colored("OK", 0x0A);
            kprintf("] NTFS at %s: %s (%u MB)\n", mnt, ni->volume_label,
                    (uint32_t)((ni->total_sectors * ni->bytes_per_sector) >> 20));
        } else {
            kprintf("  [");
            terminal_print_colored("--", 0x08);
            kprintf("] %s: no filesystem\n", mnt);
        }
    }

    /* Enable interrupts */
    sti();
    ok("Interrupts enabled");

    /* ============================================================
     * Phase 6: Launch microkernel servers (ring 3)
     *
     * Two modes:
     *   1. ELF modules: truly isolated address spaces (kernel pages
     *      are supervisor-only, servers CANNOT access kernel memory)
     *   2. In-kernel fallback: single-binary model (servers share
     *      kernel address space with PAGE_USER — less isolated)
     *
     * Mode 1 is used when GRUB loads server ELF binaries as modules.
     * Mode 2 is the fallback when booting without modules.
     * ============================================================ */
    terminal_print_colored("\n  --- Launching servers (ring 3) ---\n", 0x0E);

    bool elf_loaded = false;
    if (saved_mbi && (saved_mbi->flags & (1 << 3)) && saved_mbi->mods_count > 0) {
        struct multiboot_module* mods = (struct multiboot_module*)(uint32_t)saved_mbi->mods_addr;
        uint32_t elf_count = 0;

        for (uint32_t i = 0; i < saved_mbi->mods_count; i++) {
            uint32_t start = mods[i].mod_start;
            uint32_t end   = mods[i].mod_end;
            uint32_t size  = end - start;
            const char* cmdline = (const char*)(uint32_t)mods[i].cmdline;

            if (size < sizeof(elf32_ehdr_t)) continue;
            if (elf_validate((void*)start, size) != 0) continue;

            /* Determine which server this is from the module command line */
            int32_t pid = -1;
            if (cmdline && *cmdline) {
                /* Find the basename */
                const char* name = cmdline;
                for (const char* p = cmdline; *p; p++)
                    if (*p == '/' || *p == '\\') name = p + 1;

                if (strncmp(name, "console", 7) == 0) {
                    pid = elf_load((void*)start, size, "console_srv", 1, true, ELF_MAP_VGA);
                    if (pid >= 0) console_server_pid = pid;
                } else if (strncmp(name, "vfs", 3) == 0) {
                    pid = elf_load((void*)start, size, "vfs_srv", 2, false, 0);
                    if (pid >= 0) vfs_server_pid = pid;
                } else if (strncmp(name, "ata", 3) == 0 || strncmp(name, "disk", 4) == 0) {
                    pid = elf_load((void*)start, size, "ata_srv", 2, true, 0);
                    if (pid >= 0) disk_server_pid = pid;
                } else if (strncmp(name, "net", 3) == 0) {
                    pid = elf_load((void*)start, size, "net_srv", 2, true, 0);
                    if (pid >= 0) net_server_pid = pid;
                }
            }

            if (pid >= 0) {
                elf_count++;
                kprintf("  [");
                terminal_print_colored("OK", 0x0A);
                kprintf("] ELF: %s (PID %u, isolated)\n",
                        cmdline ? cmdline : "unknown", pid);
            }
        }

        if (elf_count > 0) {
            elf_loaded = true;
            terminal_print_colored("  --- ELF servers: truly isolated address spaces ---\n", 0x0E);
        }
    }

    /* Fall back to in-kernel servers for any not loaded from ELF */
    if (!elf_loaded || !console_server_pid || !vfs_server_pid ||
        !disk_server_pid || !net_server_pid) {
        if (!elf_loaded) {
            kprintf("  (no ELF modules, using in-kernel servers)\n");
        }
        servers_launch_missing();
    }

    kprintf("  [");
    terminal_print_colored("OK", 0x0A);
    kprintf("] Console server (PID %u, ring 3%s)\n", console_server_pid,
            elf_loaded ? ", isolated" : "");
    kprintf("  [");
    terminal_print_colored("OK", 0x0A);
    kprintf("] VFS server (PID %u, ring 3%s)\n", vfs_server_pid,
            elf_loaded ? ", isolated" : "");
    kprintf("  [");
    terminal_print_colored("OK", 0x0A);
    kprintf("] ATA disk server (PID %u, ring 3%s)\n", disk_server_pid,
            elf_loaded ? ", isolated" : ", IOPL=3");
    kprintf("  [");
    terminal_print_colored("OK", 0x0A);
    kprintf("] Network server (PID %u, ring 3%s)\n", net_server_pid,
            elf_loaded ? ", isolated" : ", IOPL=3");

    terminal_print_colored("  --- All servers running ---\n\n", 0x0E);

    /* Startup sound */
    speaker_startup_sound();

    /* ============================================================
     * Phase 7: Login and shell
     * ============================================================ */
    terminal_print_colored("  ----------------------------------------\n", 0x08);
    {
        char motd[512]; int32_t n = ramfs_read("/etc/motd", motd, sizeof(motd)-1);
        if (n > 0) { motd[n] = '\0'; kprintf("  %s", motd); }
    }
    terminal_print_colored("  ----------------------------------------\n", 0x08);

    login_set_user("root");
    env_set("USER", "root");
    env_set("HOME", "/home/root");
    kprintf("\n  Logged in as ");
    terminal_print_colored("root", 0x0A);
    kprintf("\n\n");

    serial_printf("Boot complete. Microkernel architecture active.\n");
    serial_printf("  Kernel (ring 0): scheduler, IPC, VMM\n");
    serial_printf("  Servers (ring 3): console, VFS, ATA, network\n");

    /* Launch shell — runs in ring 0 for now (has direct driver access).
     * Moving shell to ring 3 is the next step: replace direct calls
     * with IPC client wrappers. The shell can use 'services' command
     * to see running servers and 'ps' to see ring levels. */
    shell_init();
    shell_run();
}

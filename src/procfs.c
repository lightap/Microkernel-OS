#include "procfs.h"
#include "task.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include "paging.h"
#include "cpuid.h"
#include "rtc.h"
#include "ramfs.h"
#include "net.h"
#include "ipc.h"
#include "login.h"
#include "fat16.h"
#include "env.h"
#include "vga.h"

/* ---- ksnprintf implementation ---- */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

static int sn_puts(char* buf, int pos, int max, const char* s) {
    while (*s && pos < max - 1) buf[pos++] = *s++;
    return pos;
}

static int sn_putd(char* buf, int pos, int max, int32_t val) {
    if (val < 0) { if (pos < max - 1) buf[pos++] = '-'; val = -val; }
    char tmp[12]; int i = 0;
    if (val == 0) tmp[i++] = '0';
    else while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    for (int j = i - 1; j >= 0 && pos < max - 1; j--) buf[pos++] = tmp[j];
    return pos;
}

static int sn_putu(char* buf, int pos, int max, uint32_t val) {
    char tmp[12]; int i = 0;
    if (val == 0) tmp[i++] = '0';
    else while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    for (int j = i - 1; j >= 0 && pos < max - 1; j--) buf[pos++] = tmp[j];
    return pos;
}

static int sn_putx(char* buf, int pos, int max, uint32_t val) {
    const char* hex = "0123456789abcdef";
    char tmp[9]; int i = 0;
    if (val == 0) tmp[i++] = '0';
    else while (val > 0) { tmp[i++] = hex[val & 0xF]; val >>= 4; }
    for (int j = i - 1; j >= 0 && pos < max - 1; j--) buf[pos++] = tmp[j];
    return pos;
}

int ksnprintf(char* buf, int max, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int pos = 0;
    while (*fmt && pos < max - 1) {
        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
        fmt++;
        switch (*fmt) {
            case 's': pos = sn_puts(buf, pos, max, va_arg(ap, const char*)); break;
            case 'd': pos = sn_putd(buf, pos, max, va_arg(ap, int32_t)); break;
            case 'u': pos = sn_putu(buf, pos, max, va_arg(ap, uint32_t)); break;
            case 'x': pos = sn_putx(buf, pos, max, va_arg(ap, uint32_t)); break;
            case 'c': if (pos < max - 1) buf[pos++] = (char)va_arg(ap, int); break;
            case '%': if (pos < max - 1) buf[pos++] = '%'; break;
            case '\0': goto done;
            default: if (pos < max - 1) buf[pos++] = *fmt; break;
        }
        fmt++;
    }
done:
    buf[pos] = '\0';
    va_end(ap);
    return pos;
}

/* ---- /proc file generators ---- */

static int gen_cpuinfo(char* buf, int max) {
    cpu_info_t* ci = cpuid_get_info();
    const char* brand = ci->brand;
    while (*brand == ' ') brand++;
    int p = 0;
    p += ksnprintf(buf + p, max - p, "processor\t: 0\n");
    p += ksnprintf(buf + p, max - p, "vendor_id\t: %s\n", ci->vendor);
    p += ksnprintf(buf + p, max - p, "model name\t: %s\n", brand);
    p += ksnprintf(buf + p, max - p, "cpu family\t: %u\n", ci->family);
    p += ksnprintf(buf + p, max - p, "model\t\t: %u\n", ci->model);
    p += ksnprintf(buf + p, max - p, "stepping\t: %u\n", ci->stepping);
    p += ksnprintf(buf + p, max - p, "flags\t\t:");
    if (ci->has_fpu)  p += ksnprintf(buf + p, max - p, " fpu");
    if (ci->has_pae)  p += ksnprintf(buf + p, max - p, " pae");
    if (ci->has_pse)  p += ksnprintf(buf + p, max - p, " pse");
    if (ci->has_apic) p += ksnprintf(buf + p, max - p, " apic");
    if (ci->has_msr)  p += ksnprintf(buf + p, max - p, " msr");
    if (ci->has_sse)  p += ksnprintf(buf + p, max - p, " sse");
    if (ci->has_sse2) p += ksnprintf(buf + p, max - p, " sse2");
    if (ci->has_sse3) p += ksnprintf(buf + p, max - p, " sse3");
    if (ci->has_avx)  p += ksnprintf(buf + p, max - p, " avx");
    p += ksnprintf(buf + p, max - p, "\nbogomips\t: %u.00\n", timer_get_frequency() * 2);
    return p;
}

static int gen_meminfo(char* buf, int max) {
    uint32_t total_pages = pmm_get_total_pages();
    uint32_t free_pages = pmm_get_free_pages();
    uint32_t used_pages = pmm_get_used_pages();
    uint32_t total_kb = total_pages * 4;
    uint32_t free_kb = free_pages * 4;
    uint32_t used_kb = used_pages * 4;
    uint32_t heap_free_kb = heap_free_space() / 1024;
    uint32_t heap_used_kb = heap_used_space() / 1024;

    int p = 0;
    p += ksnprintf(buf + p, max - p, "MemTotal:       %u kB\n", total_kb);
    p += ksnprintf(buf + p, max - p, "MemFree:        %u kB\n", free_kb);
    p += ksnprintf(buf + p, max - p, "MemUsed:        %u kB\n", used_kb);
    p += ksnprintf(buf + p, max - p, "HeapFree:       %u kB\n", heap_free_kb);
    p += ksnprintf(buf + p, max - p, "HeapUsed:       %u kB\n", heap_used_kb);
    p += ksnprintf(buf + p, max - p, "PageSize:       4 kB\n");
    p += ksnprintf(buf + p, max - p, "TotalPages:     %u\n", total_pages);
    p += ksnprintf(buf + p, max - p, "FreePages:      %u\n", free_pages);
    return p;
}

static int gen_uptime(char* buf, int max) {
    uint32_t secs = timer_get_seconds();
    uint32_t ticks = timer_get_ticks();
    int p = 0;
    p += ksnprintf(buf + p, max - p, "%u.%u 0.0\n", secs, (ticks % 100) * 10);
    return p;
}

static int gen_version(char* buf, int max) {
    return ksnprintf(buf, max,
        "MicroKernel v0.2.0 (i686-gcc) #1 SMP PREEMPT x86\n");
}

static int gen_stat(char* buf, int max) {
    uint32_t ticks = timer_get_ticks();
    uint32_t switches = task_total_switches();
    int p = 0;
    p += ksnprintf(buf + p, max - p, "cpu  %u 0 0 %u 0 0 0 0 0 0\n", ticks, ticks);
    p += ksnprintf(buf + p, max - p, "ctxt %u\n", switches);
    p += ksnprintf(buf + p, max - p, "btime 0\n");
    p += ksnprintf(buf + p, max - p, "processes %u\n", task_count());
    p += ksnprintf(buf + p, max - p, "procs_running %u\n", task_count());
    return p;
}

static int gen_loadavg(char* buf, int max) {
    uint32_t count = task_count();
    return ksnprintf(buf, max, "0.%u 0.%u 0.%u %u/%u 0\n",
                     count * 5, count * 3, count * 2, count, count);
}

static int gen_filesystems(char* buf, int max) {
    int p = 0;
    p += ksnprintf(buf + p, max - p, "nodev\tramfs\n");
    p += ksnprintf(buf + p, max - p, "nodev\tprocfs\n");
    p += ksnprintf(buf + p, max - p, "\tfat16\n");
    return p;
}

static int gen_mounts(char* buf, int max) {
    int p = 0;
    p += ksnprintf(buf + p, max - p, "ramfs / ramfs rw 0 0\n");
    p += ksnprintf(buf + p, max - p, "procfs /proc procfs ro 0 0\n");
    if (fat16_is_mounted()) {
        fat16_info_t* fi = fat16_get_info();
        p += ksnprintf(buf + p, max - p, "/dev/hda /disk fat16 rw 0 0 # %s\n",
                        fi->volume_label);
    }
    return p;
}

static const char* task_state_str[] = {
    "R (ready)", "R (running)", "S (sleeping)", "D (blocked)", "Z (terminated)"
};

static int gen_pid_status(char* buf, int max, int pid) {
    task_t* all = task_get_all();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (all[i].active && (int)all[i].id == pid) {
            int p = 0;
            p += ksnprintf(buf + p, max - p, "Name:\t%s\n", all[i].name);
            p += ksnprintf(buf + p, max - p, "State:\t%s\n", task_state_str[all[i].state]);
            p += ksnprintf(buf + p, max - p, "Pid:\t%u\n", all[i].id);
            p += ksnprintf(buf + p, max - p, "Priority:\t%u\n", all[i].priority);
            p += ksnprintf(buf + p, max - p, "Quantum:\t%u\n", all[i].quantum);
            p += ksnprintf(buf + p, max - p, "CpuTicks:\t%u\n", all[i].cpu_ticks);
            p += ksnprintf(buf + p, max - p, "Switches:\t%u\n", all[i].switches);
            p += ksnprintf(buf + p, max - p, "StackBase:\t0x%x\n", all[i].stack_base);
            p += ksnprintf(buf + p, max - p, "StackSize:\t%u\n", all[i].stack_size);
            return p;
        }
    }
    return ksnprintf(buf, max, "No such process\n");
}

static int gen_processes(char* buf, int max) {
    task_t* all = task_get_all();
    int p = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (all[i].active) {
            p += ksnprintf(buf + p, max - p, "%u %s %s\n",
                           all[i].id, all[i].name, task_state_str[all[i].state]);
        }
    }
    return p;
}

static int gen_net_dev(char* buf, int max) {
    int p = 0;
    p += ksnprintf(buf + p, max - p,
        "Inter-|   Receive                                     |  Transmit\n");
    p += ksnprintf(buf + p, max - p,
        " face |bytes  packets errs drop fifo frame compressed |bytes  packets\n");
    if (net_is_available()) {
        p += ksnprintf(buf + p, max - p,
            "  eth0: 0       0       0    0    0    0     0         0       0\n");
    }
    p += ksnprintf(buf + p, max - p,
        "    lo: 0       0       0    0    0    0     0         0       0\n");
    return p;
}

static int gen_interrupts(char* buf, int max) {
    int p = 0;
    p += ksnprintf(buf + p, max - p, "  0: %u   PIT (timer)\n", timer_get_ticks());
    p += ksnprintf(buf + p, max - p, "  1: -    PS/2 keyboard\n");
    p += ksnprintf(buf + p, max - p, "  8: -    RTC\n");
    if (net_is_available())
        p += ksnprintf(buf + p, max - p, " 11: -    RTL8139 NIC\n");
    p += ksnprintf(buf + p, max - p, "128: -    Syscall (INT 0x80)\n");
    return p;
}

static int gen_scheduler(char* buf, int max) {
    int p = 0;
    p += ksnprintf(buf + p, max - p, "mode:\t\t%s\n",
                   task_is_preemptive() ? "preemptive" : "cooperative");
    p += ksnprintf(buf + p, max - p, "quantum:\t%u ticks (%u ms)\n",
                   task_get_quantum(), task_get_quantum() * 1000 / timer_get_frequency());
    p += ksnprintf(buf + p, max - p, "frequency:\t%u Hz\n", timer_get_frequency());
    p += ksnprintf(buf + p, max - p, "total_switches:\t%u\n", task_total_switches());
    p += ksnprintf(buf + p, max - p, "active_tasks:\t%u\n", task_count());
    return p;
}

/* ---- /proc dispatch ---- */

typedef struct {
    const char* name;
    int (*generator)(char* buf, int max);
} procfs_entry_t;

static const procfs_entry_t proc_files[] = {
    { "cpuinfo",     gen_cpuinfo },
    { "meminfo",     gen_meminfo },
    { "uptime",      gen_uptime },
    { "version",     gen_version },
    { "stat",        gen_stat },
    { "loadavg",     gen_loadavg },
    { "filesystems", gen_filesystems },
    { "mounts",      gen_mounts },
    { "processes",   gen_processes },
    { "interrupts",  gen_interrupts },
    { "scheduler",   gen_scheduler },
    { NULL, NULL }
};

/* Sub-directories */
static const procfs_entry_t proc_net_files[] = {
    { "dev", gen_net_dev },
    { NULL, NULL }
};

void procfs_init(void) {
    /* /proc and /proc/net directories are created by create_default_fs */
}

bool procfs_is_virtual(const char* path) {
    return (strncmp(path, "/proc", 5) == 0);
}

bool procfs_is_dir(const char* path) {
    if (strcmp(path, "/proc") == 0) return true;
    if (strcmp(path, "/proc/net") == 0) return true;
    /* /proc/<pid> directories */
    if (strncmp(path, "/proc/", 6) == 0) {
        const char* rest = path + 6;
        if (isdigit(rest[0])) {
            /* Check it's just a number */
            const char* p = rest;
            while (*p && isdigit(*p)) p++;
            if (*p == '\0') return true;
        }
    }
    return false;
}

int32_t procfs_read(const char* path, void* buf, uint32_t max) {
    if (!procfs_is_virtual(path)) return -1;
    if (max == 0) return 0;

    char resolved[128];
    if (path[0] != '/') {
        /* Shouldn't happen for /proc paths, but handle it */
        strcpy(resolved, "/proc/");
        strcat(resolved, path);
    } else {
        strcpy(resolved, path);
    }

    const char* name = resolved + 6; /* skip "/proc/" */
    if (strlen(resolved) <= 6) return -1;

    /* Check /proc/net/ subdir */
    if (strncmp(name, "net/", 4) == 0) {
        const char* net_name = name + 4;
        for (int i = 0; proc_net_files[i].name; i++) {
            if (strcmp(net_name, proc_net_files[i].name) == 0)
                return proc_net_files[i].generator((char*)buf, max);
        }
        return -1;
    }

    /* Check /proc/<pid>/status */
    if (isdigit(name[0])) {
        int pid = atoi(name);
        const char* slash = strchr(name, '/');
        if (slash && strcmp(slash + 1, "status") == 0) {
            return gen_pid_status((char*)buf, max, pid);
        }
        /* /proc/<pid> alone -> same as status */
        if (!slash || slash[1] == '\0') {
            return gen_pid_status((char*)buf, max, pid);
        }
        return -1;
    }

    /* Check top-level /proc/ files */
    for (int i = 0; proc_files[i].name; i++) {
        if (strcmp(name, proc_files[i].name) == 0)
            return proc_files[i].generator((char*)buf, max);
    }

    return -1;
}

int32_t procfs_stat(const char* path, ramfs_type_t* type, uint32_t* size) {
    if (!procfs_is_virtual(path)) return -1;

    if (procfs_is_dir(path)) {
        if (type) *type = RAMFS_DIR;
        if (size) *size = 0;
        return 0;
    }

    /* Try generating the content to get size */
    char tmp[64];
    int32_t n = procfs_read(path, tmp, sizeof(tmp));
    if (n >= 0) {
        if (type) *type = RAMFS_FILE;
        if (size) *size = (uint32_t)n;
        return 0;
    }
    return -1;
}

void procfs_list(void) {
    /* List /proc directory */
    terminal_print_colored("net", 0x09);
    terminal_print_colored("/", 0x09);
    kprintf("  (0 bytes)\n");

    /* Show active PIDs as directories */
    task_t* all = task_get_all();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (all[i].active) {
            char num[8];
            ksnprintf(num, sizeof(num), "%u", all[i].id);
            terminal_print_colored(num, 0x09);
            terminal_print_colored("/", 0x09);
            kprintf("  (0 bytes)\n");
        }
    }

    /* Virtual files */
    for (int i = 0; proc_files[i].name; i++) {
        kprintf("%s  (virtual)\n", proc_files[i].name);
    }
}

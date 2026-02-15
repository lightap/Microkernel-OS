#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"
#include "rtc.h"
#include "pmm.h"
#include "heap.h"
#include "task.h"
#include "ipc.h"
#include "ramfs.h"
#include "speaker.h"
#include "serial.h"
#include "power.h"
#include "cpuid.h"
#include "pci.h"
#include "paging.h"
#include "env.h"
#include "syscall.h"
#include "login.h"
#include "editor.h"
#include "games.h"
#include "screensaver.h"
#include "net.h"
#include "gui.h"
#include "ata.h"
#include "fat16.h"
#include "ntfs.h"
#include "procfs.h"
#include "gl_demo.h"
#include "elf.h"
#include "server.h"

#define CMD_MAX 256
#define HISTORY_SIZE 32
#define MAX_ARGS 16
#define OUTPUT_BUF_SIZE 4096

static char history[HISTORY_SIZE][CMD_MAX];
static int  history_count = 0;

/* Output capture for pipes/redirection */
static char output_buffer[OUTPUT_BUF_SIZE];
static int  output_pos = 0;

static void capture_hook(char c) {
    if (output_pos < OUTPUT_BUF_SIZE - 1)
        output_buffer[output_pos++] = c;
}

static void start_capture(void) { output_pos = 0; terminal_set_hook(capture_hook); }
static void stop_capture(void)  { terminal_set_hook(NULL); output_buffer[output_pos] = '\0'; }

/* Tokenize command */
static int tokenize(char* cmd, char* argv[]) {
    int argc = 0;
    char* p = cmd;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') { p++; argv[argc++] = p; while (*p && *p != '"') p++; }
        else { argv[argc++] = p; while (*p && *p != ' ' && *p != '\t') p++; }
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* History */
static void add_history(const char* cmd) {
    if (!cmd[0]) return;
    if (history_count > 0 && strcmp(history[(history_count-1) % HISTORY_SIZE], cmd) == 0) return;
    strcpy(history[history_count % HISTORY_SIZE], cmd);
    history_count++;
}

/* Simple wildcard matching */
static bool glob_match(const char* pattern, const char* str) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;
            while (*str) { if (glob_match(pattern, str)) return true; str++; }
            return false;
        }
        if (*pattern == '?' || *pattern == *str) { pattern++; str++; }
        else return false;
    }
    return *str == '\0';
}

/* Tab completion - find matching files in CWD */
static void tab_complete(char* buf, size_t* pos, size_t* len) {
    /* Find the word being completed */
    int word_start = *pos;
    while (word_start > 0 && buf[word_start-1] != ' ') word_start--;
    char prefix[64];
    int plen = *pos - word_start;
    if (plen <= 0 || plen >= 63) return;
    strncpy(prefix, buf + word_start, plen);
    prefix[plen] = '\0';

    /* Search for matches in CWD */
    char matches[8][RAMFS_MAX_NAME];
    int match_count = 0;

    /* Check if it's a command (first word) or file */
    extern const char* cmd_names[];
    if (word_start == 0) {
        /* Complete commands */
        for (int i = 0; cmd_names[i]; i++) {
            if (strncmp(cmd_names[i], prefix, plen) == 0 && match_count < 8)
                strcpy(matches[match_count++], cmd_names[i]);
        }
    }

    /* Also complete filenames */
    /* Simple: we'd search ramfs here - just complete from CWD */
    int32_t cwd_idx = ramfs_find(ramfs_get_cwd());
    if (cwd_idx >= 0) {
        /* Access internals via ramfs_find */
        for (int i = 0; i < RAMFS_MAX_FILES; i++) {
            ramfs_type_t type;
            uint32_t sz;
            /* We need to check each file - use a helper */
            char path[128];
            strcpy(path, ramfs_get_cwd());
            if (path[strlen(path)-1] != '/') strcat(path, "/");
            /* This is limited without direct access - just use what we have */
            (void)type; (void)sz; (void)path;
        }
    }

    if (match_count == 1) {
        /* Single match - complete it */
        const char* rest = matches[0] + plen;
        while (*rest && *len < CMD_MAX - 2) {
            /* Insert character at pos */
            for (size_t i = *len; i > *pos; i--) buf[i] = buf[i-1];
            buf[*pos] = *rest;
            (*pos)++; (*len)++;
            terminal_putchar(*rest);
            rest++;
        }
        buf[*len] = '\0';
    } else if (match_count > 1) {
        /* Show all matches */
        kprintf("\n");
        for (int i = 0; i < match_count; i++) kprintf("  %s", matches[i]);
        kprintf("\n");
    }
}

/* Enhanced readline with tab completion and history */
static void shell_readline(char* buf, size_t max) {
    size_t pos = 0, len = 0;
    int hist_idx = history_count;
    buf[0] = '\0';

    while (1) {
        unsigned char c = (unsigned char)keyboard_getchar();

        if (c == '\n') { terminal_putchar('\n'); buf[len] = '\0'; return; }
        else if (c == '\b') {
            if (pos > 0) {
                for (size_t i = pos-1; i < len-1; i++) buf[i] = buf[i+1];
                pos--; len--;
                terminal_backspace();
                int r, cl; terminal_get_cursor(&r, &cl);
                for (size_t i = pos; i < len; i++) terminal_putchar(buf[i]);
                terminal_putchar(' ');
                terminal_set_cursor(r, cl);
            }
        } else if (c == '\t') {
            tab_complete(buf, &pos, &len);
        } else if (c == KEY_UP) {
            if (hist_idx > 0 && hist_idx > history_count - HISTORY_SIZE) {
                hist_idx--;
                /* Clear current line */
                while (pos > 0) { terminal_backspace(); pos--; }
                for (size_t i = 0; i < len; i++) terminal_putchar(' ');
                for (size_t i = 0; i < len; i++) terminal_backspace();
                strcpy(buf, history[hist_idx % HISTORY_SIZE]);
                len = pos = strlen(buf);
                terminal_print(buf);
            }
        } else if (c == KEY_DOWN) {
            if (hist_idx < history_count - 1) {
                hist_idx++;
                while (pos > 0) { terminal_backspace(); pos--; }
                for (size_t i = 0; i < len; i++) terminal_putchar(' ');
                for (size_t i = 0; i < len; i++) terminal_backspace();
                strcpy(buf, history[hist_idx % HISTORY_SIZE]);
                len = pos = strlen(buf);
                terminal_print(buf);
            } else if (hist_idx == history_count - 1) {
                hist_idx = history_count;
                while (pos > 0) { terminal_backspace(); pos--; }
                for (size_t i = 0; i < len; i++) terminal_putchar(' ');
                for (size_t i = 0; i < len; i++) terminal_backspace();
                buf[0] = '\0'; len = pos = 0;
            }
        } else if (c == KEY_LEFT && pos > 0) {
            pos--;
            int r, cl; terminal_get_cursor(&r, &cl);
            terminal_set_cursor(r, cl - 1);
        } else if (c == KEY_RIGHT && pos < len) {
            pos++;
            int r, cl; terminal_get_cursor(&r, &cl);
            terminal_set_cursor(r, cl + 1);
        } else if (c == KEY_HOME) {
            int r, cl; terminal_get_cursor(&r, &cl);
            terminal_set_cursor(r, cl - pos);
            pos = 0;
        } else if (c == KEY_END) {
            int r, cl; terminal_get_cursor(&r, &cl);
            terminal_set_cursor(r, cl + (len - pos));
            pos = len;
        } else if (c == 3) { /* Ctrl+C */
            kprintf("^C\n"); buf[0] = '\0'; return;
        } else if (c == 12) { /* Ctrl+L */
            terminal_clear(); buf[0] = '\0'; return;
        } else if (c >= 32 && c != 127 && len < max - 1) {
            for (size_t i = len; i > pos; i--) buf[i] = buf[i-1];
            buf[pos] = c; len++; pos++;
            terminal_putchar(c);
            int r, cl; terminal_get_cursor(&r, &cl);
            for (size_t i = pos; i < len; i++) terminal_putchar(buf[i]);
            terminal_set_cursor(r, cl);
        }
        buf[len] = '\0';
    }
}

/* Forward declarations */
static void execute_command(char* cmdline);

/* ====== COMMAND IMPLEMENTATIONS ====== */

static void cmd_help(int argc, char* argv[]) {
    (void)argc; (void)argv;
    uint8_t d = 0x07, g = 0x0A;
    kprintf("\n");
    terminal_print_colored("  === MicroKernel Shell v0.3 ===\n\n", 0x0B);

    terminal_print_colored("  SYSTEM\n", g);
    terminal_print_colored("    help clear uname uptime date sysinfo reboot shutdown\n", d);
    terminal_print_colored("    cpuid lspci paging whoami login logout users adduser passwd\n\n", d);

    terminal_print_colored("  MEMORY\n", g);
    terminal_print_colored("    mem heap alloc\n\n", d);

    terminal_print_colored("  PROCESSES & IPC\n", g);
    terminal_print_colored("    ps kill ipc services mkport send recv syscall scheduler\n", d);
    terminal_print_colored("    exec <elf> - run ELF binary in isolated address space\n\n", d);

    terminal_print_colored("  FILESYSTEM\n", g);
    terminal_print_colored("    ls cd pwd cat more head tail grep find\n", d);
    terminal_print_colored("    touch mkdir write cp mv rm tree hex wc\n", d);
    terminal_print_colored("    /proc/*: cpuinfo meminfo uptime version stat processes\n\n", d);

    terminal_print_colored("  DISK STORAGE\n", g);
    terminal_print_colored("    disk      - show ATA disk and filesystem info\n", d);
    terminal_print_colored("    format -y - format disk with FAT16\n", d);
    terminal_print_colored("    mount     - mount filesystem at /disk/ (FAT16/NTFS)\n", d);
    terminal_print_colored("    save <file> [name] - copy RAM file to disk\n", d);
    terminal_print_colored("    load <file> [path] - copy disk file to RAM\n", d);
    terminal_print_colored("    ntfsinfo  - show NTFS filesystem details\n", d);
    terminal_print_colored("    ls /disk  cat /disk/file  write /disk/file ...\n\n", d);

    terminal_print_colored("  NETWORK\n", g);
    terminal_print_colored("    ifconfig ping arp nslookup dns\n\n", d);

    terminal_print_colored("  TOOLS\n", g);
    terminal_print_colored("    edit echo beep color calc history env export unset\n\n", d);

    terminal_print_colored("  FUN\n", g);
    terminal_print_colored("    snake 2048 matrix starfield pipes logo\n\n", d);

    terminal_print_colored("  DESKTOP\n", g);
    terminal_print_colored("    gui / startx  - launch graphical desktop\n", d);
    terminal_print_colored("    gl / opengl   - 3D OpenGL demo (ESC to exit)\n\n", d);

    terminal_print_colored("  SHELL FEATURES\n", g);
    terminal_print_colored("    Tab completion, history (up/down), pipes (|)\n", d);
    terminal_print_colored("    Redirection (> >>), variables ($VAR), scripts (sh file)\n\n", d);
}

static void cmd_clear(int ac, char** av) { (void)ac; (void)av; terminal_clear(); }

static void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) { if (i>1) terminal_putchar(' '); kprintf("%s", argv[i]); }
    kprintf("\n");
}

static void cmd_uname(int argc, char** argv) {
    bool all = (argc > 1 && strcmp(argv[1], "-a") == 0);
    kprintf("microkernel 0.3.0 x86 i686");
    if (all) { kprintf(" "); cpuid_get_info(); kprintf("%s", cpuid_get_info()->vendor); }
    kprintf("\n");
}

static void cmd_uptime(int ac, char** av) {
    (void)ac; (void)av;
    uint32_t h, m, s; timer_get_uptime(&h, &m, &s);
    kprintf("up %u:%d:%d (%u ticks)\n", h, m, s, timer_get_ticks());
}

static void cmd_date(int ac, char** av) {
    (void)ac; (void)av;
    rtc_time_t t; rtc_read(&t);
    static const char* w[] = {"?","Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* mo[] = {"?","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    kprintf("%s %s %u %d:%d:%d %u\n", w[t.weekday%8], mo[t.month<=12?t.month:0], t.day, t.hour, t.minute, t.second, t.year);
}

static void cmd_sysinfo(int ac, char** av) {
    (void)ac; (void)av;
    rtc_time_t t; rtc_read(&t);
    uint32_t h, m, s; timer_get_uptime(&h, &m, &s);
    terminal_print_colored("\n  System Information\n  ------------------\n", 0x0B);
    kprintf("  OS:      microkernel v0.3.0 x86\n");
    cpuid_print();
    kprintf("  Uptime:  %u:%d:%d\n", h, m, s);
    kprintf("  Memory:  %u MB total, %u MB free\n", pmm_get_total_pages()*4/1024, pmm_get_free_pages()*4/1024);
    kprintf("  Heap:    %u used, %u free\n", heap_used_space(), heap_free_space());
    kprintf("  Tasks:   %u  Services: %u  Files: %u\n", task_count(), ipc_port_count(), ramfs_file_count());
    kprintf("  PCI:     %u devices\n", pci_device_count());
    kprintf("  Network: %s\n", net_is_available() ? "RTL8139 (up)" : "not available");
    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        if (ata_drive_present(i)) {
            ata_drive_t* d = ata_get_drive_n(i);
            const char* fs = "no filesystem";
            if (fat16_is_mounted() && fat16_get_drive_idx() == i) fs = "FAT16";
            else if (ntfs_is_mounted() && ntfs_get_drive_idx() == i) fs = "NTFS";
            kprintf("  Disk %d:  %s (%u MB) [%s → /disk%s]\n", i, d->model, d->size_mb,
                    fs, i == 0 ? "" : "2");
        }
    }
    if (ata_drive_count() == 0) kprintf("  Disk:    none\n");
    paging_print_info();
    kprintf("  User:    %s\n\n", login_current_user());
}

static void cmd_cpuid(int ac, char** av) { (void)ac; (void)av; cpuid_print(); }
static void cmd_lspci(int ac, char** av) { (void)ac; (void)av; pci_list(); }
static void cmd_paging(int ac, char** av) { (void)ac; (void)av; paging_print_info(); }

static void cmd_mem(int ac, char** av) {
    (void)ac; (void)av;
    uint32_t total=pmm_get_total_pages()*4, used=pmm_get_used_pages()*4, fr=pmm_get_free_pages()*4;
    kprintf("Physical: %uMB total, %uMB used, %uMB free\n", total/1024, used/1024, fr/1024);
    kprintf("  ["); uint32_t bw=40, fl=(used*bw)/total;
    for(uint32_t i=0;i<bw;i++){if(i<fl)terminal_print_colored("#",0x0C);else terminal_print_colored("-",0x02);}
    kprintf("] %u%%\n",(used*100)/total);
}

static void cmd_heap(int ac, char** av) { (void)ac; (void)av; heap_dump(); }

static void cmd_alloc(int argc, char** argv) {
    if(argc<2){kprintf("Usage: alloc <bytes>\n");return;}
    void* p=kmalloc(atoi(argv[1]));
    if(p) kprintf("Allocated at %x\n",(uint32_t)p); else kprintf("Failed!\n");
}

static void cmd_ps(int ac, char** av) { (void)ac; (void)av; task_list(); }

static void cmd_kill(int argc, char** argv) {
    if(argc<2){kprintf("Usage: kill <pid>\n");return;}
    uint32_t pid=atoi(argv[1]); if(!pid){kprintf("Can't kill PID 0\n");return;}
    task_kill(pid); kprintf("Killed %u\n",pid);
}

static void cmd_ipc(int ac, char** av) { (void)ac; (void)av; ipc_status(); }

static void cmd_mkport(int argc, char** argv) {
    if(argc<2){kprintf("Usage: mkport <name>\n");return;}
    task_t* t = task_get_current();
    int32_t p=ipc_register_service(argv[1], t ? t->id : 0);
    if(p>=0) kprintf("Service '%s' registered\n",argv[1]); else kprintf("Failed\n");
}

static void cmd_services(int ac, char** av) { (void)ac; (void)av; ipc_service_list(); }

static void cmd_send(int argc, char** argv) {
    if(argc<3){kprintf("Usage: send <pid> <msg>\n");return;}
    message_t msg; memset(&msg,0,sizeof(msg));
    msg.type=MSG_PING;
    char text[48]=""; for(int i=2;i<argc;i++){if(i>2)strcat(text," ");strcat(text,argv[i]);}
    strncpy((char*)msg.raw,text,sizeof(msg.raw)-1);
    int32_t r=ipc_send(atoi(argv[1]),&msg);
    if(r==0) kprintf("Sent to PID %s\n",argv[1]); else kprintf("Error %d\n",r);
}

static void cmd_recv(int argc, char** argv) {
    if(argc<2){kprintf("Usage: recv <from_pid|any>\n");return;}
    uint32_t from = strcmp(argv[1],"any")==0 ? PID_ANY : (uint32_t)atoi(argv[1]);
    message_t msg; memset(&msg,0,sizeof(msg));
    int32_t r=ipc_receive(from,&msg);
    if(r==0){kprintf("From PID %u, type %u: \"%s\"\n",msg.sender,msg.type,(char*)msg.raw);}
    else kprintf("Error %d\n",r);
}

static void cmd_syscall_test(int ac, char** av) {
    (void)ac; (void)av;
    kprintf("Testing INT 0x80 syscalls...\n");
    const char* msg = "  Hello from syscall!\n";
    sys_write(msg, strlen(msg));
    kprintf("  PID via syscall: %d\n", sys_getpid());
    kprintf("  Syscall interface operational.\n");
}

/* Filesystem commands */
static void cmd_ls(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : ramfs_get_cwd();
    /* Check for wildcard */
    if (argc > 1 && strchr(argv[1], '*')) {
        /* Wildcard listing - match files in CWD */
        kprintf("Matching '%s':\n", argv[1]);
        /* Simple: list files matching pattern from CWD */
        /* Would need ramfs iteration API for full implementation */
        ramfs_list(ramfs_get_cwd());
    } else {
        ramfs_list(path);
    }
}

static void cmd_cd(int argc, char** argv) {
    if(argc<2){ramfs_set_cwd("/");return;}
    char resolved[128]; ramfs_resolve_path(argv[1],resolved);
    if(ramfs_find(resolved)>=0) ramfs_set_cwd(resolved);
    else kprintf("cd: %s: not found\n",argv[1]);
}

static void cmd_pwd(int ac, char** av) { (void)ac; (void)av; kprintf("%s\n",ramfs_get_cwd()); }

static void cmd_cat(int argc, char** argv) {
    if(argc<2){kprintf("Usage: cat <file>\n");return;}
    char buf[RAMFS_MAX_DATA+1]; int32_t n=ramfs_read(argv[1],buf,RAMFS_MAX_DATA);
    if(n<0){kprintf("cat: %s: not found\n",argv[1]);return;}
    buf[n]='\0'; kprintf("%s",buf); if(n>0&&buf[n-1]!='\n') kprintf("\n");
}

static void cmd_touch(int argc, char** argv) {
    if(argc<2){kprintf("Usage: touch <file>\n");return;}
    ramfs_create(argv[1], RAMFS_FILE);
}

static void cmd_mkdir(int argc, char** argv) {
    if(argc<2){kprintf("Usage: mkdir <dir>\n");return;}
    int32_t r=ramfs_create(argv[1],RAMFS_DIR);
    if(r<0) kprintf("mkdir: failed (%d)\n",r);
}

static void cmd_write(int argc, char** argv) {
    if(argc<3){kprintf("Usage: write <file> <text...>\n");return;}
    char buf[RAMFS_MAX_DATA]="";
    for(int i=2;i<argc;i++){if(i>2)strcat(buf," ");strcat(buf,argv[i]);}
    strcat(buf,"\n");
    int32_t r=ramfs_write(argv[1],buf,strlen(buf));
    if(r>=0) kprintf("Wrote %d bytes\n",r); else kprintf("Failed\n");
}

static void cmd_rm(int argc, char** argv) {
    if(argc<2){kprintf("Usage: rm <file>\n");return;}
    int32_t r=ramfs_delete(argv[1]);
    if(r==-1)kprintf("Not found\n"); else if(r==-2)kprintf("Dir not empty\n");
}

static void cmd_tree(int argc, char** argv) {
    terminal_print_colored(".\n",0x09);
    ramfs_tree((argc>1)?argv[1]:ramfs_get_cwd(),1);
}

static void cmd_hex(int argc, char** argv) {
    if(argc<2){kprintf("Usage: hex <file>\n");return;}
    uint8_t buf[RAMFS_MAX_DATA]; int32_t n=ramfs_read(argv[1],buf,sizeof(buf));
    if(n<0){kprintf("Not found\n");return;}
    for(int32_t i=0;i<n;i+=16){
        terminal_print_hex(i); kprintf("  ");
        for(int j=0;j<16;j++){
            if(i+j<n)kprintf("%c%c ","0123456789ABCDEF"[(buf[i+j]>>4)&0xF],"0123456789ABCDEF"[buf[i+j]&0xF]);
            else kprintf("   ");
            if(j==7)kprintf(" ");
        }
        kprintf(" |");
        for(int j=0;j<16&&i+j<n;j++){char c=buf[i+j];terminal_putchar((c>=32&&c<127)?c:'.');}
        kprintf("|\n");
    }
}

static void cmd_wc(int argc, char** argv) {
    if(argc<2){kprintf("Usage: wc <file>\n");return;}
    char buf[RAMFS_MAX_DATA]; int32_t n=ramfs_read(argv[1],buf,sizeof(buf));
    if(n<0){kprintf("Not found\n");return;}
    int lines=0,words=0; bool in_word=false;
    for(int32_t i=0;i<n;i++){
        if(buf[i]=='\n')lines++;
        if(isspace(buf[i])){in_word=false;}else{if(!in_word)words++;in_word=true;}
    }
    kprintf("  %d lines, %d words, %d bytes  %s\n",lines,words,n,argv[1]);
}

static void cmd_cp(int argc, char** argv) {
    if (argc < 3) { kprintf("Usage: cp <src> <dest>\n"); return; }
    char buf[RAMFS_MAX_DATA];
    int32_t n = ramfs_read(argv[1], buf, sizeof(buf));
    if (n < 0) { kprintf("cp: %s: not found\n", argv[1]); return; }

    /* If dest is a directory, append source filename */
    char dest[RAMFS_MAX_PATH];
    ramfs_resolve_path(argv[2], dest);
    int32_t di = ramfs_find(dest);
    if (di >= 0) {
        ramfs_node_t* dn = ramfs_get_node(di);
        if (dn && dn->type == RAMFS_DIR) {
            /* Extract source filename */
            const char* fname = argv[1];
            for (const char* p = argv[1]; *p; p++)
                if (*p == '/') fname = p + 1;
            if (dest[strlen(dest)-1] != '/') strcat(dest, "/");
            strcat(dest, fname);
        }
    }

    int32_t r = ramfs_write(dest, buf, n);
    if (r < 0) kprintf("cp: failed to write %s\n", dest);
}

static void cmd_mv(int argc, char** argv) {
    if (argc < 3) { kprintf("Usage: mv <src> <dest>\n"); return; }

    /* If dest is a directory, append source filename */
    char dest[RAMFS_MAX_PATH];
    ramfs_resolve_path(argv[2], dest);
    int32_t di = ramfs_find(dest);
    if (di >= 0) {
        ramfs_node_t* dn = ramfs_get_node(di);
        if (dn && dn->type == RAMFS_DIR) {
            const char* fname = argv[1];
            for (const char* p = argv[1]; *p; p++)
                if (*p == '/') fname = p + 1;
            if (dest[strlen(dest)-1] != '/') strcat(dest, "/");
            strcat(dest, fname);
        }
    }

    char src_resolved[RAMFS_MAX_PATH];
    ramfs_resolve_path(argv[1], src_resolved);
    int32_t r = ramfs_rename(src_resolved, dest);
    if (r == -1) kprintf("mv: %s: not found\n", argv[1]);
    else if (r == -4) kprintf("mv: %s: already exists\n", dest);
    else if (r < 0) kprintf("mv: failed (%d)\n", r);
}

static void cmd_head(int argc, char** argv) {
    int lines = 10;
    const char* file = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'n' && argc > i + 1)
            lines = atoi(argv[++i]);
        else if (argv[i][0] == '-' && isdigit(argv[i][1]))
            lines = atoi(argv[i] + 1);
        else
            file = argv[i];
    }
    if (!file) { kprintf("Usage: head [-n N] <file>\n"); return; }

    char buf[RAMFS_MAX_DATA + 1];
    int32_t n = ramfs_read(file, buf, RAMFS_MAX_DATA);
    if (n < 0) { kprintf("head: %s: not found\n", file); return; }
    buf[n] = '\0';

    int count = 0;
    for (int32_t i = 0; i < n && count < lines; i++) {
        terminal_putchar(buf[i]);
        if (buf[i] == '\n') count++;
    }
    if (count == 0 && n > 0 && buf[n-1] != '\n') terminal_putchar('\n');
}

static void cmd_tail(int argc, char** argv) {
    int lines = 10;
    const char* file = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'n' && argc > i + 1)
            lines = atoi(argv[++i]);
        else if (argv[i][0] == '-' && isdigit(argv[i][1]))
            lines = atoi(argv[i] + 1);
        else
            file = argv[i];
    }
    if (!file) { kprintf("Usage: tail [-n N] <file>\n"); return; }

    char buf[RAMFS_MAX_DATA + 1];
    int32_t n = ramfs_read(file, buf, RAMFS_MAX_DATA);
    if (n < 0) { kprintf("tail: %s: not found\n", file); return; }
    buf[n] = '\0';

    /* Count total newlines */
    int total = 0;
    for (int32_t i = 0; i < n; i++)
        if (buf[i] == '\n') total++;

    /* Find start position: skip (total - lines) newlines */
    int skip = total - lines;
    int32_t start = 0;
    if (skip > 0) {
        int count = 0;
        for (int32_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                count++;
                if (count >= skip) { start = i + 1; break; }
            }
        }
    }
    for (int32_t i = start; i < n; i++)
        terminal_putchar(buf[i]);
    if (n > 0 && buf[n-1] != '\n') terminal_putchar('\n');
}

static void cmd_grep(int argc, char** argv) {
    if (argc < 2) { kprintf("Usage: grep <pattern> [file]\n"); return; }
    const char* pattern = argv[1];
    uint32_t plen = strlen(pattern);

    char buf[RAMFS_MAX_DATA + 1];
    int32_t n;

    if (argc >= 3) {
        /* Grep in file */
        n = ramfs_read(argv[2], buf, RAMFS_MAX_DATA);
        if (n < 0) { kprintf("grep: %s: not found\n", argv[2]); return; }
    } else {
        /* Grep from pipe input */
        n = ramfs_read("/tmp/.pipe", buf, RAMFS_MAX_DATA);
        if (n < 0) { kprintf("Usage: grep <pattern> <file>\n"); return; }
    }
    buf[n] = '\0';

    /* Process line by line */
    char* line = buf;
    while (*line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Search for pattern in line */
        bool found = false;
        for (char* p = line; *p; p++) {
            if (strncmp(p, pattern, plen) == 0) { found = true; break; }
        }
        if (found) {
            /* Print line with matching part highlighted */
            for (char* p = line; *p; p++) {
                if (strncmp(p, pattern, plen) == 0) {
                    terminal_print_colored(pattern, 0x0C); /* Red highlight */
                    p += plen - 1;
                } else {
                    terminal_putchar(*p);
                }
            }
            terminal_putchar('\n');
        }

        if (nl) { *nl = '\n'; line = nl + 1; }
        else break;
    }
}

static void cmd_find(int argc, char** argv) {
    const char* start_path = (argc >= 2) ? argv[1] : "/";
    const char* name_pat = NULL;

    /* Parse -name option */
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-name") == 0) {
            name_pat = argv[i + 1];
            if (!start_path || strcmp(start_path, argv[i]) == 0)
                start_path = "/";
        }
    }

    char path[RAMFS_MAX_PATH];
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        ramfs_node_t* node = ramfs_get_node(i);
        if (!node) continue;

        ramfs_get_path(i, path, sizeof(path));

        /* Check if under start_path */
        if (strncmp(path, start_path, strlen(start_path)) != 0 &&
            strcmp(start_path, "/") != 0)
            continue;

        /* Check name filter if given */
        if (name_pat) {
            /* Support simple * wildcard */
            if (strchr(name_pat, '*')) {
                if (!glob_match(name_pat, node->name)) continue;
            } else {
                /* Substring match */
                bool match = false;
                uint32_t nlen = strlen(name_pat);
                for (char* p = node->name; *p; p++) {
                    if (strncmp(p, name_pat, nlen) == 0) { match = true; break; }
                }
                if (!match) continue;
            }
        }

        if (node->type == RAMFS_DIR) {
            terminal_print_colored(path, 0x09);
            terminal_print_colored("/\n", 0x09);
        } else {
            kprintf("%s\n", path);
        }
    }
}

/* Network */
static ip_addr_t parse_ip(const char* s) {
    ip_addr_t ip={{0,0,0,0}}; int octet=0,val=0;
    while(*s){
        if(*s=='.'){if(octet<4)ip.b[octet++]=val;val=0;}
        else if(isdigit(*s)){val=val*10+(*s-'0');}
        s++;
    }
    if(octet<4) ip.b[octet]=val;
    return ip;
}

static void cmd_ifconfig(int ac, char** av) { (void)ac; (void)av; net_ifconfig(); }
static void cmd_arp(int ac, char** av) { (void)ac; (void)av; net_arp_table(); }

static void cmd_ping(int argc, char** argv) {
    if(argc<2){kprintf("Usage: ping <ip>\n");return;}
    ip_addr_t target=parse_ip(argv[1]);
    int count=(argc>2)?atoi(argv[2]):4;
    kprintf("PING %d.%d.%d.%d\n",target.b[0],target.b[1],target.b[2],target.b[3]);
    int ok=0;
    for(int i=0;i<count;i++){
        uint32_t rtt;
        if(net_ping(target,2000,&rtt)){
            kprintf("  Reply from %d.%d.%d.%d: time=%ums\n",target.b[0],target.b[1],target.b[2],target.b[3],rtt);
            ok++;
        } else {
            kprintf("  Request timed out.\n");
        }
    }
    kprintf("--- %d packets sent, %d received ---\n",count,ok);
}

/* DNS lookup */
static void cmd_nslookup(int argc, char** argv) {
    if (argc < 2) { kprintf("Usage: nslookup <hostname>\n"); return; }
    if (!net_is_available()) { kprintf("  No network interface.\n"); return; }

    ip_addr_t dns;
    net_dns_get_server(&dns);
    kprintf("Server:  %d.%d.%d.%d\n", dns.b[0], dns.b[1], dns.b[2], dns.b[3]);
    kprintf("Name:    %s\n", argv[1]);

    ip_addr_t result;
    if (net_dns_resolve(argv[1], &result, 3000)) {
        kprintf("Address: %d.%d.%d.%d\n", result.b[0], result.b[1], result.b[2], result.b[3]);
    } else {
        kprintf("  ** DNS lookup failed (timeout or no response) **\n");
    }
}

/* Set DNS server */
static void cmd_dns(int argc, char** argv) {
    if (argc < 2) {
        ip_addr_t dns;
        net_dns_get_server(&dns);
        kprintf("  DNS server: %d.%d.%d.%d\n", dns.b[0], dns.b[1], dns.b[2], dns.b[3]);
        kprintf("  Usage: dns <ip>  (set DNS server)\n");
        return;
    }
    ip_addr_t ip = parse_ip(argv[1]);
    net_set_dns(ip.b[0], ip.b[1], ip.b[2], ip.b[3]);
    kprintf("  DNS server set to %d.%d.%d.%d\n", ip.b[0], ip.b[1], ip.b[2], ip.b[3]);
}

/* Scheduler control */
static void cmd_scheduler(int argc, char** argv) {
    if (argc < 2) {
        kprintf("  Mode:     %s\n", task_is_preemptive() ? "preemptive" : "cooperative");
        kprintf("  Quantum:  %u ticks (%u ms)\n", task_get_quantum(),
                task_get_quantum() * 1000 / timer_get_frequency());
        kprintf("  Switches: %u\n", task_total_switches());
        kprintf("\n  Usage:\n");
        kprintf("    scheduler preemptive   - enable preemptive scheduling\n");
        kprintf("    scheduler cooperative  - switch to cooperative only\n");
        kprintf("    scheduler quantum <n>  - set time slice to n ticks\n");
        return;
    }
    if (strcmp(argv[1], "preemptive") == 0 || strcmp(argv[1], "on") == 0) {
        task_set_preemptive(true);
        kprintf("  Scheduler set to preemptive.\n");
    } else if (strcmp(argv[1], "cooperative") == 0 || strcmp(argv[1], "off") == 0) {
        task_set_preemptive(false);
        kprintf("  Scheduler set to cooperative.\n");
    } else if (strcmp(argv[1], "quantum") == 0 && argc > 2) {
        uint32_t q = atoi(argv[2]);
        task_set_quantum(q);
        kprintf("  Quantum set to %u ticks (%u ms).\n", q, q * 1000 / timer_get_frequency());
    } else {
        kprintf("  Unknown option: %s\n", argv[1]);
    }
}

/* ====== DISK COMMANDS ====== */
static void cmd_disk(int ac, char** av) {
    (void)ac; (void)av;
    kprintf("\n");
    ata_print_info();
    kprintf("\n");
    if (fat16_is_mounted()) fat16_print_info();
    if (ntfs_is_mounted()) ntfs_print_info();
    if (!fat16_is_mounted() && !ntfs_is_mounted())
        kprintf("  No filesystem mounted\n");
    kprintf("\n");
}

static void cmd_format(int ac, char** av) {
    (void)av;
    if (!ata_available()) {
        kprintf("  No ATA disk. Run QEMU with: -hda disk.img\n");
        kprintf("  Create image: dd if=/dev/zero of=disk.img bs=1M count=64\n");
        return;
    }
    ata_drive_t* d = ata_get_drive();
    if (ac > 1 && strcmp(av[1], "-y") != 0) {
        kprintf("  Usage: format [-y]\n");
        return;
    }
    if (ac <= 1 || strcmp(av[1], "-y") != 0) {
        kprintf("  WARNING: This will erase all data on %s (%u MB)!\n",
                d->model, d->size_mb);
        kprintf("  Type 'format -y' to confirm.\n");
        return;
    }
    kprintf("  Formatting %u MB disk...\n", d->size_mb);
    if (fat16_format(d->sectors, "MICROKERNEL")) {
        kprintf("  Mounting...\n");
        if (fat16_mount()) {
            kprintf("  FAT16 filesystem ready. Use /disk/ to access.\n");
        }
    } else {
        kprintf("  Format failed!\n");
    }
}

static void cmd_mount(int ac, char** av) {
    (void)ac; (void)av;
    if (ata_drive_count() == 0) {
        kprintf("  No ATA disks detected.\n");
        return;
    }
    bool any = false;
    for (int drv = 0; drv < ATA_MAX_DRIVES; drv++) {
        if (!ata_drive_present(drv)) continue;
        const char* mnt = (drv == 0) ? "/disk" : "/disk2";
        /* Check if already mounted */
        if (fat16_is_mounted() && fat16_get_drive_idx() == drv) {
            kprintf("  FAT16 already mounted at %s/\n", mnt); any = true; continue;
        }
        if (ntfs_is_mounted() && ntfs_get_drive_idx() == drv) {
            kprintf("  NTFS already mounted at %s/\n", mnt); any = true; continue;
        }
        /* Try to mount */
        if (!fat16_is_mounted() && fat16_mount_drive(drv)) {
            fat16_info_t* fi = fat16_get_info();
            kprintf("  Mounted FAT16: %s (%u MB) at %s/\n", fi->volume_label,
                    (fi->total_clusters * fi->cluster_size) / (1024*1024), mnt);
            any = true;
        } else if (!ntfs_is_mounted() && ntfs_mount_drive(drv)) {
            ntfs_info_t* ni = ntfs_get_info();
            kprintf("  Mounted NTFS: %s (%u MB) at %s/\n", ni->volume_label,
                    (uint32_t)((ni->total_sectors * ni->bytes_per_sector) >> 20), mnt);
            any = true;
        } else {
            kprintf("  Drive %d: no FAT16 or NTFS filesystem\n", drv);
        }
    }
    if (!any) kprintf("  No filesystems mounted. Use 'format -y' for FAT16.\n");
}

static void cmd_umount(int ac, char** av) {
    (void)ac; (void)av;
    bool any = false;
    if (fat16_is_mounted()) {
        int d = fat16_get_drive_idx();
        fat16_unmount();
        kprintf("  /disk%s unmounted (FAT16).\n", d == 0 ? "" : "2");
        any = true;
    }
    if (ntfs_is_mounted()) {
        int d = ntfs_get_drive_idx();
        ntfs_unmount();
        kprintf("  /disk%s unmounted (NTFS).\n", d == 0 ? "" : "2");
        any = true;
    }
    if (!any) kprintf("  Nothing mounted.\n");
}

static void cmd_ntfsinfo(int ac, char** av) {
    (void)ac; (void)av;
    if (!ntfs_is_mounted()) {
        kprintf("  NTFS not mounted.\n");
        if (fat16_is_mounted())
            kprintf("  (FAT16 is currently mounted)\n");
        return;
    }
    ntfs_print_info();
}

static void cmd_save(int argc, char** argv) {
    if (argc < 2) {
        kprintf("  Usage: save <ramfs-path> [disk-name | /disk2/name]\n");
        kprintf("  Copies a file from RAM to disk.\n");
        kprintf("  Defaults to first mounted disk. Use /disk2/ prefix for second disk.\n");
        kprintf("  Example: save /home/user/photo.png photo.png\n");
        kprintf("  Example: save /home/user/photo.png /disk2/photo.png\n");
        return;
    }
    if (!fat16_is_mounted() && !ntfs_is_mounted()) {
        kprintf("  No disk mounted. Use 'mount' first.\n");
        return;
    }
    /* Read from ramfs */
    ramfs_type_t type; uint32_t size;
    if (ramfs_stat(argv[1], &type, &size) < 0) {
        kprintf("  File not found: %s\n", argv[1]);
        return;
    }
    if (type != RAMFS_FILE) {
        kprintf("  Not a file: %s\n", argv[1]);
        return;
    }
    uint8_t* buf = kmalloc(size);
    if (!buf) { kprintf("  Out of memory\n"); return; }
    int32_t rd = ramfs_read(argv[1], buf, size);
    if (rd <= 0) { kfree(buf); kprintf("  Read failed\n"); return; }

    /* Determine target: /disk2/name or default disk */
    char full_path[256];
    if (argc >= 3 && strncmp(argv[2], "/disk", 5) == 0) {
        /* User gave full disk path like /disk2/photo.png */
        ksnprintf(full_path, sizeof(full_path), "%s", argv[2]);
    } else {
        /* Extract filename, save to first mounted disk */
        const char* disk_name;
        if (argc >= 3) {
            disk_name = argv[2];
        } else {
            disk_name = argv[1];
            const char* p = argv[1];
            while (*p) { if (*p == '/') disk_name = p + 1; p++; }
        }
        /* Pick which mount point */
        const char* mnt = "/disk";
        if (fat16_is_mounted()) mnt = (fat16_get_drive_idx() == 0) ? "/disk" : "/disk2";
        else if (ntfs_is_mounted()) mnt = (ntfs_get_drive_idx() == 0) ? "/disk" : "/disk2";
        ksnprintf(full_path, sizeof(full_path), "%s/%s", mnt, disk_name);
    }

    /* Write through ramfs dispatch (handles both FS types) */
    int32_t wr = ramfs_write(full_path, buf, (uint32_t)rd);
    kfree(buf);

    if (wr > 0) {
        kprintf("  Saved %u bytes to %s\n", wr, full_path);
    } else {
        kprintf("  Write failed (disk full?)\n");
    }
}

static void cmd_load(int argc, char** argv) {
    if (argc < 2) {
        kprintf("  Usage: load <disk-file> [ramfs-path]\n");
        kprintf("         load /disk2/<file> [ramfs-path]\n");
        kprintf("  Copies a file from disk to RAM filesystem\n");
        kprintf("  Example: load photo.png /home/user/photo.png\n");
        kprintf("  Example: load /disk2/photo.png\n");
        return;
    }
    if (!fat16_is_mounted() && !ntfs_is_mounted()) {
        kprintf("  No disk mounted. Use 'mount' first.\n");
        return;
    }

    /* Build full disk path */
    char full_path[256];
    const char* file_name = argv[1];
    if (strncmp(argv[1], "/disk", 5) == 0) {
        /* User gave full path like /disk2/photo.png */
        ksnprintf(full_path, sizeof(full_path), "%s", argv[1]);
        /* Extract just the filename for ramfs destination */
        const char* p = argv[1];
        while (*p) { if (*p == '/') file_name = p + 1; p++; }
    } else {
        /* Default: first mounted disk */
        const char* mnt = "/disk";
        if (fat16_is_mounted()) mnt = (fat16_get_drive_idx() == 0) ? "/disk" : "/disk2";
        else if (ntfs_is_mounted()) mnt = (ntfs_get_drive_idx() == 0) ? "/disk" : "/disk2";
        ksnprintf(full_path, sizeof(full_path), "%s/%s", mnt, argv[1]);
    }

    /* Read through ramfs dispatch */
    ramfs_type_t type; uint32_t fsize;
    if (ramfs_stat(full_path, &type, &fsize) < 0) {
        kprintf("  File not found: %s\n", full_path);
        return;
    }
    if (type != RAMFS_FILE) {
        kprintf("  Not a file: %s\n", full_path);
        return;
    }

    uint8_t* buf = kmalloc(fsize);
    if (!buf) { kprintf("  Out of memory\n"); return; }
    int32_t rd = ramfs_read(full_path, buf, fsize);
    if (rd <= 0) { kfree(buf); kprintf("  Read failed\n"); return; }

    /* Determine ramfs destination */
    char rpath[256];
    if (argc >= 3) {
        strncpy(rpath, argv[2], sizeof(rpath) - 1);
    } else {
        ksnprintf(rpath, sizeof(rpath), "/home/%s/%s", login_current_user(), file_name);
    }

    int32_t wr = ramfs_write(rpath, buf, (uint32_t)rd);
    kfree(buf);

    if (wr > 0) {
        kprintf("  Loaded %u bytes from %s to %s\n", wr, full_path, rpath);
    } else {
        kprintf("  Write to ramfs failed\n");
    }
}

/* Execute an ELF binary from the filesystem */
static void cmd_exec(int argc, char** argv) {
    if (argc < 2) {
        kprintf("  Usage: exec <path> [options]\n");
        kprintf("  Options:\n");
        kprintf("    -n <name>   Process name (default: filename)\n");
        kprintf("    -io         Grant I/O port access (IOPL=3)\n");
        kprintf("    -vga        Map VGA framebuffer\n");
        kprintf("    -p <0-3>    Priority (default: 2)\n");
        kprintf("  Example: exec /home/user/hello.elf\n");
        kprintf("           exec /disk/server.elf -io -n myserver\n");
        return;
    }

    /* Parse options */
    const char* path = argv[1];
    const char* name = NULL;
    bool io_priv = false;
    uint32_t flags = 0;
    uint32_t priority = 10;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-io") == 0) {
            io_priv = true;
        } else if (strcmp(argv[i], "-vga") == 0) {
            flags |= ELF_MAP_VGA;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            priority = atoi(argv[++i]);
            if (priority < 3) priority = 3;
        }
    }

    /* Derive process name from filename if not given */
    char namebuf[32];
    if (!name) {
        const char* p = path;
        const char* base = path;
        while (*p) { if (*p == '/') base = p + 1; p++; }
        strncpy(namebuf, base, sizeof(namebuf) - 1);
        namebuf[sizeof(namebuf) - 1] = '\0';
        /* Strip .elf extension if present */
        size_t len = strlen(namebuf);
        if (len > 4 && strcmp(namebuf + len - 4, ".elf") == 0)
            namebuf[len - 4] = '\0';
        name = namebuf;
    }

    /* Stat the file to get its size */
    ramfs_type_t type;
    uint32_t fsize;
    if (ramfs_stat(path, &type, &fsize) < 0) {
        kprintf("  File not found: %s\n", path);
        return;
    }
    if (type != RAMFS_FILE) {
        kprintf("  Not a file: %s\n", path);
        return;
    }
    if (fsize < sizeof(elf32_ehdr_t)) {
        kprintf("  File too small to be an ELF binary (%u bytes)\n", fsize);
        return;
    }

    /* Read the file into memory */
    uint8_t* buf = kmalloc(fsize);
    if (!buf) {
        kprintf("  Out of memory (need %u bytes)\n", fsize);
        return;
    }
    int32_t rd = ramfs_read(path, buf, fsize);
    if (rd <= 0) {
        kfree(buf);
        kprintf("  Failed to read file\n");
        return;
    }

    /* Validate and load */
    if (elf_validate(buf, (uint32_t)rd) != 0) {
        kfree(buf);
        kprintf("  Not a valid ELF binary (need 32-bit x86 executable, linked at 0x%x+)\n",
                USER_BASE);
        return;
    }

    kprintf("  Loading '%s' (%u bytes)...\n", name, rd);
    int32_t pid = elf_load(buf, (uint32_t)rd, name, priority, io_priv, flags);
    kfree(buf);

    if (pid < 0) {
        kprintf("  Failed to load ELF binary\n");
        return;
    }

    kprintf("  [");
    terminal_print_colored("OK", 0x0A);
    kprintf("] Process '%s' started (PID %u, isolated", name, pid);
    if (io_priv) kprintf(", IOPL=3");
    if (flags & ELF_MAP_VGA) kprintf(", VGA mapped");
    kprintf(")\n");
}

/* Login/user commands */
static void cmd_whoami(int ac, char** av) { (void)ac; (void)av; kprintf("%s\n",login_current_user()); }
static void cmd_users(int ac, char** av) { (void)ac; (void)av; login_list_users(); }

static void cmd_adduser(int argc, char** argv) {
    if(argc<3){kprintf("Usage: adduser <name> <password>\n");return;}
    if(login_add_user(argv[1],argv[2])) kprintf("User '%s' created\n",argv[1]);
    else kprintf("Failed (exists or full)\n");
}

static void cmd_passwd(int argc, char** argv) {
    if(argc<3){kprintf("Usage: passwd <old> <new>\n");return;}
    if(login_change_pass(login_current_user(),argv[1],argv[2])) kprintf("Password changed\n");
    else kprintf("Wrong password\n");
}

static void cmd_login(int ac, char** av) { (void)ac; (void)av; login_prompt(); }
static void cmd_logout(int ac, char** av) { (void)ac; (void)av; login_logout(); kprintf("Logged out.\n"); login_prompt(); }

/* Env vars */
static void cmd_env(int ac, char** av) { (void)ac; (void)av; env_list(); }

static void cmd_export(int argc, char** argv) {
    if(argc<2){env_list();return;}
    char* eq=strchr(argv[1],'=');
    if(eq){*eq='\0';env_set(argv[1],eq+1);kprintf("%s=%s\n",argv[1],eq+1);}
    else if(argc>=3){env_set(argv[1],argv[2]);}
    else kprintf("Usage: export KEY=VALUE\n");
}

static void cmd_unset(int argc, char** argv) {
    if(argc<2){kprintf("Usage: unset <var>\n");return;}
    env_unset(argv[1]);
}

/* Tools */
static void cmd_beep(int argc, char** argv) {
    uint32_t f=880, d=200;
    if (argc > 1) f = atoi(argv[1]);
    if (argc > 2) d = atoi(argv[2]);
    if (f < 20) f = 20;
    if (f > 20000) f = 20000;
    speaker_beep(f, d);
}

static void cmd_color(int argc, char** argv) {
    if(argc<2){kprintf("Colors: 0-F. Usage: color <fg> [bg]\n");return;}
    uint8_t fg=0x07,bg=0x00;
    char c=toupper(argv[1][0]);
    if(c>='0'&&c<='9')fg=c-'0'; else if(c>='A'&&c<='F')fg=c-'A'+10;
    if(argc>2){c=toupper(argv[2][0]);if(c>='0'&&c<='9')bg=c-'0';else if(c>='A'&&c<='F')bg=c-'A'+10;}
    terminal_setcolor(fg|(bg<<4));
}

static void cmd_calc(int argc, char** argv) {
    if(argc<4){kprintf("Usage: calc <a> <op> <b>\n");return;}
    int a=atoi(argv[1]),b=atoi(argv[3]); char op=argv[2][0]; int r=0;
    switch(op){
        case'+':r=a+b;break; case'-':r=a-b;break; case'*':r=a*b;break;
        case'/':if(!b){kprintf("Div by 0\n");return;}r=a/b;break;
        case'%':if(!b){kprintf("Div by 0\n");return;}r=a%b;break;
        case'&':r=a&b;break; case'|':r=a|b;break; case'^':r=a^b;break;
        default:kprintf("Unknown op\n");return;
    }
    kprintf("%d %c %d = %d\n",a,op,b,r);
}

static void cmd_history(int ac, char** av) {
    (void)ac; (void)av;
    int start=(history_count>HISTORY_SIZE)?history_count-HISTORY_SIZE:0;
    for(int i=start;i<history_count;i++) kprintf("  %d  %s\n",i+1,history[i%HISTORY_SIZE]);
}

static void cmd_edit(int argc, char** argv) {
    if(argc<2){kprintf("Usage: edit <file>\n");return;}
    editor_open(argv[1]);
}

static void cmd_logo(int ac, char** av) {
    (void)ac; (void)av;
    terminal_print_colored("\n", 0x0B);
    terminal_print_colored("   __  __ _               _  __                    _\n", 0x0B);
    terminal_print_colored("  |  \\/  (_) ___ _ __ ___| |/ /___ _ __ _ __   ___| |\n", 0x0B);
    terminal_print_colored("  | |\\/| | |/ __| '__/ _ \\ ' // _ \\ '__| '_ \\ / _ \\ |\n", 0x03);
    terminal_print_colored("  | |  | | | (__| | | (_) | . \\  __/ |  | | | |  __/ |\n", 0x03);
    terminal_print_colored("  |_|  |_|_|\\___|_|  \\___/|_|\\_\\___|_|  |_| |_|\\___|_|\n", 0x01);
    terminal_print_colored("                                            v0.2.0\n\n", 0x08);
}

/* Fun */
static void cmd_snake(int ac, char** av)     { (void)ac; (void)av; game_snake(); }
static void cmd_2048(int ac, char** av)      { (void)ac; (void)av; game_2048(); }
static void cmd_matrix(int ac, char** av)    { (void)ac; (void)av; screensaver_matrix(); }
static void cmd_starfield(int ac, char** av) { (void)ac; (void)av; screensaver_starfield(); }
static void cmd_pipes(int ac, char** av)     { (void)ac; (void)av; screensaver_pipes(); }

/* OpenGL 3D demo */
static void cmd_gl(int ac, char** av) {
    (void)ac; (void)av;
    gl_demo_start();
}

/* Shell scripting - execute commands from a file */
static void cmd_gui(int ac, char** av) {
    (void)ac; (void)av;
    gui_start();
    /* After returning from GUI, redraw terminal */
    terminal_clear();
    kprintf("Returned to console.\n");
}

static void cmd_sh(int argc, char** argv) {
    if(argc<2){kprintf("Usage: sh <script>\n");return;}
    char buf[RAMFS_MAX_DATA]; int32_t n=ramfs_read(argv[1],buf,sizeof(buf)-1);
    if(n<0){kprintf("sh: %s: not found\n",argv[1]);return;}
    buf[n]='\0';

    /* Execute each line */
    char* line=buf;
    while(*line){
        char cmd[CMD_MAX];
        int ci=0;
        while(*line && *line!='\n' && ci<CMD_MAX-1) cmd[ci++]=*line++;
        cmd[ci]='\0';
        if(*line=='\n') line++;

        /* Skip empty lines and comments */
        char* p=cmd; while(*p==' '||*p=='\t')p++;
        if(!*p || *p=='#') continue;

        /* Expand environment variables */
        char expanded[CMD_MAX];
        env_expand(p, expanded, CMD_MAX);

        terminal_print_colored("+ ", 0x08);
        kprintf("%s\n", expanded);
        execute_command(expanded);
    }
}

/* ====== PAGER (more) ====== */
static void pager_display(const char* data, int len) {
    int lines = 0;
    int page_lines = VGA_HEIGHT - 2; /* leave room for --more-- prompt */

    for (int i = 0; i < len; i++) {
        terminal_putchar(data[i]);
        if (data[i] == '\n') {
            lines++;
            if (lines >= page_lines) {
                terminal_print_colored("-- more (q=quit, space=next page, enter=next line) --", 0x70);
                while (1) {
                    char c = keyboard_getchar();
                    if (c == 'q' || c == 3) {  /* q or Ctrl+C */
                        /* Erase the prompt */
                        terminal_putchar('\r');
                        for (int j = 0; j < 55; j++) terminal_putchar(' ');
                        terminal_putchar('\r');
                        terminal_putchar('\n');
                        return;
                    } else if (c == '\n') {
                        /* One more line */
                        /* Erase the prompt */
                        terminal_putchar('\r');
                        for (int j = 0; j < 55; j++) terminal_putchar(' ');
                        terminal_putchar('\r');
                        lines = page_lines - 1;
                        break;
                    } else {
                        /* Space or anything else = next page */
                        terminal_putchar('\r');
                        for (int j = 0; j < 55; j++) terminal_putchar(' ');
                        terminal_putchar('\r');
                        lines = 0;
                        break;
                    }
                }
            }
        }
    }
}

static void cmd_more(int argc, char** argv) {
    if (argc < 2) {
        /* No argument — read from /tmp/.pipe if piped, otherwise show usage */
        char buf[OUTPUT_BUF_SIZE];
        int32_t n = ramfs_read("/tmp/.pipe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            pager_display(buf, n);
        } else {
            kprintf("  Usage: more <file>  or  command | more\n");
        }
        return;
    }
    /* Display file through pager */
    char buf[OUTPUT_BUF_SIZE];
    int32_t n = ramfs_read(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        kprintf("  more: %s: not found\n", argv[1]);
        return;
    }
    buf[n] = '\0';
    pager_display(buf, n);
}

/* ====== COMMAND TABLE ====== */
typedef struct { const char* name; void(*func)(int,char**); } command_t;

static const command_t commands[] = {
    {"help",cmd_help},{"?",cmd_help},{"clear",cmd_clear},{"cls",cmd_clear},
    {"echo",cmd_echo},{"uname",cmd_uname},{"uptime",cmd_uptime},{"date",cmd_date},{"time",cmd_date},
    {"sysinfo",cmd_sysinfo},{"cpuid",cmd_cpuid},{"lspci",cmd_lspci},{"paging",cmd_paging},
    {"mem",cmd_mem},{"free",cmd_mem},{"heap",cmd_heap},{"alloc",cmd_alloc},
    {"ps",cmd_ps},{"kill",cmd_kill},{"ipc",cmd_ipc},{"services",cmd_services},{"mkport",cmd_mkport},{"send",cmd_send},{"recv",cmd_recv},
    {"syscall",cmd_syscall_test},
    {"ls",cmd_ls},{"dir",cmd_ls},{"cd",cmd_cd},{"pwd",cmd_pwd},{"cat",cmd_cat},
    {"touch",cmd_touch},{"mkdir",cmd_mkdir},{"write",cmd_write},{"rm",cmd_rm},{"del",cmd_rm},
    {"tree",cmd_tree},{"hex",cmd_hex},{"xxd",cmd_hex},{"wc",cmd_wc},{"more",cmd_more},{"less",cmd_more},
    {"cp",cmd_cp},{"mv",cmd_mv},{"head",cmd_head},{"tail",cmd_tail},{"grep",cmd_grep},{"find",cmd_find},
    {"ifconfig",cmd_ifconfig},{"ping",cmd_ping},{"arp",cmd_arp},
    {"nslookup",cmd_nslookup},{"dig",cmd_nslookup},{"dns",cmd_dns},
    {"scheduler",cmd_scheduler},{"sched",cmd_scheduler},
    {"disk",cmd_disk},{"hdd",cmd_disk},{"format",cmd_format},
    {"mount",cmd_mount},{"umount",cmd_umount},{"unmount",cmd_umount},
    {"ntfsinfo",cmd_ntfsinfo},
    {"save",cmd_save},{"load",cmd_load},{"exec",cmd_exec},
    {"whoami",cmd_whoami},{"users",cmd_users},{"adduser",cmd_adduser},{"passwd",cmd_passwd},
    {"login",cmd_login},{"logout",cmd_logout},
    {"env",cmd_env},{"export",cmd_export},{"set",cmd_export},{"unset",cmd_unset},
    {"edit",cmd_edit},{"nano",cmd_edit},{"vi",cmd_edit},
    {"beep",cmd_beep},{"color",cmd_color},{"calc",cmd_calc},{"history",cmd_history},{"logo",cmd_logo},
    {"snake",cmd_snake},{"2048",cmd_2048},
    {"matrix",cmd_matrix},{"starfield",cmd_starfield},{"pipes",cmd_pipes},
    {"gui",cmd_gui},{"startx",cmd_gui},{"desktop",cmd_gui},
    {"gl",cmd_gl},{"opengl",cmd_gl},{"3d",cmd_gl},{"gldemo",cmd_gl},
    {"sh",cmd_sh},{"run",cmd_sh},
    {NULL,NULL}
};

/* Export command names for tab completion */
const char* cmd_names[96];
static void init_cmd_names(void) {
    int j=0;
    for(int i=0;commands[i].name&&j<95;i++) cmd_names[j++]=commands[i].name;
    cmd_names[j]=NULL;
}

/* Execute single command (no pipes/redirection) */
static void exec_single(char* cmdline) {
    char* argv[MAX_ARGS];
    int argc = tokenize(cmdline, argv);
    if (!argc) return;

    if(strcmp(argv[0],"reboot")==0){power_reboot();return;}
    if(strcmp(argv[0],"shutdown")==0||strcmp(argv[0],"halt")==0){power_shutdown();return;}

    for(int i=0;commands[i].name;i++)
        if(strcmp(argv[0],commands[i].name)==0){if(commands[i].func)commands[i].func(argc,argv);return;}

    terminal_print_colored(argv[0],0x0C);
    kprintf(": command not found. Type 'help'.\n");
}

/* Execute with pipe and redirection support */
static void execute_command(char* cmdline) {
    /* Expand environment variables */
    char expanded[CMD_MAX];
    env_expand(cmdline, expanded, CMD_MAX);

    /* Check for output redirection */
    char* redir = NULL;
    bool append = false;
    char* gt = strchr(expanded, '>');
    if (gt) {
        if (gt > expanded && *(gt-1) != '\\') {
            if (*(gt+1) == '>') { append = true; *gt = '\0'; gt += 2; }
            else { *gt = '\0'; gt += 1; }
            while (*gt == ' ') gt++;
            redir = gt;
            /* Trim trailing spaces from redir filename */
            char* end = redir + strlen(redir) - 1;
            while (end > redir && *end == ' ') *end-- = '\0';
        }
    }

    /* Check for pipe */
    char* pipe = strchr(expanded, '|');
    if (pipe && !redir) {
        *pipe = '\0';
        pipe++;
        while (*pipe == ' ') pipe++;

        /* Capture output of first command */
        start_capture();
        exec_single(expanded);
        stop_capture();

        /* For pipe, we write captured output to a temp file, then use it as input */
        /* Simple approach: write to /tmp/.pipe, then modify second command */
        ramfs_write("/tmp/.pipe", output_buffer, output_pos);

        /* Execute second command - for now just run it (full pipe would need stdin redirection) */
        /* If it's 'grep' or 'wc', handle specially */
        char* argv2[MAX_ARGS];
        char pipe_cmd[CMD_MAX];
        strcpy(pipe_cmd, pipe);
        int argc2 = tokenize(pipe_cmd, argv2);

        if (argc2 > 0 && (strcmp(argv2[0], "more") == 0 || strcmp(argv2[0], "less") == 0)) {
            /* Page through captured output */
            pager_display(output_buffer, output_pos);
        } else if (argc2 > 0 && strcmp(argv2[0], "wc") == 0) {
            /* Count lines/words in captured output */
            int lines=0, words=0; bool in_word=false;
            for (int i = 0; i < output_pos; i++) {
                if (output_buffer[i]=='\n') lines++;
                if (isspace(output_buffer[i])) in_word=false;
                else { if(!in_word) words++; in_word=true; }
            }
            kprintf("  %d lines, %d words, %d bytes\n", lines, words, output_pos);
        } else if (argc2 > 1 && strcmp(argv2[0], "grep") == 0) {
            /* Simple grep */
            char* line = output_buffer;
            while (*line) {
                char* nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                /* Check if line contains pattern */
                char* found = NULL;
                for (char* p = line; *p; p++) {
                    if (strncmp(p, argv2[1], strlen(argv2[1])) == 0) { found = p; break; }
                }
                if (found) kprintf("%s\n", line);
                if (nl) { *nl = '\n'; line = nl + 1; }
                else break;
            }
        } else {
            /* Just print the output and run second command normally */
            exec_single(pipe);
        }
        return;
    }

    /* Handle redirection */
    if (redir) {
        start_capture();
        exec_single(expanded);
        stop_capture();

        if (append)
            ramfs_append(redir, output_buffer, output_pos);
        else
            ramfs_write(redir, output_buffer, output_pos);
        return;
    }

    exec_single(expanded);
}

static void print_prompt(void) {
    terminal_print_colored(login_current_user(), 0x0A);
    terminal_print_colored("@", 0x08);
    terminal_print_colored("microkernel", 0x0A);
    terminal_print_colored(":", 0x08);
    terminal_print_colored(ramfs_get_cwd(), 0x09);
    terminal_print_colored("$ ", 0x07);
}

void shell_init(void) {
    memset(history, 0, sizeof(history));
    history_count = 0;
    init_cmd_names();
}

void shell_run(void) {
    char cmd[CMD_MAX];
    while (1) {
        print_prompt();
        shell_readline(cmd, CMD_MAX);
        if (cmd[0]) {
            add_history(cmd);
            serial_printf("[sh] %s\n", cmd);
            execute_command(cmd);
        }
    }
}

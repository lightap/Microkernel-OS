# MicroKernel v0.3.0 — Feature-Rich x86 Microkernel

A ~100KB microkernel for x86 (i386) with 55+ shell commands, written in C and x86 assembly.

## Quick Start

```bash
# Build (needs: gcc-i686-linux-gnu, nasm)
make

# Run in QEMU
qemu-system-i386 -kernel microkernel.bin -m 128M

# With networking (required for ping, nslookup, etc.)
qemu-system-i386 -kernel microkernel.bin -m 128M \
    -netdev user,id=n0 -device rtl8139,netdev=n0 \
    -serial stdio
```

## Features

### Core Kernel
- **GDT** — kernel + user code/data segments
- **IDT** — 32 CPU exceptions + 16 hardware IRQs + syscall gate
- **PIC** — 8259 remapping (IRQs 32-47)
- **Paging** — identity-mapped virtual memory with page fault handler
- **Physical Memory Manager** — bitmap page allocator
- **Kernel Heap** — kmalloc/kfree with block splitting/merging
- **Preemptive Multitasking** — timer-driven context switching with configurable time quantum, priority scheduling, per-task quantum, lock/unlock for critical sections
- **Cooperative Fallback** — task yield/sleep still available

### /proc Virtual Filesystem
- **/proc/cpuinfo** — CPU vendor, model, feature flags (like Linux)
- **/proc/meminfo** — physical memory, heap usage, page counts
- **/proc/uptime** — system uptime in seconds
- **/proc/version** — kernel version string
- **/proc/stat** — CPU ticks, context switches, process count
- **/proc/loadavg** — system load average
- **/proc/filesystems** — registered filesystem types
- **/proc/mounts** — mounted filesystems
- **/proc/processes** — list of all tasks with state
- **/proc/interrupts** — IRQ statistics
- **/proc/scheduler** — scheduler mode, quantum, switch count
- **/proc/net/dev** — network device statistics
- **/proc/<pid>/status** — per-process details (name, state, priority, ticks)

### Networking
- **RTL8139 driver** — auto-detected via PCI
- **Ethernet + ARP** — frame send/receive, ARP cache
- **IP + ICMP** — ping (send and respond)
- **UDP** — full send/receive with user-registerable handler
- **DNS resolver** — query A records, configurable DNS server
- Commands: `ifconfig`, `ping <ip>`, `arp`, `nslookup <host>`, `dns [server]`

### Hardware Detection
- **CPUID** — CPU brand, family/model, feature flags
- **PCI Bus** — full enumeration with class/vendor identification
- **RTL8139 NIC** — real network driver for QEMU

### Drivers
- **PIT Timer** — 100Hz with preemptive scheduling
- **PS/2 Keyboard** — full US QWERTY with shift, caps lock, ctrl combos
- **VGA Text Mode** — 80x25 with scrolling, colors, box drawing
- **Serial Port** — COM1 debug logging at 38400 baud
- **RTC Clock** — real date/time from CMOS
- **PC Speaker** — beep with custom frequency

### Interactive Shell (55+ commands)
- Tab completion, command history (up/down arrows)
- Pipes (`ps | grep kernel`), redirection (`echo hello > file`)
- Environment variables, shell scripting

### Commands
| Category | Commands |
|----------|----------|
| System | `help` `clear` `uname` `uptime` `date` `sysinfo` `reboot` `shutdown` |
| Hardware | `cpuid` `lspci` `paging` |
| Memory | `mem` `heap` `alloc` |
| Processes | `ps` `kill` `scheduler` |
| IPC | `ipc` `mkport` `send` `recv` |
| Syscalls | `syscall` (test INT 0x80) |
| Filesystem | `ls` `cd` `pwd` `cat` `touch` `mkdir` `write` `rm` `tree` `hex` `wc` `cp` `mv` `head` `tail` `grep` `find` `more` |
| Network | `ifconfig` `ping` `arp` `nslookup` `dns` |
| Users | `whoami` `login` `logout` `users` `adduser` `passwd` |
| Env Vars | `env` `export` `set` `unset` |
| Editor | `edit` / `nano` / `vi` |
| Tools | `echo` `beep` `color` `calc` `history` `logo` |
| Scripts | `sh` / `run` |
| Games | `snake` `2048` |
| Screensavers | `matrix` `starfield` `pipes` |

### New in v0.3.0
- **Preemptive multitasking** — `scheduler preemptive/cooperative`, `scheduler quantum <n>`
- **/proc filesystem** — `cat /proc/cpuinfo`, `cat /proc/meminfo`, `ls /proc`
- **UDP networking** — full UDP send/receive stack
- **DNS resolution** — `nslookup google.com` resolves hostnames to IP addresses

## License
copyright andrew pliatsikas 2026
BSD LICENSE

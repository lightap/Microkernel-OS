# Makefile for x86 microkernel

CC      = i686-linux-gnu-gcc
AS      = nasm
LD      = i686-linux-gnu-gcc

CFLAGS  = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Iinclude -nostdlib -fno-builtin -fno-pie
LDFLAGS = -T linker.ld -ffreestanding -O2 -nostdlib -no-pie -Wl,--build-id=none -lgcc
ASFLAGS = -f elf32

SRC_DIR = src
OBJ_DIR = build

# Source files
C_SRCS  = $(wildcard $(SRC_DIR)/*.c)
ASM_SRCS = $(wildcard $(SRC_DIR)/*.asm)

# Object files
C_OBJS  = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(C_SRCS))
ASM_OBJS = $(patsubst $(SRC_DIR)/%.asm, $(OBJ_DIR)/%.o, $(ASM_SRCS))

# crt0.o and userlib.o are user-space runtime objects (for ELF binaries),
# not part of the kernel itself — exclude them from the kernel link.
# Also exclude any user programs (hello.o, etc.)
USER_OBJS = $(OBJ_DIR)/crt0.o $(OBJ_DIR)/userlib.o  $(OBJ_DIR)/hello.o $(OBJ_DIR)/amazing.o $(OBJ_DIR)/texcube.o
KERNEL_OBJS = $(filter-out $(USER_OBJS), $(ASM_OBJS) $(C_OBJS))

# Output
KERNEL  = microkernel.bin
ISO     = microkernel.iso




.PHONY: all clean iso run

all: $(KERNEL)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.asm | $(OBJ_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL): $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

# Create bootable ISO using GRUB
iso: $(KERNEL)
	mkdir -p isodir/boot/grub
	cp $(KERNEL) isodir/boot/microkernel.bin
	echo 'menuentry "microkernel" {' > isodir/boot/grub/grub.cfg
	echo '    multiboot /boot/microkernel.bin' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) isodir

# Run in QEMU (direct kernel boot, no ISO needed)
run: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL) -m 2G

# Run with mouse support (for GUI desktop)
run-gui: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL) -m 2G -device VGA

# Run with virtio-gpu (for GUI desktop with virtio backend)
run-virtio: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL) -m 2G -device virtio-gpu-pci

# Run with virtio-gpu + disk + network
run-virtio-full: $(KERNEL) $(DISK_IMG)
	qemu-system-i386 -kernel $(KERNEL) -m 2G -device virtio-gpu-pci \
		-hda $(DISK_IMG) -netdev user,id=n0 -device rtl8139,netdev=n0

# Run with virtio-gpu-gl (3D acceleration) + virtio-tablet for mouse input
# Use this when you need GL acceleration — PS/2 mouse doesn't work with gtk,gl=on
run-virtio-gl: $(KERNEL) $(DISK_IMG)
	qemu-system-i386 -kernel $(KERNEL) -m 2G \
		-device virtio-gpu-gl-pci -display gtk,gl=on \
		-device virtio-tablet-pci \
		-hda $(DISK_IMG) -netdev user,id=n0 -device rtl8139,netdev=n0 \
		-serial stdio

# Load files into the OS at boot (appears in /home/user/)
# Usage:
#   make run-img FILE=photo.png              (single file)
#   make run-img FILE="a.png,b.bmp,c.tga"   (multiple, comma-separated)
# Max 3MB per file. Then: gui -> Files -> /home/user/ -> click image
run-img: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL) -m 2G -serial stdio -initrd "$(FILE)"

# Run from ISO
run-iso: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 2G

# Create a blank disk image (64MB default)
DISK_SIZE ?= 64
DISK_IMG ?= disk.img
disk:
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(DISK_SIZE) 2>/dev/null
	@echo "Created $(DISK_IMG) ($(DISK_SIZE) MB)"
	@echo "Use 'format -y' inside the OS to create a FAT16 filesystem"

# Run with hard disk attached
# Usage: make run-disk
#        make run-disk DISK_IMG=mydata.img
run-disk: $(KERNEL) $(DISK_IMG)
	qemu-system-i386 -kernel $(KERNEL) -m 2G -hda $(DISK_IMG)

# Run with disk + network
run-full: $(KERNEL) $(DISK_IMG)
	qemu-system-i386 -kernel $(KERNEL) -m 2G -hda $(DISK_IMG) \
		-netdev user,id=n0 -device rtl8139,netdev=n0

# Run with disk + files from host
# Usage: make run-disk-img DISK_IMG=disk.img FILE=photo.png
run-disk-img: $(KERNEL) $(DISK_IMG)
	qemu-system-i386 -kernel $(KERNEL) -m 2G -hda $(DISK_IMG) -initrd "$(FILE)"

# Create disk if it doesn't exist
$(DISK_IMG):
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(DISK_SIZE) 2>/dev/null

clean:
	rm -rf $(OBJ_DIR) $(KERNEL) $(ISO) isodir

# ============================================================
# User-space ELF binaries (run inside the OS with exec command)
# ============================================================
USER_LDFLAGS = -T user.ld -nostdlib -no-pie -Wl,--build-id=none -lgcc

# Build hello.elf test program
hello: $(OBJ_DIR)/crt0.o $(OBJ_DIR)/userlib.o $(OBJ_DIR)/hello.o
	$(LD) $(USER_LDFLAGS) -o $(OBJ_DIR)/hello.elf $^
	@echo "Built $(OBJ_DIR)/hello.elf"

# Boot kernel with hello.elf loaded via initrd
run-hello: $(KERNEL) hello
	qemu-system-i386 -kernel $(KERNEL) -m 2G -serial stdio -initrd "$(OBJ_DIR)/hello.elf"

# Boot with hello.elf + GPU 3D acceleration (virgl)
run-hello-gl: $(KERNEL) hello disk
	qemu-system-i386 -kernel $(KERNEL) -m 2G \
		-device virtio-gpu-gl-pci -display gtk,gl=on \
		-device virtio-tablet-pci \
		-hda $(DISK_IMG) -serial stdio \
		-initrd "$(OBJ_DIR)/hello.elf"

# Build amazing.elf program
amazing: $(OBJ_DIR)/crt0.o $(OBJ_DIR)/userlib.o $(OBJ_DIR)/amazing.o
	$(LD) $(USER_LDFLAGS) -o $(OBJ_DIR)/amazing.elf $^
	@echo "Built $(OBJ_DIR)/amazing.elf"

# Boot kernel with amazing.elf loaded via initrd
run-amazing: $(KERNEL) amazing
	qemu-system-i386 -kernel $(KERNEL) -m 2G -serial stdio -initrd "$(OBJ_DIR)/amazing.elf"

# Boot with both hello and amazing
run-both: $(KERNEL) hello amazing
	qemu-system-i386 -kernel $(KERNEL) -m 2G -serial stdio -initrd "$(OBJ_DIR)/hello.elf,$(OBJ_DIR)/amazing.elf"

# Build texcube.elf program
texcube: $(OBJ_DIR)/crt0.o $(OBJ_DIR)/userlib.o $(OBJ_DIR)/texcube.o
	$(LD) $(USER_LDFLAGS) -o $(OBJ_DIR)/texcube.elf $^
	@echo "Built $(OBJ_DIR)/texcube.elf"

# Boot kernel with texcube.elf loaded via initrd
run-texcube: $(KERNEL) texcube
	qemu-system-i386 -kernel $(KERNEL) -m 2G -serial stdio -initrd "$(OBJ_DIR)/texcube.elf"

# Build all user programs
user-programs: hello amazing texcube
	@echo "All user programs built!"

# Update the USER_OBJS line to exclude texcube.o from kernel:
# USER_OBJS = $(OBJ_DIR)/crt0.o $(OBJ_DIR)/userlib.o $(OBJ_DIR)/hello.o $(OBJ_DIR)/amazing.o $(OBJ_DIR)/texcube.o
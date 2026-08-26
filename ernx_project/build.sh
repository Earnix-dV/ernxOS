#!/bin/bash
set -e

cd "$(dirname "$0")"

# Fail early with a useful message instead of producing a mysterious
# "command not found" halfway through the build.
for tool in as gcc ld python3 grub-mkrescue; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required tool '$tool' is not installed."
        echo "On Linux Mint/Ubuntu/Debian: sudo apt install build-essential grub-pc-bin grub-common xorriso python3"
        exit 1
    fi
done

mkdir -p build

echo "==> Assembling boot.s..."
as --32 src/boot.s -o build/boot.o

echo "==> Assembling idt_asm.s..."
as --32 src/idt_asm.s -o build/idt_asm.o

echo "==> Assembling task_asm.s..."
as --32 src/task_asm.s -o build/task_asm.o

# The kernel is split into one .c/.h pair per subsystem (see src/) instead
# of one giant kernel.c - each module below compiles to its own object,
# and they're linked together at the end. kernel.c itself is now just the
# ~80-line kernel_main() that wires the modules together at boot.
MODULES="vga util interrupts paging wm vfs disk fs keyboard mouse hw gfx ernxscript shell kernel"

CFLAGS="-m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra"
OBJS="build/boot.o build/idt_asm.o build/task_asm.o"

for mod in $MODULES; do
    echo "==> Compiling ${mod}.c..."
    gcc $CFLAGS -c "src/${mod}.c" -o "build/${mod}.o"
    OBJS="$OBJS build/${mod}.o"
done

echo "==> Linking kernel..."
ld -m elf_i386 -T src/linker.ld -o myos.bin $OBJS -nostdlib

echo "==> Packing files/ into initrd.img..."
python3 tools/pack_initrd.py initrd.img files/*

echo "==> Building bootable ISO..."
mkdir -p isodir/boot/grub
cp myos.bin isodir/boot/myos.bin
cp initrd.img isodir/boot/initrd.img
cp src/grub.cfg isodir/boot/grub/grub.cfg
grub-mkrescue -o myos.iso isodir

echo ""
echo "Build complete: myos.iso"
echo "Run ./run.sh to boot it in VirtualBox."

#!/bin/sh
#
# Boot the ESP image under QEMU with UEFI firmware.
#
# This is the development loop for the Steam Deck: OVMF is the same class of
# firmware, GRUB is the same loader, and the kernel is entered through the same
# multiboot2 path. What differs is the hardware behind it, so anything that works
# here still has to be tried on the Deck -- but anything that fails here would
# have failed there too, and finding out takes seconds instead of a reboot.
#
# Unlike the CAmkES `simulate` script this does not run headless. The whole point
# of the exercise is the framebuffer, so QEMU opens a window and the serial line
# goes to the terminal alongside it. On the Deck there is no serial line at all,
# which is why the framebuffer has to carry the console in the end.
#
# Requires, on the host: qemu-system-x86, ovmf.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname -- "$here")

esp=${ESP:-$repo/build/esp.img}
mem=${MEM:-1G}
# Matches what the CAmkES simulate script asks for, so the kernel meets the CPU
# it was configured against.
cpu=${CPU:-Nehalem,-vme,+pdpe1gb,-xsave,-xsaveopt,-xsavec,-fsgsbase,-invpcid,+syscall,+lm,enforce}

if [ ! -f "$esp" ]; then
    echo "missing: $esp" >&2
    echo "run $here/make-esp.sh first" >&2
    exit 1
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "missing tool: qemu-system-x86_64" >&2
    echo "apt install qemu-system-x86 ovmf" >&2
    exit 1
fi

# Distributions disagree on the names, and the 4M build is the current one.
code=
for c in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
         /usr/share/ovmf/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd; do
    [ -f "$c" ] && { code=$c; break; }
done
vars_template=
for v in /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd \
         /usr/share/ovmf/OVMF_VARS.fd /usr/share/edk2/ovmf/OVMF_VARS.fd; do
    [ -f "$v" ] && { vars_template=$v; break; }
done

if [ -z "$code" ] || [ -z "$vars_template" ]; then
    echo "OVMF firmware not found" >&2
    echo "apt install ovmf" >&2
    exit 1
fi

# The firmware writes its variables, so it needs a copy it may modify. Keeping it
# next to the image means boot order and mode choices survive a restart.
vars=$repo/build/OVMF_VARS.fd
if [ ! -f "$vars" ]; then
    mkdir -p "$(dirname -- "$vars")"
    cp "$vars_template" "$vars"
fi

# The image is attached as USB storage behind an xHCI controller rather than as a
# plain disk, because that is how the Deck will boot it.
#
# A USB keyboard on the xHCI controller, which is the arrangement the Steam Deck
# has and the one the driver is being written against.
#
# QEMU sends key events to one keyboard device, and this takes them away from the
# i8042 that q35 has by default. Remove it to exercise the legacy keyboard path
# instead -- that one is dead on the Deck, whose firmware emulates nothing, but
# it is what works here today.
gdb=
if [ "${1:-}" = "-d" ] || [ "${1:-}" = "--gdb" ]; then
    gdb="-s -S"
    echo "waiting for GDB on port 1234..."
fi

set -x
exec qemu-system-x86_64 \
    -machine q35 \
    -cpu "$cpu" \
    -m "size=$mem" \
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$code" \
    -drive "if=pflash,format=raw,unit=1,file=$vars" \
    -drive "format=raw,file=$esp,if=none,id=esp" \
    -device qemu-xhci,id=xhci \
    -device usb-storage,bus=xhci.0,drive=esp \
    -device usb-kbd,bus=xhci.0 \
    -vga std \
    -serial mon:stdio \
    $gdb

#!/bin/sh
#
# Build a bootable disk image holding GRUB, seL4 and its capDL loader.
#
# The same image serves QEMU and real hardware: written to a microSD or a USB
# stick it is what the Steam Deck boots, and handed to QEMU as a disk it is what
# OVMF boots. Keeping one artefact for both is the point -- a development loop
# that does not share the target's boot path proves very little about the target.
#
# Two partitions, for a reason that has nothing to do with booting.
#
#   1. A plain FAT32 partition, holding the kernel, the capDL image and the GRUB
#      configuration.
#   2. An EFI system partition, holding only BOOTX64.EFI.
#
# All of it would fit on one, and a single EFI system partition is the obvious
# layout. But Windows deliberately gives no drive letter to a partition marked as
# one, so a card built that way can only be updated by writing the whole image
# again -- which needs a tool that can lock a raw disk, and is slow besides.
# Splitting it puts the files a rebuild actually changes on a filesystem Windows
# mounts like any other, and the EFI system partition then holds one file that
# changes only when GRUB's module list does.
#
# The data partition comes first because Windows treats removable media as
# having one partition that matters, the first. With the EFI system partition
# there, the one Windows would mount is the one it refuses to mount, and neither
# is reachable. Firmware has no such preference: it finds the EFI system
# partition by its type in the GPT, wherever it sits.
#
# GRUB likewise finds its configuration by searching for a file rather than by
# position, so the arrangement is not written down anywhere that could disagree
# with it.
#
# Requires, on the host: grub-efi-amd64-bin, grub-common, mtools, dosfstools,
# gdisk. mtools and sgdisk are what keep this free of root: neither the
# filesystems nor the partition table need the image to be mounted or attached to
# a loop device.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname -- "$here")

build_dir=${BUILD_DIR:-$repo/vendor/camkes-project/build}
out=${OUT:-$repo/build/esp.img}
# The same contents as loose files. Writing the image needs a tool that addresses
# the whole disk rather than a volume, and not every machine has one to hand; a
# card partitioned by other means can be filled by copying these instead. They
# are also what an update copies, once the card exists.
files=${FILES:-$repo/build/esp-files}

# FAT32 needs upwards of 33 MiB before it has enough clusters to be valid, so
# neither partition can be as small as its contents would allow.
esp_mb=${ESP_MB:-64}
data_mb=${DATA_MB:-192}

kernel=$build_dir/images/kernel-x86_64-pc99
capdl=$build_dir/images/capdl-loader-image-x86_64-pc99

for f in "$kernel" "$capdl"; do
    if [ ! -f "$f" ]; then
        echo "missing: $f" >&2
        echo "build the CAmkES system first, or set BUILD_DIR" >&2
        exit 1
    fi
done

for t in grub-mkstandalone mmd mcopy mkfs.vfat sgdisk; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "missing tool: $t" >&2
        echo "apt install grub-efi-amd64-bin grub-common mtools dosfstools gdisk" >&2
        exit 1
    fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The configuration baked into BOOTX64.EFI does nothing but find the real one.
# Keeping the menu on the data partition means a change to it needs no new EFI
# binary and no reflashing -- it is a text file on a drive Windows will open.
cat > "$work/embedded.cfg" <<'GRUB'
insmod part_gpt
insmod fat
insmod search_fs_file

search --no-floppy --file --set=root /sel4/grub.cfg

if [ -e ($root)/sel4/grub.cfg ]; then
    configfile ($root)/sel4/grub.cfg
else
    echo 'No /sel4/grub.cfg found on any partition.'
    echo 'The data partition is missing or unreadable.'
    sleep 30
fi
GRUB

# Only the modules the configurations actually reach for. grub-mkstandalone
# pulls in dependencies itself, so this is a list of entry points rather than a
# closure: part_gpt and fat to read the partitions, efi_gop for the framebuffer,
# multiboot2 for the loader itself.
modules='part_gpt part_msdos fat normal configfile echo ls test search search_fs_file
         multiboot2 efi_gop all_video video videoinfo gfxterm terminal boot reboot halt minicmd sleep'

echo "building BOOTX64.EFI"
grub-mkstandalone \
    --format=x86_64-efi \
    --modules="$modules" \
    --output="$work/BOOTX64.EFI" \
    "boot/grub/grub.cfg=$work/embedded.cfg"

MTOOLS_SKIP_CHECK=1
export MTOOLS_SKIP_CHECK

echo "building the EFI system partition (${esp_mb}M)"
mkfs.vfat -F 32 -n SEL4BOOT -C "$work/esp.img" $((esp_mb * 1024)) >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI

echo "building the data partition (${data_mb}M)"
mkfs.vfat -F 32 -n SEL4 -C "$work/data.img" $((data_mb * 1024)) >/dev/null
mmd -i "$work/data.img" ::/sel4
mcopy -i "$work/data.img" "$here/grub.cfg" ::/sel4/grub.cfg
mcopy -i "$work/data.img" "$kernel" ::/sel4/kernel
mcopy -i "$work/data.img" "$capdl" ::/sel4/capdl-loader

echo "contents of the data partition:"
mdir -i "$work/data.img" -/ :: || true

# The first partition starts at the usual 1 MiB, which keeps it aligned to any
# erase block a card is likely to have.
data_start=2048
data_sectors=$((data_mb * 1024 * 2))
esp_start=$((data_start + data_sectors))
esp_sectors=$((esp_mb * 1024 * 2))

echo "building $out"
mkdir -p "$(dirname -- "$out")"
rm -f "$out"
# Both partitions plus the two copies of the GPT and the gap before the first.
dd if=/dev/zero of="$out" bs=1M count=$((esp_mb + data_mb + 2)) status=none

# sgdisk writes a protective MBR of its own, which is what stops a machine that
# reads the image as MBR from seeing an unpartitioned disk.
sgdisk --clear \
       --new=1:${data_start}:$((data_start + data_sectors - 1)) \
       --typecode=1:0700 --change-name=1:"SEL4" \
       --new=2:${esp_start}:$((esp_start + esp_sectors - 1)) \
       --typecode=2:EF00 --change-name=2:"EFI System" \
       "$out" >/dev/null

dd if="$work/data.img" of="$out" bs=512 seek=$data_start conv=notrunc status=none
dd if="$work/esp.img" of="$out" bs=512 seek=$esp_start conv=notrunc status=none

echo "staging the same contents as files in $files"
rm -rf "$files"
mkdir -p "$files/EFI/BOOT" "$files/sel4"
cp "$work/BOOTX64.EFI" "$files/EFI/BOOT/BOOTX64.EFI"
cp "$here/grub.cfg" "$files/sel4/grub.cfg"
cp "$kernel" "$files/sel4/kernel"
cp "$capdl" "$files/sel4/capdl-loader"

echo
sgdisk --print "$out" | tail -5
echo
echo "wrote $out"
echo "  QEMU:      $here/run-ovmf.sh"
echo "  microSD:   write the whole image once, with Rufus in DD mode or Etcher."
echo
echo "  After that the SEL4 partition appears in Windows with a drive letter,"
echo "  and updating the card is copying $files/sel4/ over its sel4 folder."
echo
echo "wrote $files"
echo "  EFI/BOOT/BOOTX64.EFI   -> the EFI system partition"
echo "  sel4/                  -> the data partition"
echo "  Use these when the card is partitioned by other means, such as diskpart,"
echo "  or to refresh a card that already has the layout."

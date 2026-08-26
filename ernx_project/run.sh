#!/bin/bash
# run.sh - create/recover the MyOS VirtualBox VM, attach the ISO + persistent disk,
# and boot it.  Safe to run repeatedly after an interrupted/failed run.
set -euo pipefail

cd "$(dirname "$0")"
ISO_PATH="$(pwd)/myos.iso"
DISK_PATH="$(pwd)/myos_disk.vdi"
VM_NAME="MyOS"
VM_DIR="$HOME/VirtualBox VMs/$VM_NAME"
VM_FILE="$VM_DIR/$VM_NAME.vbox"

for tool in VBoxManage; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required tool '$tool' is not installed."
        exit 1
    fi
done

if [ ! -f "$ISO_PATH" ]; then
    echo "myos.iso not found — run ./build.sh first."
    exit 1
fi

# If a previous run left the VM in a saved/running state, make it fully off.
VBoxManage controlvm "$VM_NAME" poweroff 2>/dev/null || true
sleep 1

# Recover an existing .vbox file that was left behind after unregistervm.
# This avoids CreateMachine failing just because the VM was unregistered.
if ! VBoxManage list vms | grep -Fq '"MyOS"'; then
    if [ -f "$VM_FILE" ]; then
        echo "==> Re-registering existing VM '$VM_NAME'..."
        VBoxManage registervm "$VM_FILE"
    else
        echo "==> Creating VM '$VM_NAME'..."
        VBoxManage createvm --name "$VM_NAME" --ostype Other --register
    fi
fi

# Ensure the VM has the expected basic settings.  These are harmless if the
# values were already set.
VBoxManage modifyvm "$VM_NAME" --memory 32
VBoxManage storagectl "$VM_NAME" --name "IDE" --add ide 2>/dev/null || true

# Detach anything currently occupying the IDE slots before replacing it.
# This also releases the VDI from the VM before we repair its registry entry.
VBoxManage storageattach "$VM_NAME" --storagectl "IDE" --port 0 --device 0 --medium none 2>/dev/null || true
VBoxManage storageattach "$VM_NAME" --storagectl "IDE" --port 1 --device 0 --medium none 2>/dev/null || true

# VirtualBox can retain an old medium UUID in its registry after a failed
# createmedium operation. Do NOT query the disk by path with showmediuminfo:
# if the registry UUID and the UUID inside the VDI disagree, that command can
# fail before we can repair the registration. `list hdds` still exposes the
# registry record, so extract the UUID by matching its Location.
registered_uuid="$(VBoxManage list hdds 2>/dev/null | awk -v target="$DISK_PATH" '
    $1 == "UUID:" { uuid=$2 }
    $1 == "Location:" {
        location=$0
        sub(/^[[:space:]]*Location:[[:space:]]*/, "", location)
        if (location == target) { print uuid; exit }
    }
')"

if [ -n "$registered_uuid" ]; then
    echo "==> Found VirtualBox registry entry for myos_disk.vdi (UUID $registered_uuid)."

    # Always remove the registry entry before deciding whether the file must
    # be created. This is the important case after a failed `createmedium`:
    # the VDI file may not exist anymore, while VirtualBox still remembers
    # the path and refuses to create another medium there.
    # closemedium WITHOUT --delete removes only the registry entry and never
    # deletes the user's VDI file.
    echo "==> Removing old VirtualBox VDI registration (data preserved)..."
    if ! VBoxManage closemedium disk "$registered_uuid" >/dev/null 2>&1; then
        echo "ERROR: VirtualBox refused to unregister the VDI entry."
        echo "       Registry UUID: $registered_uuid"
        echo "       File: $DISK_PATH"
        echo "       The VDI file, if present, was NOT deleted."
        exit 1
    fi
fi

# Create the persistent disk only if it genuinely does not exist.  Never
# recreate it on normal boots, otherwise the user's saved files disappear.
if [ ! -f "$DISK_PATH" ]; then
    echo "==> Creating persistent disk myos_disk.vdi (8 MB)..."
    VBoxManage createmedium disk --filename "$DISK_PATH" --size 8 --format VDI
fi

# Attach fresh copies of the ISO and persistent VDI.
echo "==> Attaching myos.iso (primary master) and myos_disk.vdi (secondary master)..."
VBoxManage storageattach "$VM_NAME" --storagectl "IDE" --port 0 --device 0 --type dvddrive --medium "$ISO_PATH"
VBoxManage storageattach "$VM_NAME" --storagectl "IDE" --port 1 --device 0 --type hdd --medium "$DISK_PATH"
VBoxManage modifyvm "$VM_NAME" --boot1 dvd --boot2 disk

# Start the VirtualBox window in fullscreen. The guest renders a 320x200
# VGA desktop; VirtualBox scales it to the host display.
VBoxManage setextradata "$VM_NAME" "GUI/Fullscreen" "true"

echo "==> Starting VM in fullscreen..."
VBoxManage startvm "$VM_NAME" --type gui

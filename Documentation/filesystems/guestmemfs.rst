.. SPDX-License-Identifier: GPL-2.0

======================================================
Guestmemfs - Persistent in-memory guest RAM filesystem
======================================================

Overview
========

Guestmemfs is an in-memory filesystem designed specifically for the purpose of
live update of virtual machines by being a persistent across kexec source of
guest VM memory.

Live update of a hypervisor refers to act of pausing running VMs, serialising
state, kexec-ing into a new hypervisor image, re-hydraing the KVM guests and
resuming them. To achieve this guest memory must be preserved across kexec.

Additionally, guestmemfs provides:
- secret hiding for guest memory: the physical memory allocated for guestmemfs
  is carved out of the direct map early in boot.
- struct page overhead elimination: guestmemfs memory is not allocated by the
  buddy allocator and does not have associated struct pages.
- huge page mappings: allocations are done at PMD size and this improves TLB
  performance (work in progress.)

Compilation
===========

Guestmemfs is enabled via CONFIG_GUESTMEMFS_FS

Persistence across kexec is enabled via CONFIG_KEXEC_KHO

Usage
=====

On first boot (cold boot), allocate a large contiguous chunk of memory for
guestmemfs via a kernel cmdline argument, eg:
`guestmemfs=10G`.

Mount guestmemfs:
mount -t guestmemfs guestmemfs /mnt/guestmemfs/

Create and truncate a file which will be used for guest RAM:

touch /mnt/guesttmemfs/guest-ram
truncate -s 500M /mnt/guestmemfs/guest-ram

Boot a VM with this as the RAM source and the live update option enabled:

qemu-system-x86_64 ... \
  -object memory-backend-file,id=pc.ram,size=100M,mem-path=/mnt/guestmemfs/guest-ram,share=yes,prealloc=off \
  -migrate-mode-enable cpr-reboot \
  ...

Suspect the guest and save the state via QEMU monitor:

migrate_set_parameter mode cpr-reboot
migrate file:/qemu.sav

Activate KHO to serialise guestmemfs metadata and then kexec to the new
hypervisor image:

echo 1 > /sys/kernel/kho/active
kexec -s -l --reuse-cmdline
kexec -e

After the kexec completes remount guestmemfs (or have it added to fstab)
Re-start QEMU in live update restore mode:

qemu-system-x86_64 ... \
  -object memory-backend-file,id=pc.ram,size=100M,mem-path=/mnt/guestmemfs/guest-ram,share=yes,prealloc=off \
  -migrate-mode-enable cpr-reboot \
  -incoming defer
  ...

Finally restore the VM state and resume it via QEMU console:

migrate_incoming file:/qemu.sav

Future Work
===========
- NUMA awareness and multi-mount point support
- Actually creating PMD-level mappings in page tables
- guest_memfd style interface for confidential computing
- supporting PUD-level allocations and mappings
- MCE handling
- Persisting IOMMU pgtables to allow DMA to guestmemfs during kexec

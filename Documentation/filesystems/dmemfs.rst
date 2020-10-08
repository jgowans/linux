.. SPDX-License-Identifier: GPL-2.0

=====================================
The Direct Memory Filesystem - DMEMFS
=====================================


.. Table of contents

   - Overview
   - Compilation
   - Usage

Overview
========

Dmemfs (Direct Memory filesystem) is device memory or reserved
memory based filesystem. This kind of memory is special as it
is not managed by kernel and it is without 'struct page'. Therefore
it can save extra memory from the host system for various usage,
especially for guest virtual machines.

It uses a kernel boot parameter ``dmem=`` to reserve the system
memory when the host system boots up, the details can be checked
in /Documentation/admin-guide/kernel-parameters.txt.

Compilation
===========

The filesystem should be enabled by turning on the kernel configuration
options::

        CONFIG_DMEM_FS          - Direct Memory filesystem support
        CONFIG_DMEM             - Allow reservation of memory for dmem


Additionally, the following can be turned on to aid debugging::

        CONFIG_DMEM_DEBUG_FS    - Enable debug information for dmem

Usage
========

Dmemfs supports mapping ``4K``, ``2M`` and ``1G`` size of pages to
the userspace, for example ::

    # mount -t dmemfs none -o pagesize=4K /mnt/

The it can create the backing storage with 4G size ::

    # truncate /mnt/dmemfs-uuid --size 4G

To use as backing storage for virtual machine starts with qemu, just need
to specify the memory-backed-file in the qemu command line like this ::

    # -object memory-backend-file,id=ram-node0,mem-path=/mnt/dmemfs-uuid \
        share=yes,size=4G,host-nodes=0,policy=preferred -numa node,nodeid=0,memdev=ram-node0

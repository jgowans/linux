/* SPDX-License-Identifier: MIT */

#ifndef _LINUX_PKERNFS_H
#define _LINUX_PKERNFS_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/kvm_host.h>

bool is_pkernfs_file(struct file *filep);

int pkernfs_gmem_bind(struct kvm *kvm, struct kvm_memory_slot *slot,
		      struct file *file, loff_t offset);

#endif /* _LINUX_PKERNFS_H */

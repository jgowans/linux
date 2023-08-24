/* SPDX-License-Identifier: MIT */

#ifndef _LINUX_GUESTMEMFS_H
#define _LINUX_GUESTMEMFS_H

/*
 * Carves out chunks of memory from memblocks for guestmemfs.
 * Must be called in early boot before memblocks are freed.
 */
# ifdef CONFIG_GUESTMEMFS_FS
void guestmemfs_reserve_mem(void);
#else
void guestmemfs_reserve_mem(void) { }
#endif

#endif

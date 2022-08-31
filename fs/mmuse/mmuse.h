/* SPDX-License-Identifier: MIT */

#include <linux/dcache.h>

/* Creates an "admin" file in the supplied root dir. */
int mmuse_create_admin_file(struct dentry *root);

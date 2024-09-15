// SPDX-License-Identifier: GPL-2.0-only

#include <linux/anon_inodes.h>
#include <linux/fdtable.h>
#include <linux/kexec.h>
#include <linux/libfdt.h>
#include "iommufd_private.h"
#include "io_pagetable.h"

/**
 * Serialised format:
 * /iommufd
 *   compatible = "iommufd-v0",
 *   iommufds = [
 *     persistent_id = {
 *       account_mode = u8
 *       ioases = [
 *         {
 *           areas = [
 *           ]
 *         }
 *       ]
 *     }
 *   ]
 */

static struct kobject *persisted_dir_kobj;

static int serialise_iommufd(void *fdt, struct iommufd_ctx *ictx)
{
	int err = 0;
	char name[24];
	struct iommufd_object *obj;
	unsigned long obj_idx;

	snprintf(name, sizeof(name), "%lu", ictx->persistent_id);
	err |= fdt_begin_node(fdt, name);
	err |= fdt_begin_node(fdt, "ioases");
	xa_for_each(&ictx->objects, obj_idx, obj) {
		struct iommufd_ioas *ioas;
		struct iopt_area *area;
		int area_idx = 0;

		if (obj->type != IOMMUFD_OBJ_IOAS)
			continue;

		ioas = (struct iommufd_ioas *) obj;
		snprintf(name, sizeof(name), "%lu", obj_idx);
		err |= fdt_begin_node(fdt, name);

		for (area = iopt_area_iter_first(&ioas->iopt, 0, ULONG_MAX); area;
				area = iopt_area_iter_next(area, 0, ULONG_MAX)) {
			unsigned long iova_start, iova_len;

			snprintf(name, sizeof(name), "%i", area_idx);
			err |= fdt_begin_node(fdt, name);
			iova_start = iopt_area_iova(area);
			iova_len = iopt_area_length(area);
			err |= fdt_property(fdt, "iova-start",
					&iova_start, sizeof(iova_start));
			err |= fdt_property(fdt, "iova-len",
					&iova_len, sizeof(iova_len));
			err |= fdt_property(fdt, "iommu-prot",
					&area->iommu_prot, sizeof(area->iommu_prot));
			err |= fdt_end_node(fdt); /* area_idx */
			++area_idx;
		}
		err |= fdt_end_node(fdt); /* ioas obj_idx */
	}
	err |= fdt_end_node(fdt); /* ioases*/
	err |= fdt_end_node(fdt); /* ictx->persistent_id */
	return 0;
}

int iommufd_serialise_kho(struct notifier_block *self, unsigned long cmd,
			  void *fdt)
{
	static const char compatible[] = "iommufd-v0";
	struct iommufd_ctx *ictx;
	unsigned long xa_idx;
	int err = 0;

	switch (cmd) {
	case KEXEC_KHO_ABORT:
		/* Would do serialise rollback here. */
		return NOTIFY_DONE;
	case KEXEC_KHO_DUMP:
		err |= fdt_begin_node(fdt, "iommufd");
		fdt_property(fdt, "compatible", compatible, sizeof(compatible));
		err |= fdt_begin_node(fdt, "iommufds");
		xa_for_each(&persistent_iommufds, xa_idx, ictx) {
			err |= serialise_iommufd(fdt, ictx);
		}
		err |= fdt_end_node(fdt); /* iommufds */
		err |= fdt_end_node(fdt); /* iommufd */
		return err? NOTIFY_BAD : NOTIFY_DONE;
	default:
		return NOTIFY_BAD;
	}
}

static int rehydrate_iommufd(char *iommufd_name)
{
	struct file *file;
	int fd;
	int off;
	struct iommufd_ctx *ictx;
	struct files_struct *files = current->files;  // Current process's files_struct
	const void *fdt = kho_get_fdt();
	char kho_path[42];

	fd = anon_inode_getfd("iommufd", &iommufd_fops, NULL, O_RDWR);
	if (fd < 0)
		return fd;
	file = files_lookup_fd_raw(files, fd);
	iommufd_fops_open(NULL, file);
	ictx = file->private_data;

	snprintf(kho_path, sizeof(kho_path), "/iommufd/iommufds/%s/ioases", iommufd_name);
	fdt_for_each_subnode(off, fdt, fdt_path_offset(fdt, kho_path)) {
	    struct iommufd_ioas *ioas;
	    int range_off;

	    ioas = iommufd_ioas_alloc(ictx);
	    iommufd_object_finalize(ictx, &ioas->obj);

	    fdt_for_each_subnode(range_off, fdt, off) {
		    const unsigned long *iova_start, *iova_len;
		    const int *iommu_prot;
		    int len;
		    struct iopt_area *area = iopt_area_alloc();

		    iova_start = fdt_getprop(fdt, range_off, "iova-start", &len);
		    iova_len = fdt_getprop(fdt, range_off, "iova-len", &len);
		    iommu_prot = fdt_getprop(fdt, range_off, "iommu-prot", &len);

		    area->iommu_prot = *iommu_prot;
		    area->node.start = *iova_start;
		    area->node.last = *iova_start + *iova_len - 1;
		    interval_tree_insert(&area->node, &ioas->iopt.area_itree);
	    }
	    /*
	     * Here we should do something to associate struct iommufd_device with the
	     * ictx, then get the iommu_ops via dev_iommu_ops(), and call the new
	     * .domain_restore callback to get the struct iommu_domain.
	     * Something like:
	     * hwpt->domain = ops->domain_restore(dev, persistent_id);
	     * Hand wavy - the details allude me at the moment...
	     */
	}

	return fd;
}

static ssize_t iommufd_show(struct kobject *kobj, struct kobj_attribute *attr,
	char *buf)
{
	char fd_str[10];
	ssize_t len;

	len = snprintf(buf, sizeof(fd_str), "%i\n", rehydrate_iommufd("1"));
	return len;
}

static struct kobj_attribute persisted_attr =
	__ATTR_RO_MODE(iommufd, 0440);

static int deserialise_iommufds(const void *fdt, int root_off)
{
	int off;

	/*
	 * For each persisted iommufd id, create a directory
	 * in sysfs with an iommufd file in it.
	 */
	fdt_for_each_subnode(off, fdt, root_off) {
		struct kobject *kobj;
		const char *name = fdt_get_name(fdt, off, NULL);
		int rc;

		kobj = kobject_create_and_add(name, persisted_dir_kobj);
		rc = sysfs_create_file(kobj, &persisted_attr.attr);
		if (rc)
			pr_warn("Unable to create sysfs file for iommufd node %s\n", name);
	}
	return 0;
}

int __init iommufd_deserialise_kho(void)
{
	const void *fdt = kho_get_fdt();
	int off;

	if (!fdt)
		return 0;

	/* Parent directory for persisted iommufd files. */
	persisted_dir_kobj = kobject_create_and_add("iommufd_persisted", kernel_kobj);

	off = fdt_path_offset(fdt, "/iommufd");
	if (off <= 0)
		return 0; /* No data in KHO */

	deserialise_iommufds(fdt, fdt_subnode_offset(fdt, off, "iommufds"));
	return 0;
}

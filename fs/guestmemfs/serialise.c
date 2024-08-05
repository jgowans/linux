// SPDX-License-Identifier: GPL-2.0-only

#include "guestmemfs.h"
#include <linux/kexec.h>
#include <linux/memblock.h>

/*
 * Responsible for serialisation and deserialisation of filesystem metadata
 * to and from KHO to survive kexec. The deserialisation logic needs to mirror
 * serialisation, so putting them in the same file.
 *
 * The format of the device tree structure is:
 *
 * /guestmemfs
 *   compatible = "guestmemfs-v1"
 *   fs_mem {
 *     mem = [ ... ]
 *   };
 *   superblock {
 *     mem = [
 *       persistent super block,
 *       inodes,
 *       allocator_bitmap,
 *   };
 *   mappings_block {
 *     mem = [ ... ]
 *   };
 *   // For every mappings_block mem, which inode it belongs to.
 *   mappings_to_inode {
 *     num_inodes,
 *     mem = [ ... ],
 *   }
 */

static int serialise_superblock(struct super_block *sb, void *fdt)
{
	struct kho_mem mem[3];
	int err = 0;
	struct guestmemfs_sb *psb = sb->s_fs_info;

	err |= fdt_begin_node(fdt, "superblock");

	mem[0].addr = virt_to_phys(psb);
	mem[0].len = sizeof(*psb);

	mem[1].addr = virt_to_phys(psb->inodes);
	mem[1].len = 2 << 20;

	mem[2].addr = virt_to_phys(psb->allocator_bitmap);
	mem[2].len = 1 << 20;

	err |= fdt_property(fdt, "mem", &mem, sizeof(mem));
	err |= fdt_end_node(fdt);

	return err;
}

static int serialise_mappings_blocks(struct super_block *sb, void *fdt)
{
	struct kho_mem *mappings_mems;
	struct kho_mem mappings_to_inode_mem;
	struct guestmemfs_sb *psb = sb->s_fs_info;
	int inode_idx;
	size_t num_inodes = PMD_SIZE / sizeof(struct guestmemfs_inode);
	struct guestmemfs_inode *inode;
	int err = 0;
	int *mappings_to_inode;
	int mappings_to_inode_idx = 0;

	mappings_to_inode = kzalloc(PAGE_SIZE, GFP_KERNEL);

	mappings_mems = kcalloc(psb->allocated_inodes, sizeof(struct kho_mem), GFP_KERNEL);

	for (inode_idx = 1; inode_idx < num_inodes; ++inode_idx) {
		inode = guestmemfs_get_persisted_inode(sb, inode_idx);
		if (inode->flags & GUESTMEMFS_INODE_FLAG_FILE) {
			mappings_mems[mappings_to_inode_idx].addr = virt_to_phys(inode->mappings);
			mappings_mems[mappings_to_inode_idx].len = PAGE_SIZE;
			mappings_to_inode[mappings_to_inode_idx] = inode_idx;
			mappings_to_inode_idx++;
		}
	}

	err |= fdt_begin_node(fdt, "mappings_blocks");
	err |= fdt_property(fdt, "mem", mappings_mems,
		sizeof(struct kho_mem) * mappings_to_inode_idx);
	err |= fdt_end_node(fdt);


	err |= fdt_begin_node(fdt, "mappings_to_inode");
	mappings_to_inode_mem.addr = virt_to_phys(mappings_to_inode);
	mappings_to_inode_mem.len = PAGE_SIZE;
	err |= fdt_property(fdt, "mem", &mappings_to_inode_mem,
			sizeof(mappings_to_inode_mem));
	err |= fdt_property(fdt, "num_inodes", &psb->allocated_inodes,
			sizeof(psb->allocated_inodes));

	err |= fdt_end_node(fdt);

	return err;
}

int guestmemfs_serialise_to_kho(struct notifier_block *self,
			      unsigned long cmd,
			      void *v)
{
	static const char compatible[] = "guestmemfs-v1";
	struct kho_mem mem;
	void *fdt = v;
	int err = 0;

	switch (cmd) {
	case KEXEC_KHO_ABORT:
		GUESTMEMFS_PSB(guestmemfs_sb)->serialised = 0;
		return NOTIFY_DONE;
	case KEXEC_KHO_DUMP:
		/* Handled below */
		break;
	default:
		return NOTIFY_BAD;
	}

	spin_lock(&GUESTMEMFS_PSB(guestmemfs_sb)->allocation_lock);
	err |= fdt_begin_node(fdt, "guestmemfs");
	err |= fdt_property(fdt, "compatible", compatible, sizeof(compatible));

	err |= fdt_begin_node(fdt, "fs_mem");
	mem.addr = guestmemfs_base | KHO_MEM_ADDR_FLAG_NOINIT;
	mem.len = guestmemfs_size;
	err |= fdt_property(fdt, "mem", &mem, sizeof(mem));
	err |= fdt_end_node(fdt);

	err |= serialise_superblock(guestmemfs_sb, fdt);
	err |= serialise_mappings_blocks(guestmemfs_sb, fdt);

	err |= fdt_end_node(fdt);

	if (!err)
		GUESTMEMFS_PSB(guestmemfs_sb)->serialised = 1;

	spin_unlock(&GUESTMEMFS_PSB(guestmemfs_sb)->allocation_lock);

	pr_info("Serialised extends [0x%llx + 0x%llx] via KHO: %i\n",
			guestmemfs_base, guestmemfs_size, err);

	return err;
}

static struct guestmemfs_sb *deserialise_superblock(const void *fdt, int root_off)
{
	const struct kho_mem *mem;
	int mem_len;
	struct guestmemfs_sb *old_sb;
	int off;

	off = fdt_subnode_offset(fdt, root_off, "superblock");
	mem = fdt_getprop(fdt, off, "mem", &mem_len);

	if (mem_len != 3 * sizeof(struct kho_mem)) {
		pr_err("Incorrect mem_len; got %i\n", mem_len);
		return NULL;
	}

	old_sb = kho_claim_mem(mem);
	old_sb->inodes = kho_claim_mem(mem + 1);
	old_sb->allocator_bitmap = kho_claim_mem(mem + 2);

	return old_sb;
}

static int deserialise_mappings_blocks(const void *fdt, int root_off,
		struct guestmemfs_sb *sb)
{
	int off;
	int len = 0;
	const unsigned long *num_inodes;
	const struct kho_mem *mappings_to_inode_mem;
	int *mappings_to_inode;
	int mappings_block;
	const struct kho_mem *mappings_blocks_mems;

	/*
	 * Array of struct kho_mem - one for each persisted mappings
	 * blocks.
	 */
	off = fdt_subnode_offset(fdt, root_off, "mappings_blocks");
	mappings_blocks_mems = fdt_getprop(fdt, off, "mem", &len);

	/*
	 * Array specifying which inode a specific index into the
	 * mappings_blocks kho_mem array corresponds to. num_inodes
	 * indicates the size of the array which is the number of mappings
	 * blocks which need to be restored.
	 */
	off = fdt_subnode_offset(fdt, root_off, "mappings_to_inode");
	if (off < 0) {
		pr_warn("No fs_mem available in KHO\n");
		return -EINVAL;
	}
	num_inodes = fdt_getprop(fdt, off, "num_inodes", &len);
	if (len != sizeof(num_inodes)) {
		pr_warn("Invalid num_inodes len: %i\n", len);
		return -EINVAL;
	}
	mappings_to_inode_mem = fdt_getprop(fdt, off, "mem", &len);
	if (len != sizeof(*mappings_to_inode_mem)) {
		pr_warn("Invalid mappings_to_inode_mem len: %i\n", len);
		return -EINVAL;
	}
	mappings_to_inode = kho_claim_mem(mappings_to_inode_mem);

	/*
	 * Re-assigned the mappings block to the inodes. Indexes into
	 * mappings_to_inode specifies which inode to assign each mappings
	 * block to.
	 */
	for (mappings_block = 0; mappings_block < *num_inodes; ++mappings_block) {
		int inode = mappings_to_inode[mappings_block];

		sb->inodes[inode].mappings = kho_claim_mem(&mappings_blocks_mems[mappings_block]);
	}

	return 0;
}

static int deserialise_fs_mem(const void *fdt, int root_off)
{
	int err;
	/* Offset into the KHO DT */
	int off;
	int len = 0;
	const struct kho_mem *mem;

	off = fdt_subnode_offset(fdt, root_off, "fs_mem");
	if (off < 0) {
		pr_info("No fs_mem available in KHO\n");
		return -EINVAL;
	}

	mem = fdt_getprop(fdt, off, "mem", &len);
	if (mem && len == sizeof(*mem)) {
		guestmemfs_base = mem->addr & ~KHO_MEM_ADDR_FLAG_MASK;
		guestmemfs_size = mem->len;
	} else {
		pr_err("KHO did not contain a guestmemfs base address and size\n");
		return -EINVAL;
	}

	pr_info("Reclaimed [%llx + %llx] via KHO\n", guestmemfs_base, guestmemfs_size);
	if (err) {
		pr_err("Unable to reserve [0x%llx + 0x%llx] from memblock: %i\n",
				guestmemfs_base, guestmemfs_size, err);
		return err;
	}
	return 0;
}
struct guestmemfs_sb *guestmemfs_restore_from_kho(void)
{
	const void *fdt = kho_get_fdt();
	struct guestmemfs_sb *old_sb;
	int err;
	/* Offset into the KHO DT */
	int off;

	if (!fdt) {
		pr_err("Unable to get KHO DT after KHO boot?\n");
		return NULL;
	}

	off = fdt_path_offset(fdt, "/guestmemfs");
	pr_info("guestmemfs offset: %i\n", off);

	if (!off) {
		pr_info("No guestmemfs data available in KHO\n");
		return NULL;
	}
	err = fdt_node_check_compatible(fdt, off, "guestmemfs-v1");
	if (err) {
		pr_err("Existing KHO superblock format is not compatible with this kernel\n");
		return NULL;
	}

	old_sb = deserialise_superblock(fdt, off);
	if (!old_sb) {
		pr_warn("Failed to restore superblock\n");
		return NULL;
	}

	err = deserialise_mappings_blocks(fdt, off, old_sb);
	if (err) {
		pr_warn("Failed to restore mappings blocks\n");
		return NULL;
	}

	err = deserialise_fs_mem(fdt, off);
	if (err) {
		pr_warn("Failed to restore filesystem memory extents\n");
		return NULL;
	}

	return old_sb;
}

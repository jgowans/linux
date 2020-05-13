#include <linux/module.h>    // included for all kernel modules
#include <linux/kernel.h>    // included for KERN_INFO
#include <linux/init.h>      // included for __init and __exit macros
#include <linux/moduleparam.h>
// #include <asm/io.h>
// #include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>

// Arch?
#include <asm/page_types.h>
#include <asm/page.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jgowans");
MODULE_DESCRIPTION("Tests that the vIOMMU is doing the right thing.");

#define BAR 0

static int num;

static struct pci_dev *global_pci_device;
static unsigned long __iomem *mmio;

/* ---------------------------------- */

static int do_dma_callback(const char *val, const struct kernel_param *kp) {
	void *buffer;
	dma_addr_t dma_addr;
	unsigned long n_bytes;
	n_bytes = 1000 * PAGE_SIZE;

	// TODO: dynamic number of pages.
	printk("Got echo_callback with: %s\n", val);

	buffer = kmalloc(n_bytes, GFP_KERNEL);
	// lower bits should be free; we're going to use them.
	BUG_ON(((unsigned long)buffer) & (PAGE_SIZE - 1));
	printk("kmalloced 0x%lx bytes: %px\n", n_bytes, buffer);

	// TODO: zero first word of all pages.
	dma_addr = dma_map_page(&(global_pci_device->dev), virt_to_page(buffer),
		0, n_bytes, DMA_BIDIRECTIONAL);
	printk("DMA addr: 0x%llx\n", dma_addr);
	iowrite32(dma_addr, mmio);
	iowrite32(n_bytes, mmio + 1);
	// TODO: validate magic number on all pages.
	return 0;
}

struct kernel_param_ops do_dma_ops = {
	.set = do_dma_callback
};
module_param_cb(do_dma_cb, &do_dma_ops, &num, 0664);

/* ---------------------------------- */

static int probe(struct pci_dev *dev, const struct pci_device_id *id) {
	printk("jgowans_dma_tester probe with device: %px\n", &dev->dev);
	global_pci_device = dev;
	if (pci_enable_device(dev) < 0) {
		dev_err(&(dev->dev), "pci_enable_device\n");
		goto error;
	}
	if (pci_request_region(dev, BAR, "myregion0")) {
		dev_err(&(dev->dev), "pci_request_region\n");
		goto error;
	}
	mmio = pci_iomap(dev, BAR, pci_resource_len(dev, BAR));

	printk("------- jgowans doing MMIO write now -----------------\n");
	iowrite32(0xABFFFFFF, mmio);
	return 0;
error:
	return 1;
}

static void remove(struct pci_dev *dev) {
}

static struct pci_device_id id_table[] = {
    { PCI_DEVICE(0x1234, 0x6345), },
    { 0, }
};

static struct pci_driver pci_driver = {
    .name     = "jgowans-dma-tester",
    .id_table = id_table,
    .probe    = probe,
    .remove   = remove,
};

/* ---------------------------------- */

static int __init jgowans_dma_tester_init(void)
{
	printk(KERN_INFO "Hello world! from jgowans moudle\n");
	pci_register_driver(&pci_driver);
	return 0;    // Non-zero return means that the module couldn't be loaded.
}

static void __exit jgowans_dma_tester_cleanup(void)
{
	printk(KERN_INFO "Cleaning up module.\n");
}

module_init(jgowans_dma_tester_init);
module_exit(jgowans_dma_tester_cleanup);


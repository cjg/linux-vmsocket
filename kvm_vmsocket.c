#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/proc_fs.h>
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/seq_file.h>
#include <linux/cdev.h>
#include <linux/pci.h>

#include <asm/system.h>		/* cli(), *_flags */
#include <asm/uaccess.h>	/* copy_*_user */

MODULE_DESCRIPTION("Guest driver for the VMSocket PCI Device.");
MODULE_AUTHOR("Giuseppe Coviello <cjg@cruxppc.org>");
MODULE_LICENSE("GPL");

#define VMSOCKET_DEBUG

#undef PDEBUG             /* undef it, just in case */
#ifdef VMSOCKET_DEBUG
#  ifdef __KERNEL__
     /* This one if debugging is on, and kernel space */
#    define PDEBUG(fmt, args...) printk( KERN_ERR "kvm_vmsocket: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

#undef PDEBUGG
#define PDEBUGG(fmt, args...) /* nothing: it's a placeholder */

#ifndef VMSOCKET_MAJOR
#define VMSOCKET_MAJOR 0   /* dynamic major by default */
#endif

#define VMSOCKET_CONNRQ_REG(dev) ((struct vmsocket_dev *)(dev))->regs
#define VMSOCKET_CONNST_REG(dev) (((struct vmsocket_dev *)(dev))->regs + 0x4)

struct vmsocket_dev {
	void __iomem * regs;

	void * base_addr;

	unsigned int regaddr;
	unsigned int reg_size;

	unsigned int ioaddr;
	unsigned int ioaddr_size;
	unsigned int irq;

	struct semaphore sem;
	struct cdev cdev;
} kvm_vmsocket_device;

static struct vmsocket_dev vmsocket_dev;
static atomic_t vmsocket_available = ATOMIC_INIT(1);
int vmsocket_major = VMSOCKET_MAJOR;
int vmsocket_minor = 0;

static int vmsocket_open(struct inode *inode, struct file *filp)
{
	int status;
	struct vmsocket_dev *dev = &vmsocket_dev;

	if(! atomic_dec_and_test (&vmsocket_available)) {
		atomic_inc(&vmsocket_available);
		return -EBUSY;
	}

	/* TODO: establish connection */ 
	writew(0xFFFF, VMSOCKET_CONNRQ_REG(&vmsocket_dev));
	if((status = readl(VMSOCKET_CONNST_REG(&vmsocket_dev))) < 0) {
		PDEBUG("can't establish connection.\n");
		atomic_inc(&vmsocket_available);
		return status;
	}

	filp->private_data = dev;
	return 0;
}

static int vmsocket_release(struct inode *inode, struct file *filp)
{
	PDEBUG("vmsocket_release()\n");
	/* TODO: close connection */

	atomic_inc(&vmsocket_available);
	return 0;
}

static ssize_t vmsocket_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct vmsocket_dev *dev = filp->private_data;
	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	/* TODO: read */

	up(&dev->sem);
	return 0;
}

static ssize_t vmsocket_write(struct file *filp, const char __user *buf, 
			      size_t count, loff_t *f_pos)
{
	struct vmsocket_dev *dev = filp->private_data;
	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	/* TODO: write */

	up(&dev->sem);
	return 0;
}

static const struct file_operations vmsocket_fops = {
    .owner   = THIS_MODULE,
    .open    = vmsocket_open,
    .release = vmsocket_release,
    .read    = vmsocket_read,
    .write   = vmsocket_write,
};

static struct pci_device_id kvm_vmsocket_id_table[] = {
    { 0x1af4, 0x1110, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
    { 0 },
};
MODULE_DEVICE_TABLE (pci, kvm_vmsocket_id_table);

static int vmsocket_probe (struct pci_dev *pdev, 
			   const struct pci_device_id * ent)
{
	int result;
	PDEBUG("vmsocket_probe()\n");
	
	result = pci_enable_device(pdev);
	
	if (result) {
		printk(KERN_ERR "Cannot probe VMSocket device %s: error %d\n",
		       pci_name(pdev), result);
		return result;
	}

	result = pci_request_regions(pdev, "kvm_vmsocket");
	if (result < 0) {
		printk(KERN_ERR "VMSocket: cannot request regions\n");
		goto pci_disable;
	}

	vmsocket_dev.ioaddr = pci_resource_start(pdev, 1);
	vmsocket_dev.ioaddr_size = pci_resource_len(pdev, 1);

	vmsocket_dev.base_addr = pci_iomap(pdev, 1, 0);
	printk(KERN_INFO "VMSocket: iomap base = 0x%lu \n",
	       (unsigned long) vmsocket_dev.base_addr);

	if (!vmsocket_dev.base_addr) {
		printk(KERN_ERR "VMSocket: cannot iomap region of size %d\n",
		       vmsocket_dev.ioaddr_size);
		goto pci_release;
	}

	printk(KERN_INFO "VMSocket: ioaddr = %x ioaddr_size = %d\n",
	       vmsocket_dev.ioaddr, vmsocket_dev.ioaddr_size);

	vmsocket_dev.regaddr =  pci_resource_start(pdev, 0);
	vmsocket_dev.reg_size = pci_resource_len(pdev, 0);
	vmsocket_dev.regs = pci_iomap(pdev, 0, 0x100);

	if (!vmsocket_dev.regs) {
		printk(KERN_ERR "VMSocket: cannot ioremap registers of size %d\n",
		       vmsocket_dev.reg_size);
		goto reg_release;
	}

	init_MUTEX(&vmsocket_dev.sem);
	cdev_init(&vmsocket_dev.cdev, &vmsocket_fops);
	vmsocket_dev.cdev.owner = THIS_MODULE;
	vmsocket_dev.cdev.ops = &vmsocket_fops;
	result = cdev_add(&vmsocket_dev.cdev, MKDEV(vmsocket_major, 
						    vmsocket_minor), 1);
	if(result)
		printk(KERN_ERR "Error %d adding vmsocket%d", result,
		       vmsocket_minor);

	printk(KERN_INFO 
	       "Registered kvm_vmsocket device, major: %d minor: %d.\n",
	       vmsocket_major, vmsocket_minor);

	return 0;

reg_release:
	pci_iounmap(pdev, vmsocket_dev.base_addr);
pci_release:
	pci_release_regions(pdev);
pci_disable:
	pci_disable_device(pdev);
	return -EBUSY;
}

static void vmsocket_remove(struct pci_dev* pdev)
{
	printk(KERN_INFO "Unregistered kvm_vmsocket device.\n");
	pci_iounmap(pdev, vmsocket_dev.regs);
	pci_iounmap(pdev, vmsocket_dev.base_addr);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static struct pci_driver vmsocket_pci_driver = {
    .name        = "kvm_vmsocket",
    .id_table    = kvm_vmsocket_id_table,
    .probe       = vmsocket_probe,
    .remove      = vmsocket_remove,
};


static int __init vmsocket_init_module(void)
{
	int result;
	dev_t dev = 0;

	if(vmsocket_major) {
		dev = MKDEV(vmsocket_major, vmsocket_minor);
		result = register_chrdev_region(dev, 1, "kvm_vmsocket");
	} else {
		result = alloc_chrdev_region(&dev, vmsocket_minor, 1,
					     "kvm_vmsocket");
		vmsocket_major = MAJOR(dev);
	}

	if(result < 0) {
		printk(KERN_ERR "kvm_vmsocket: can't get major %d\n", 
		       vmsocket_major);
		return result;
	}

	pci_register_driver(&vmsocket_pci_driver);

	return 0;
}
module_init(vmsocket_init_module);

static void __exit vmsocket_exit(void)
{
	cdev_del(&vmsocket_dev.cdev);
	pci_unregister_driver (&vmsocket_pci_driver);
	unregister_chrdev_region(MKDEV(vmsocket_major, vmsocket_minor), 1);
}
module_exit(vmsocket_exit);


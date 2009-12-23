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
#    define PDEBUG(fmt, args...) printk( KERN_CRIT "kvm_vmsocket: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

#undef PDEBUGG
#define PDEBUGG(fmt, args...) /* nothing: it's a placeholder */

#define VMSOCKET_ERR(fmt, args...) printk( KERN_ERR "kvm_vmsocket: " fmt "\n", ## args)
#define VMSOCKET_INFO(fmt, args...) printk( KERN_INFO "kvm_vmsocket: " fmt "\n", ## args)

#ifndef VMSOCKET_MAJOR
#define VMSOCKET_MAJOR 0   /* dynamic major by default */
#endif

/* Registers */
/* Read Only */
#define VMSOCKET_STATUS_L_REG(dev)       ((dev)->regs)
/* Write Only */
#define VMSOCKET_CONNECT_W_REG(dev)      ((dev)->regs + 0x20)
#define VMSOCKET_CLOSE_W_REG(dev)        ((dev)->regs + 0x30)
#define VMSOCKET_WRITE_COMMIT_L_REG(dev) ((dev)->regs + 0x40)
#define VMSOCKET_READ_L_REG(dev)         ((dev)->regs + 0x60)

struct vmsocket_dev {
	void __iomem * regs;
	uint32_t regaddr;
	uint32_t reg_size;

	void * inbuffer;
	uint32_t inbuffer_size;
	uint32_t inbuffer_length;
	uint32_t inbuffer_addr;

	void * outbuffer;
	uint32_t outbuffer_size;
	uint32_t outbuffer_length;
	uint32_t outbuffer_addr;

	struct semaphore sem;
	struct cdev cdev;
} kvm_vmsocket_device;

static struct vmsocket_dev vmsocket_dev;
static atomic_t vmsocket_available = ATOMIC_INIT(1);
static struct class *fc = NULL;
int vmsocket_major = VMSOCKET_MAJOR;
int vmsocket_minor = 0;

static void vmsocket_write_commit(struct vmsocket_dev *dev) 
{
	if(dev->outbuffer_length > 0) {
		writel(dev->outbuffer_length, VMSOCKET_WRITE_COMMIT_L_REG(dev));
		dev->outbuffer_length = 0;
	}
}

static int vmsocket_open(struct inode *inode, struct file *filp)
{
	int status;
	struct vmsocket_dev *dev = &vmsocket_dev;

	if(!atomic_dec_and_test(&vmsocket_available)) {
		atomic_inc(&vmsocket_available);
		return -EBUSY;
	}

	writew(0xFFFF, VMSOCKET_CONNECT_W_REG(&vmsocket_dev));
	if((status = readl(VMSOCKET_STATUS_L_REG(&vmsocket_dev))) < 0) {
		VMSOCKET_ERR("can't establish connection.");
		atomic_inc(&vmsocket_available);
		return status;
	}

	filp->private_data = dev;
	return 0;
}

static int vmsocket_release(struct inode *inode, struct file *filp)
{
	struct vmsocket_dev *dev = filp->private_data;
	int status;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	
	vmsocket_write_commit(dev);
	writew(0xFFFF, VMSOCKET_CLOSE_W_REG(dev));
	if((status = readl(VMSOCKET_STATUS_L_REG(&vmsocket_dev))) != 0) 
		VMSOCKET_ERR("can't close connection.");
	atomic_inc(&vmsocket_available);
	up(&dev->sem);
	return status;
}

static ssize_t vmsocket_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct vmsocket_dev *dev = filp->private_data;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	vmsocket_write_commit(dev);

	if(count > dev->inbuffer_size)
		count = dev->inbuffer_size;

	writel(count, VMSOCKET_READ_L_REG(dev));
	count = readl(VMSOCKET_STATUS_L_REG(dev));

	if (count == 0) {
		*f_pos = 0;
		up(&dev->sem);
		return 0;
	}

	if(copy_to_user(buf, dev->inbuffer, count) > 0) {
		up(&dev->sem);
		return -EFAULT;
	}

	*f_pos += count;
	up(&dev->sem);
	return count;
}

static ssize_t vmsocket_write(struct file *filp, const char __user *buf, 
			      size_t count, loff_t *f_pos)
{
	unsigned long offset;
	struct vmsocket_dev *dev = filp->private_data;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	offset = dev->outbuffer_length;

	if (count > dev->outbuffer_size - offset) 
		count = dev->outbuffer_size - offset;

	if (count == 0) {
		*f_pos = 0;
		up(&dev->sem);
		return 0;
	}

	if(copy_from_user(dev->outbuffer + dev->outbuffer_length, buf, count) 
	   > 0) {
		up(&dev->sem);
		return -EFAULT;
	}

	dev->outbuffer_length += count;
	if(dev->outbuffer_length == dev->outbuffer_size)
		vmsocket_write_commit(dev);

	*f_pos += count;
	up(&dev->sem);
	return count;
}

static int vmsocket_flush(struct file *filp) {
	struct vmsocket_dev *dev = filp->private_data;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	vmsocket_write_commit(dev);

	up(&dev->sem);
	return 0;
}

int vmsocket_ioctl(struct inode *inode,	/* see include/linux/fs.h */
		   struct file *file,	/* ditto */
		   unsigned int ioctl_num,	/* number and param for ioctl */
		   unsigned long ioctl_param) {
	
	PDEBUG("ioctl(): num: %u param: %lu\n", ioctl_num, ioctl_param);
	return 0;
}

static const struct file_operations vmsocket_fops = {
    .owner   = THIS_MODULE,
    .open    = vmsocket_open,
    .release = vmsocket_release,
    .read    = vmsocket_read,
    .write   = vmsocket_write,
    .flush   = vmsocket_flush,
    /* .fsync   = vmsocket_fsync, */
    /* .ioctl   = vmsocket_ioctl, */
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

	result = pci_enable_device(pdev);

	if (result) {
		VMSOCKET_ERR("cannot probe device %s: error %d.",
		       pci_name(pdev), result);
		return result;
	}

	result = pci_request_regions(pdev, "kvm_vmsocket");
	if (result < 0) {
		VMSOCKET_ERR("cannot request regions.");
		goto pci_disable;
	}

	/* Registers */
	vmsocket_dev.regaddr =  pci_resource_start(pdev, 0);
	vmsocket_dev.reg_size = pci_resource_len(pdev, 0);
	vmsocket_dev.regs = pci_iomap(pdev, 0, 0x100);
	if (!vmsocket_dev.regs) {
		VMSOCKET_ERR("cannot ioremap registers.");
		goto reg_release;
	}
	
	/* I/O Buffers */
	vmsocket_dev.inbuffer_addr =  pci_resource_start(pdev, 1);
	vmsocket_dev.inbuffer = pci_iomap(pdev, 1, 0);
	vmsocket_dev.inbuffer_size = pci_resource_len(pdev, 1);
	vmsocket_dev.inbuffer_length = 0;
	if (!vmsocket_dev.inbuffer) {
		VMSOCKET_ERR("cannot ioremap input buffer.");
		goto in_release;
	}

	vmsocket_dev.outbuffer_addr =  pci_resource_start(pdev, 2);
	vmsocket_dev.outbuffer = pci_iomap(pdev, 2, 0);
	vmsocket_dev.outbuffer_size = pci_resource_len(pdev, 2);
	vmsocket_dev.outbuffer_length = 0;
	if (!vmsocket_dev.outbuffer) {
		VMSOCKET_ERR("cannot ioremap output buffer.");
		goto out_release;
	}

	init_MUTEX(&vmsocket_dev.sem);
	cdev_init(&vmsocket_dev.cdev, &vmsocket_fops);
	vmsocket_dev.cdev.owner = THIS_MODULE;
	vmsocket_dev.cdev.ops = &vmsocket_fops;
	result = cdev_add(&vmsocket_dev.cdev, MKDEV(vmsocket_major, 
						    vmsocket_minor), 1);
	if(result)
		VMSOCKET_ERR("error %d adding vmsocket%d", result, 
			     vmsocket_minor);

	VMSOCKET_INFO("registered device, major: %d minor: %d.",
		      vmsocket_major, vmsocket_minor);
	VMSOCKET_INFO("input buffer size: %d @ 0x%x.", 
		      vmsocket_dev.inbuffer_size, vmsocket_dev.inbuffer_addr);
	VMSOCKET_INFO("output buffer size: %d @ 0x%x.", 
		      vmsocket_dev.outbuffer_size, vmsocket_dev.outbuffer_addr);

	/* create sysfs entry */
	if(fc == NULL)
		fc = class_create(THIS_MODULE, "vmsocket");
	device_create(fc, NULL, vmsocket_dev.cdev.dev, "%s%d", "vmsocket", 
		      vmsocket_minor);

	return 0;

  out_release:
	pci_iounmap(pdev, vmsocket_dev.inbuffer);
  in_release:
	pci_iounmap(pdev, vmsocket_dev.regs);
  reg_release:
	pci_release_regions(pdev);
  pci_disable:
	pci_disable_device(pdev);
	return -EBUSY;
}

static void vmsocket_remove(struct pci_dev* pdev)
{
	VMSOCKET_INFO("unregistered device.");
	device_destroy(fc, vmsocket_dev.cdev.dev);
	pci_iounmap(pdev, vmsocket_dev.regs);
	pci_iounmap(pdev, vmsocket_dev.inbuffer);
	pci_iounmap(pdev, vmsocket_dev.outbuffer);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	if(fc != NULL) {
		class_destroy(fc);
		fc = NULL;
	}
		
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
		VMSOCKET_ERR("can't get major %d.", vmsocket_major);
		return result;
	}

	if((result = pci_register_driver(&vmsocket_pci_driver)) != 0) {
		VMSOCKET_ERR("can't register PCI driver.");
		return result;
	}

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


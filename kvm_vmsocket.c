#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/smp_lock.h>
#include <asm/uaccess.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>

typedef struct {
    void __iomem * regs;

    void * base_addr;

    unsigned int regaddr;
    unsigned int reg_size;

    unsigned int ioaddr;
    unsigned int ioaddr_size;
    unsigned int irq;
} kvm_vmsocket_device;

typedef struct {
    kvm_vmsocket_device * master;
    int host_fd;
} kvm_vmsocket_slave;

static struct semaphore sema;
static wait_queue_head_t wait_queue;
static int event_present;
static kvm_vmsocket_device kvm_vmsocket_dev;

static int kvm_vmsocket_ioctl(struct inode *, struct file *, unsigned int,
        unsigned long);

static const struct file_operations kvm_vmsocket_ops = {
    .owner   = THIS_MODULE,
    .ioctl   = kvm_vmsocket_ioctl,
};

static struct pci_device_id kvm_vmsocket_id_table[] = {
    { 0x1af4, 0x1110, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
    { 0 },
};
MODULE_DEVICE_TABLE (pci, kvm_vmsocket_id_table);

static int kvm_vmsocket_probe_device (struct pci_dev *pdev,
        const struct pci_device_id * ent)
{
    sema_init(&sema, 0);
    init_waitqueue_head(&wait_queue);
    event_present = 0;
}

static void kvm_vmsocket_remove_device(struct pci_dev* pdev)
{

}

static struct pci_driver kvm_vmsocket_pci_driver = {
    .name        = "kvm-vmsocket",
    .id_table    = kvm_vmsocket_id_table,
    .probe       = kvm_vmsocket_probe_device,
    .remove      = kvm_vmsocket_remove_device,
};

static int kvm_vmsocket_ioctl(struct inode * ino, struct file * filp,
        unsigned int cmd, unsigned long arg)
{
    int result;
    printk(KERN_CRIT "KVM_VMSOCKET: cmd: %x.\n Waiting for mutex ...\n", cmd);
    down_interruptible(&sema);
    switch (cmd) {
        case 0x434f4e4e: /* CONN */
            printk(KERN_CRIT "KVM_VMSOCKET: Connect.\n");
            writew(cmd, kvm_vmsocket_dev.regs);
            wait_event_interruptible(wait_queue, (event_present == 1));
            event_present = 0;
            result = readw(kvm_vmsocket_dev.regs + sizeof(int));
            break;
        default:
            printk("KVM_VMSOCKET: bad ioctl\n");
            result = -1;
    }
    up(&sema);
    return result;
}


static int __init vmsocket_init(void)
{
	return 0;
}
module_init(vmsocket_init);

static void __exit vmsocket_exit(void)
{
}
module_exit(vmsocket_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Giuseppe Coviello <cjg@cruxppc.org>");
MODULE_DESCRIPTION("Guest driver for the VMSocket PCI Device.");

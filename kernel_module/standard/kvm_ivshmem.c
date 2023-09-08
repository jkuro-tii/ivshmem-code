/* drivers/char/kvm_ivshmem.c - driver for KVM Inter-VM shared memory PCI device
 *
 * Copyright 2009 Cam Macdonell <cam@cs.ualberta.ca>
 *
 * Based on cirrusfb.c and 8139cp.c:
 *         Copyright 1999-2001 Jeff Garzik
 *         Copyright 2001-2004 Jeff Garzik
 *
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <asm/uaccess.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>

#define TRUE 1
#define FALSE 0
#define KVM_IVSHMEM_DEVICE_MINOR_NUM 0
#define VECTORS_COUNT (1)

#define DEBUG
#ifdef DEBUG
#define KVM_IVSHMEM_DPRINTK(fmt, ...)                       \
  do {                                                \
    printk(KERN_INFO "KVM_IVSHMEM: " fmt "\n", ## __VA_ARGS__);    \
  } while (0)
#else
#define KVM_IVSHMEM_DPRINTK(fmt, ...) {}
#endif
enum {
	/* KVM Inter-VM shared memory device register offsets */
	IntrMask        = 0x00,    /* Interrupt Mask */
	IntrStatus      = 0x04,    /* Interrupt Status */
	IVPosition      = 0x08,    /* VM ID */
	Doorbell        = 0x0c,    /* Doorbell */
};

typedef struct kvm_ivshmem_device {
	void __iomem * regs;

	void * base_addr;

	unsigned int regaddr;
	unsigned int reg_size;

	unsigned int ioaddr;
	unsigned int ioaddr_size;
	unsigned int irq;

	struct pci_dev *dev;
	char (*msix_names)[256];
	struct msix_entry *msix_entries;
	int nvectors;

	bool		 enabled;

} kvm_ivshmem_device;

static int event_num;
static struct semaphore sema;
static wait_queue_head_t wait_queue;

static kvm_ivshmem_device kvm_ivshmem_dev;

static long kvm_ivshmem_ioctl(struct file *, unsigned int, unsigned long);
static int kvm_ivshmem_mmap(struct file *, struct vm_area_struct *);
static int kvm_ivshmem_open(struct inode *, struct file *);
static int kvm_ivshmem_release(struct inode *, struct file *);
static ssize_t kvm_ivshmem_read(struct file *, char *, size_t, loff_t *);
static ssize_t kvm_ivshmem_write(struct file *, const char *, size_t, loff_t *);
static loff_t kvm_ivshmem_lseek(struct file * filp, loff_t offset, int origin);

enum ivshmem_ioctl { set_sema, down_sema, empty, wait_event, wait_event_irq, read_ivposn, read_livelist, sema_irq };

static const struct file_operations kvm_ivshmem_ops = {
	.owner   = THIS_MODULE,
	.open	= kvm_ivshmem_open,
	.mmap	= kvm_ivshmem_mmap,
	.read	= kvm_ivshmem_read,
	.unlocked_ioctl = kvm_ivshmem_ioctl,
	.write   = kvm_ivshmem_write,
	.llseek  = kvm_ivshmem_lseek,
	.release = kvm_ivshmem_release,
};

static struct pci_device_id kvm_ivshmem_id_table[] = {
	{ 0x1af4, 0x1110, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0 },
};

static struct miscdevice kvm_ivshmem_misc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ivshmem",
	.fops = &kvm_ivshmem_ops,
};


MODULE_DEVICE_TABLE (pci, kvm_ivshmem_id_table);

static void kvm_ivshmem_remove_device(struct pci_dev* pdev);
static int kvm_ivshmem_probe_device (struct pci_dev *pdev,
						const struct pci_device_id * ent);

static struct pci_driver kvm_ivshmem_pci_driver = {
	.name		= "kvm-shmem",
	.id_table	= kvm_ivshmem_id_table,
	.probe	   = kvm_ivshmem_probe_device,
	.remove	  = kvm_ivshmem_remove_device,
};

static long kvm_ivshmem_ioctl(struct file * filp,
			unsigned int cmd, unsigned long arg)
{

	int rv;
	uint32_t msg;

	KVM_IVSHMEM_DPRINTK("args is 0x%lx", arg);
	switch (cmd) {
		case set_sema:
			KVM_IVSHMEM_DPRINTK("initialize semaphore");
			KVM_IVSHMEM_DPRINTK("args is 0x%lx", arg);
			sema_init(&sema, arg);
			break;
		case down_sema:
			KVM_IVSHMEM_DPRINTK("sleeping on semaphore (cmd = 0x%x)", cmd);
			rv = down_interruptible(&sema);
			KVM_IVSHMEM_DPRINTK("waking");
			break;
		case empty:
			msg = ((arg & 0xff) << 8) + (cmd & 0xff);
			KVM_IVSHMEM_DPRINTK("args is 0x%lx", arg);
			KVM_IVSHMEM_DPRINTK("ringing sema doorbell");
			writel(msg, kvm_ivshmem_dev.regs + Doorbell);
			break;
		case wait_event:
			KVM_IVSHMEM_DPRINTK("sleeping on event (cmd = 0x%08x)", cmd);
			wait_event_interruptible(wait_queue, (event_num == 1));
			KVM_IVSHMEM_DPRINTK("waking");
			event_num = 0;
			break;
		case wait_event_irq:
			msg = ((arg & 0xff) << 8) + (cmd & 0xff);
			KVM_IVSHMEM_DPRINTK("ringing wait_event doorbell on 0x%lx (msg = 0x%x)", arg, msg);
			writel(msg, kvm_ivshmem_dev.regs + Doorbell);
			break;
		case read_ivposn:
			msg = readl( kvm_ivshmem_dev.regs + IVPosition);
			KVM_IVSHMEM_DPRINTK("my posn is 0x%08x", msg);
			rv = copy_to_user((void __user *)arg, &msg, sizeof(msg));
			break;
		case sema_irq:
			// 2 is the actual code, but we use 7 from the user
			msg = ((arg & 0xff) << 8) + (cmd & 0xff);
			KVM_IVSHMEM_DPRINTK("args is 0x%lx", arg);
			KVM_IVSHMEM_DPRINTK("ringing sema doorbell");
			writel(msg, kvm_ivshmem_dev.regs + Doorbell);
			break;
		default:
			KVM_IVSHMEM_DPRINTK("bad ioctl (0x%08x)", cmd);
	}
	
	return 0;
}

static ssize_t kvm_ivshmem_read(struct file * filp, char * buffer, size_t len,
						loff_t * poffset)
{

	int bytes_read = 0;
	unsigned long offset;

	offset = *poffset;

	if (!kvm_ivshmem_dev.base_addr) {
		printk(KERN_ERR "KVM_IVSHMEM: cannot read from ioaddr (NULL)");
		return 0;
	}

	if (len > kvm_ivshmem_dev.ioaddr_size - offset) {
		len = kvm_ivshmem_dev.ioaddr_size - offset;
	}

	if (len == 0) return 0;

	bytes_read = copy_to_user(buffer, kvm_ivshmem_dev.base_addr+offset, len);
	if (bytes_read > 0) {
		return -EFAULT;
	}

	*poffset += len;
	return len;
}

static loff_t kvm_ivshmem_lseek(struct file * filp, loff_t offset, int origin)
{

	loff_t retval = -1;

	switch (origin) {
		case SEEK_CUR:
			offset += filp->f_pos;
			__attribute__((__fallthrough__));
		case SEEK_SET:
			retval = offset;
			if (offset > kvm_ivshmem_dev.ioaddr_size) {
				offset = kvm_ivshmem_dev.ioaddr_size;
			}
			filp->f_pos = offset;
	}

	return retval;
}

static ssize_t kvm_ivshmem_write(struct file * filp, const char * buffer,
					size_t len, loff_t * poffset)
{

	int bytes_written = 0;
	unsigned long offset;

	offset = *poffset;

	printk(KERN_INFO "KVM_IVSHMEM: trying to write");
	if (!kvm_ivshmem_dev.base_addr) {
		printk(KERN_ERR "KVM_IVSHMEM: cannot write to ioaddr (NULL)");
		return 0;
	}

	if (len > kvm_ivshmem_dev.ioaddr_size - offset) {
		len = kvm_ivshmem_dev.ioaddr_size - offset;
	}

	printk(KERN_INFO "KVM_IVSHMEM: len is %u", (unsigned) len);
	if (len == 0) return 0;

	bytes_written = copy_from_user(kvm_ivshmem_dev.base_addr+offset,
					buffer, len);
	if (bytes_written > 0) {
		return -EFAULT;
	}

	printk(KERN_INFO "KVM_IVSHMEM: wrote %u bytes at offset %lu", (unsigned) len, offset);
	*poffset += len;
	return len;
}

static irqreturn_t kvm_ivshmem_interrupt (int irq, void *dev_instance)
{
    struct kvm_ivshmem_device * dev = dev_instance;
    u32 status;

    printk(KERN_INFO "KVM_IVSHMEM: interrupt!");

    if (unlikely(dev == NULL)) {
	KVM_IVSHMEM_DPRINTK("return IRQ_NONE");
	return IRQ_NONE;
    }

    status = readl(dev->regs + IntrStatus);
    KVM_IVSHMEM_DPRINTK("irq ignored: status = 0x%04x", status);
    if (!status || (status == 0xFFFFFFFF))
	return IRQ_NONE;

    /* depending on the message we wake different structures */
    if (status == sema_irq) {
	KVM_IVSHMEM_DPRINTK("status = sema_irq up(&sema)");
	up(&sema);
    } else if (status == wait_event_irq) {
	KVM_IVSHMEM_DPRINTK("status = wait_event_irq wake_up_interruptible(&wait_queue)");
	event_num = 1;
	wake_up_interruptible(&wait_queue);
    }

    printk(KERN_INFO "KVM_IVSHMEM: interrupt (status = 0x%04x)",
	   status);

    return IRQ_HANDLED;
}

static int request_msix_vectors(struct kvm_ivshmem_device *ivs_info, int nvectors)
{
	int i, err;
	const char *name = "ivshmem";

	printk(KERN_INFO "KVM_IVSHMEM: devname is %s", name);
	ivs_info->nvectors = nvectors;

	ivs_info->msix_entries = kmalloc(nvectors * sizeof *ivs_info->msix_entries,
					   GFP_KERNEL);
	ivs_info->msix_names = kmalloc(nvectors * sizeof *ivs_info->msix_names,
					 GFP_KERNEL);

	for (i = 0; i < nvectors; i++)
		ivs_info->msix_entries[i].entry = i;

	for (i = 0; i < nvectors; i++) {
		int n;

		snprintf(ivs_info->msix_names[i], sizeof *ivs_info->msix_names,
		 "%s-config", name);

		n = pci_alloc_irq_vectors(ivs_info->dev, VECTORS_COUNT, VECTORS_COUNT, PCI_IRQ_MSIX);
		if (n < 0) {
			printk(KERN_INFO "KVM_IVSHMEM: pci_alloc_irq_vectors i=%d: error %d", i, n);
			return n;
		}
		printk(KERN_INFO "KVM_IVSHMEM: pci_alloc_irq_vectors(): %d OK", n);

		n = pci_irq_vector(ivs_info->dev, i);
		err = request_irq(n, kvm_ivshmem_interrupt, IRQF_SHARED,
				  ivs_info->msix_names[i], ivs_info);

		if (err) {
			printk(KERN_INFO "KVM_IVSHMEM: couldn't allocate irq for msi-x entry %d with vector %d", i, n);
			return -ENOSPC;
		} else {
			printk(KERN_INFO "KVM_IVSHMEM: allocated irq #%d", n);
		}
	}

	pci_set_master(ivs_info->dev);

	return 0;
}

static int kvm_ivshmem_probe_device (struct pci_dev *pdev,
					const struct pci_device_id * ent) {

	int result;

	KVM_IVSHMEM_DPRINTK("Probing for KVM_IVSHMEM Device");

	result = pci_enable_device(pdev);
	if (result) {
		printk(KERN_ERR "KVM_IVSHMEM: Cannot probe KVM_IVSHMEM device %s: error %d",
		pci_name(pdev), result);
		return result;
	}

	result = pci_request_regions(pdev, "kvm_ivshmem");
	if (result < 0) {
		printk(KERN_ERR "KVM_IVSHMEM: cannot request regions");
		goto pci_disable;
	} else printk(KERN_ERR "KVM_IVSHMEM: result is %d", result);

	kvm_ivshmem_dev.ioaddr = pci_resource_start(pdev, 2);
	kvm_ivshmem_dev.ioaddr_size = pci_resource_len(pdev, 2);

	kvm_ivshmem_dev.base_addr = pci_iomap(pdev, 2, 0);
	printk(KERN_INFO "KVM_IVSHMEM: iomap base = 0x%p",
							kvm_ivshmem_dev.base_addr);

	if (!kvm_ivshmem_dev.base_addr) {
		printk(KERN_ERR "KVM_IVSHMEM: cannot iomap region of size %d",
							kvm_ivshmem_dev.ioaddr_size);
		goto pci_release;
	}

	printk(KERN_INFO "KVM_IVSHMEM: ioaddr = 0x%x ioaddr_size = 0x%x",
						kvm_ivshmem_dev.ioaddr, kvm_ivshmem_dev.ioaddr_size);

	kvm_ivshmem_dev.regaddr =  pci_resource_start(pdev, 0);
	kvm_ivshmem_dev.reg_size = pci_resource_len(pdev, 0);
	kvm_ivshmem_dev.regs = pci_iomap(pdev, 0, 0x100);

	kvm_ivshmem_dev.dev = pdev;

	if (!kvm_ivshmem_dev.regs) {
		printk(KERN_ERR "KVM_IVSHMEM: cannot ioremap registers of size %d",
							kvm_ivshmem_dev.reg_size);
		goto reg_release;
	}

	/* by default initialize semaphore to 0 */
	sema_init(&sema, 0);

	init_waitqueue_head(&wait_queue);
	event_num = 0;

	if (request_msix_vectors(&kvm_ivshmem_dev, VECTORS_COUNT) != 0) {
		printk(KERN_INFO "KVM_IVSHMEM: regular IRQs");
		if (request_irq(pdev->irq, kvm_ivshmem_interrupt, IRQF_SHARED,
							"kvm_ivshmem", &kvm_ivshmem_dev)) {
			printk(KERN_ERR "KVM_IVSHMEM: cannot get interrupt %d", pdev->irq);
			printk(KERN_INFO "KVM_IVSHMEM: irq = %u regaddr = %x reg_size = %d",
					pdev->irq, kvm_ivshmem_dev.regaddr, kvm_ivshmem_dev.reg_size);
		}
	} else {
		printk(KERN_INFO "KVM_IVSHMEM: MSI-X enabled");
	}

	/* set all masks to on */
	writel(0xffffffff, kvm_ivshmem_dev.regs + IntrMask);

	return 0;


reg_release:
	pci_iounmap(pdev, kvm_ivshmem_dev.base_addr);
pci_release:
	pci_release_regions(pdev);
pci_disable:
	pci_disable_device(pdev);
	return -EBUSY;

}

static void kvm_ivshmem_remove_device(struct pci_dev* pdev)
{
	int i, n;

	printk(KERN_INFO "KVM_IVSHMEM: Unregister kvm_ivshmem device.");
	for (i = 0; i < VECTORS_COUNT; i++) {
	    n = pci_irq_vector(pdev, i);
	    KVM_IVSHMEM_DPRINTK("Freeing irq# %d", n);
	    disable_irq(n);
	    free_irq(n, &kvm_ivshmem_dev);
	}
	pci_free_irq_vectors(pdev);
	pci_iounmap(pdev, kvm_ivshmem_dev.regs);
	pci_iounmap(pdev, kvm_ivshmem_dev.base_addr);
	pci_release_regions(pdev);
	pci_disable_device(pdev);

}

static void __exit kvm_ivshmem_cleanup_module (void)
{
	pci_unregister_driver (&kvm_ivshmem_pci_driver);
	misc_deregister(&kvm_ivshmem_misc_dev);
}

static int __init kvm_ivshmem_init_module (void)
{

	int err = -ENOMEM;

	/* Register device node ops. */
	err = misc_register(&kvm_ivshmem_misc_dev);
	if (err < 0) {
		printk(KERN_ERR "KVM_IVSHMEM: Unable to register kvm_ivshmem_misc device");
		return err;
	}
	KVM_IVSHMEM_DPRINTK("Registered the %s device ", kvm_ivshmem_misc_dev.name);
	
	kvm_ivshmem_dev.enabled=FALSE;

	err = pci_register_driver(&kvm_ivshmem_pci_driver);
	if (err < 0) {
		goto error;
	}

	return 0;

error:
//	unregister_chrdev(device_major_nr, "kvm_ivshmem");
	misc_deregister(&kvm_ivshmem_misc_dev);
	return err;
}


static int kvm_ivshmem_open(struct inode * inode, struct file * filp)
{

   printk(KERN_INFO "KVM_IVSHMEM: Opening kvm_ivshmem device");

   if (MINOR(inode->i_rdev) != KVM_IVSHMEM_DEVICE_MINOR_NUM) {
	  printk(KERN_INFO "KVM_IVSHMEM: minor number is %d", KVM_IVSHMEM_DEVICE_MINOR_NUM);
	  return -ENODEV;
   }

   return 0;
}

static int kvm_ivshmem_release(struct inode * inode, struct file * filp)
{

   return 0;
}

static int kvm_ivshmem_mmap(struct file *filp, struct vm_area_struct * vma)
{

	unsigned long len;
	unsigned long off;
	unsigned long start;

//	lock_kernel();

	off = vma->vm_pgoff << PAGE_SHIFT;
	start = kvm_ivshmem_dev.ioaddr;

	len=PAGE_ALIGN((start & ~PAGE_MASK) + kvm_ivshmem_dev.ioaddr_size);
	start &= PAGE_MASK;

	printk(KERN_INFO "KVM_IVSHMEM: %lu - %lu + %lu",vma->vm_end ,vma->vm_start, off);
	printk(KERN_INFO "KVM_IVSHMEM: %lu > %lu",(vma->vm_end - vma->vm_start + off), len);

	if ((vma->vm_end - vma->vm_start + off) > len) {
		// unlock_kernel();
		return -EINVAL;
	}

	off += start;
	vma->vm_pgoff = off >> PAGE_SHIFT;

	vma->vm_flags |= VM_SHARED/*|VM_RESERVED*/;

	if(io_remap_pfn_range(vma, vma->vm_start,
		off >> PAGE_SHIFT, vma->vm_end - vma->vm_start,
		vma->vm_page_prot))
	{
		KVM_IVSHMEM_DPRINTK("mmap failed");
		// unlock_kernel();
		return -ENXIO;
	}
	// unlock_kernel();

	return 0;
}

module_init(kvm_ivshmem_init_module);
module_exit(kvm_ivshmem_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cam Macdonell <cam@cs.ualberta.ca>");
MODULE_DESCRIPTION("KVM inter-VM shared memory module");
MODULE_VERSION("1.0");

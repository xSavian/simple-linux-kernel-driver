#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

int scull_major = 0;
int scull_minor = 0;
int scull_nr_devs = 1;
int scull_quantum = 4000;
int scull_qset = 1000;

struct scull_qset {
	void **data;
	struct scull_qset *next;
};

struct scull_dev {
	struct scull_qset *data;
	int quantum;
	int qset;
	unsigned long size;
	unsigned int access_key;
	struct semaphore sem;
	struct cdev cdev;
};

struct scull_dev *scull_device;

int scull_trim(struct scull_dev *dev)
{
	struct scull_qset *next, *dptr;
	int qset = dev->qset;
	int i;

	for (dptr = dev->data; dptr; dptr = next) {
		if (dptr->data) {
			for (i = 0; i < qset; i++)
				kfree(dptr->data[i]);

			kfree(dptr->data);
			dptr->data = NULL;
		}

		next = dptr->next;
		kfree(dptr);
	}

	dev->size = 0;
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	dev->data = NULL;

	return 0;
}

int scull_open(struct inode *inode, struct file *flip)
{
	struct scull_dev *dev;

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	flip->private_data = dev;

	if ((flip->f_flags & O_ACCMODE) == O_WRONLY) {
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;

		scull_trim(dev);
		up(&dev->sem);
	}

	printk(KERN_INFO "scull: device is opend\n");

	return 0;
}

int scull_release(struct inode *inode, struct file *flip)
{
	return 0;
}

struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
	struct scull_qset *qs = dev->data;

	if (!qs) {
		qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);

		if (qs == NULL)
			return NULL;

		memset(qs, 0, sizeof(struct scull_qset));
	}

	while (n--) {
		if (!qs->next) {
			qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);

			if (qs->next == NULL)
				return NULL;

			memset(qs->next, 0, sizeof(struct scull_qset));
		}

		qs = qs->next;
		continue;
	}

	return qs;
}

ssize_t scull_read(struct file *flip, char __user *buf, size_t count,
				loff_t *f_pos)
{
	struct scull_dev *dev = flip->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t rv = 0;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	if (*f_pos >= dev->size) {
		printk(KERN_INFO "scull: *f_pos more than size %lu\n", dev->size);
		goto out;
	}

	if (*f_pos + count > dev->size) {
		printk(KERN_INFO "scull: correct count\n");
		count = dev->size - *f_pos;
	}

	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;

	s_pos = rest / quantum;
	q_pos = rest % quantum;

	dptr = scull_follow(dev, item);

	if (dptr == NULL || !dptr->data || !dptr->data[s_pos])
		goto out;

	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
		rv = -EFAULT;
		goto out;
	}


	*f_pos += count;
	rv = count;

out:
	up(&dev->sem);
	return rv;
}

ssize_t scull_write(struct file *flip, const char __user *buf, size_t count,
					loff_t *f_pos)
{
	struct scull_dev *dev = flip->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t rv = -ENOMEM;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;

	s_pos = rest / quantum;
	q_pos = rest % quantum;

	dptr = scull_follow(dev, item);

	if (dptr == NULL)
		goto out;

	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);

		if (!dptr->data)
			goto out;

		memset(dptr->data, 0, qset * sizeof(char *));
	}

	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);

		if (!dptr->data[s_pos])
			goto out;
	}


	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		rv = -EFAULT;
		goto out;
	}

	*f_pos += count;
	rv = count;

	if (dev->size < *f_pos)
		dev->size = *f_pos;

out:
	up(&dev->sem);
	return rv;
}

struct file_operations scull_fops = {
	.owner = THIS_MODULE,
	.read = scull_read,
	.write = scull_write,
	.open = scull_open,
	.release = scull_release,
};

static void scull_setup_cdev(struct scull_dev *dev, int index)
{
	int err, devno = MKDEV(scull_major, scull_minor + index);

	cdev_init(&dev->cdev, &scull_fops);

	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;

	err = cdev_add(&dev->cdev, devno, 1);

	if (err)
		printk(KERN_NOTICE "Error %d adding scull  %d", err, index);
}

void scull_cleanup_module(void)
{
	int i;
	dev_t devno = MKDEV(scull_major, scull_minor);

	if (scull_device) {
		for (i = 0; i < scull_nr_devs; i++) {
			scull_trim(scull_device + i);
			cdev_del(&scull_device[i].cdev);
		}

		kfree(scull_device);
	}

	unregister_chrdev_region(devno, scull_nr_devs);
}

static int scull_init_module(void)
{
	int rv, i;
	dev_t dev;


	rv = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, "scull");


	if (rv) {
		printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
		return rv;
	}

	scull_major = MAJOR(dev);

	scull_device = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);

	if (!scull_device) {
		rv = -ENOMEM;
		goto fail;
	}

	memset(scull_device, 0, scull_nr_devs * sizeof(struct scull_dev));

	for (i = 0; i < scull_nr_devs; i++) {
		scull_device[i].quantum = scull_quantum;
		scull_device[i].qset = scull_qset;
		sema_init(&scull_device[i].sem, 1);
		scull_setup_cdev(&scull_device[i], i);
	}

	dev = MKDEV(scull_major, scull_minor + scull_nr_devs);

	printk(KERN_INFO "scull: major = %d minor = %d\n", scull_major, scull_minor);

	return 0;

fail:
	scull_cleanup_module();
	return rv;
}

MODULE_AUTHOR("AUTHOR");
MODULE_LICENSE("GPL");

module_init(scull_init_module);
module_exit(scull_cleanup_module);
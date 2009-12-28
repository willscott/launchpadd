/*
 * USB Launchpad Driver
 *
 * By Will Scott (willscott@gmail.com)
 * Credits to Greg Kroah-Hartman (greg@kroah.com)
 *
 * Based on the usb-skeleton.c
 * Namely the version found at:
 * http://lxr.free-electrons.com/source/drivers/usb/usb-skeleton.c?v=2.6.33-rc1
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/poll.h>
#include <linux/mutex.h>


/* Define these values to match your devices */
#define USB_LP_VENDOR_ID      0x1235
#define USB_LP_PRODUCT_ID     0x000e

/* table of devices that work with this driver */
static struct usb_device_id lp_table[] = {
        { USB_DEVICE(USB_LP_VENDOR_ID, USB_LP_PRODUCT_ID) },
        { }                                     /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, lp_table);


/* Get a minor range for your devices from the usb maintainer */
#define USB_LP_MINOR_BASE     192

/* our private defines. if this grows any larger, use your own .h file */
#define MAX_TRANSFER            (PAGE_SIZE - 512)
/* MAX_TRANSFER is chosen so that the VM is not stressed by
   allocations > PAGE_SIZE and the number of packets in a page
   is an integer 512 is the largest possible packet on EHCI */
#define WRITES_IN_FLIGHT        8
/* arbitrarily chosen */

/* Structure to hold all of our device specific stuff */
struct usb_launchpad {
        struct usb_device       *udev;                  /* the usb device for this device */
        struct usb_interface    *interface;             /* the interface for this device */
        struct semaphore        limit_sem;              /* limiting the number of writes in progress */
        struct usb_anchor       submitted;              /* in case we need to retract our submissions */
        struct urb              *int_in_urb;            /* the urb to read data with */
        unsigned char           *int_in_buffer;         /* the buffer to receive data */
		wait_queue_head_t 		inq;
        size_t                  int_in_size;            /* the size of the receive buffer */
        size_t                  int_in_filled;          /* number of bytes in the buffer */
        size_t                  int_in_copied;          /* already copied to user space */
        __u8                    int_in_endpointAddr;    /* the address of the interrupt in endpoint */
        __u8                    int_out_endpointAddr;   /* the address of the interrupt out endpoint */
        int                     errors;                 /* the last request tanked */
        int                     open_count;             /* count the number of openers */
        bool                    ongoing_read;           /* a read is going on */
        bool                    processed_urb;          /* indicates we haven't processed the urb */
		bool					submitted_urb;			/* indicates we haven't submitted the read urb */
        spinlock_t              err_lock;               /* lock for errors */
        struct kref             kref;
        struct mutex            io_mutex;               /* synchronize I/O with disconnect */
};
#define to_lp_dev(d) container_of(d, struct usb_launchpad, kref)

static struct usb_driver launchpad_driver;
static void lp_draw_down(struct usb_launchpad *dev);

static void lp_delete(struct kref *kref)
{
        struct usb_launchpad *dev = to_lp_dev(kref);

        usb_free_urb(dev->int_in_urb);
        usb_put_dev(dev->udev);
        kfree(dev->int_in_buffer);
        kfree(dev);
}

static int lp_open(struct inode *inode, struct file *file)
{
        struct usb_launchpad *dev;
        struct usb_interface *interface;
        int subminor;
        int retval = 0;

        subminor = iminor(inode);

        interface = usb_find_interface(&launchpad_driver, subminor);
        if (!interface) {
                err("%s - error, can't find device for minor %d",
                     __func__, subminor);
                retval = -ENODEV;
                goto exit;
        }

        dev = usb_get_intfdata(interface);
        if (!dev) {
                retval = -ENODEV;
                goto exit;
        }

        /* increment our usage count for the device */
        kref_get(&dev->kref);

        /* lock the device to allow correctly handling errors
         * in resumption */
        mutex_lock(&dev->io_mutex);

        if (!dev->open_count++) {
                retval = usb_autopm_get_interface(interface);
                        if (retval) {
                                dev->open_count--;
                                mutex_unlock(&dev->io_mutex);
                                kref_put(&dev->kref, lp_delete);
                                goto exit;
                        }
        }
        /* prevent the device from being autosuspended */

        /* save our object in the file's private structure */
        file->private_data = dev;
        mutex_unlock(&dev->io_mutex);

exit:
        return retval;
}

static int lp_release(struct inode *inode, struct file *file)
{
        struct usb_launchpad *dev;

        dev = (struct usb_launchpad *)file->private_data;
        if (dev == NULL)
                return -ENODEV;

        /* allow the device to be autosuspended */
        mutex_lock(&dev->io_mutex);
        if (!--dev->open_count && dev->interface)
                usb_autopm_put_interface(dev->interface);
        mutex_unlock(&dev->io_mutex);

        /* decrement the count on our device */
        kref_put(&dev->kref, lp_delete);
        return 0;
}

static int lp_flush(struct file *file, fl_owner_t id)
{
        struct usb_launchpad *dev;
        int res;

        dev = (struct usb_launchpad *)file->private_data;
        if (dev == NULL)
                return -ENODEV;

        /* wait for io to stop */
        mutex_lock(&dev->io_mutex);
        lp_draw_down(dev);

        /* read out errors, leave subsequent opens a clean slate */
        spin_lock_irq(&dev->err_lock);
        res = dev->errors ? (dev->errors == -EPIPE ? -EPIPE : -EIO) : 0;
        dev->errors = 0;
        spin_unlock_irq(&dev->err_lock);

        mutex_unlock(&dev->io_mutex);

        return res;
}

static void lp_read_int_callback(struct urb *urb)
{
        struct usb_launchpad *dev;

        dev = urb->context;

        spin_lock(&dev->err_lock);
        /* sync/async unlink faults aren't errors */
        if (urb->status) {
                if (!(urb->status == -ENOENT ||
                    urb->status == -ECONNRESET ||
                    urb->status == -ESHUTDOWN))
                        err("%s - nonzero write bulk status received: %d",
                            __func__, urb->status);

                dev->errors = urb->status;
        } else {
                dev->int_in_filled = urb->actual_length;
				dev->int_in_copied = 0;
        }
        dev->ongoing_read = 0;
		dev->submitted_urb = 0;
        spin_unlock(&dev->err_lock);

		wake_up_interruptible_all(&dev->inq);
}

static int lp_do_read_io(struct usb_launchpad *dev, size_t count)
{
        int rv;

        /* prepare a read */
        usb_fill_int_urb(dev->int_in_urb,
                        dev->udev,
                        usb_rcvbulkpipe(dev->udev,
                                dev->int_in_endpointAddr),
                        dev->int_in_buffer,
                        dev->int_in_size,
                        lp_read_int_callback,
                        dev, 10);
        /* tell everybody to leave the URB alone */
        spin_lock_irq(&dev->err_lock);
        dev->ongoing_read = 1;
        spin_unlock_irq(&dev->err_lock);

        /* do it */
        rv = usb_submit_urb(dev->int_in_urb, GFP_KERNEL);
        if (rv < 0) {
                err("%s - failed submitting read urb, error %d",
                        __func__, rv);
                dev->int_in_filled = 0;
                rv = (rv == -ENOMEM) ? rv : -EIO;
                spin_lock_irq(&dev->err_lock);
                dev->ongoing_read = 0;
                spin_unlock_irq(&dev->err_lock);
        }
		dev->submitted_urb = 1;

        return rv;
}

static ssize_t lp_read(struct file *file, char *buffer, size_t count,
                         loff_t *ppos)
{
        struct usb_launchpad *dev;
        int rv;
        bool ongoing_io;

        dev = (struct usb_launchpad *)file->private_data;

        /* if we cannot read at all, return EOF */
        if (!dev->int_in_urb || !count)
                return 0;

        /* no concurrent readers */
        rv = mutex_lock_interruptible(&dev->io_mutex);
        if (rv < 0)
                return rv;

        if (!dev->interface) {          /* disconnect() was called */
                rv = -ENODEV;
                goto exit;
        }

        /* if IO is under way, we must not touch things */
retry:
        spin_lock_irq(&dev->err_lock);
        ongoing_io = dev->ongoing_read;
        spin_unlock_irq(&dev->err_lock);

        if (ongoing_io || (!dev->processed_urb && dev->submitted_urb)) {
                /* nonblocking IO shall not wait */
                if (file->f_flags & O_NONBLOCK) {
                        rv = -EAGAIN;
                        goto exit;
                }
                /*
                 * IO may take forever
                 * hence wait in an interruptible state
                 */
                rv = wait_event_interruptible(dev->inq,(dev->int_in_filled));
                if (rv < 0)
                        goto exit;
                /*
                 * by waiting we also semiprocessed the urb
                 * we must finish now
                 */
                dev->processed_urb = 1;
        }

        /* errors must be reported */
        rv = dev->errors;
        if (rv < 0) {
                /* any error is reported once */
                dev->errors = 0;
                /* to preserve notifications about reset */
                rv = (rv == -EPIPE) ? rv : -EIO;
                /* no data to deliver */
                dev->int_in_filled = 0;
                /* report it */
                goto exit;
        }

        /*
         * if the buffer is filled we may satisfy the read
         * else we need to start IO
         */

        if (dev->int_in_filled > dev->int_in_copied) {
                /* we had read data */
                size_t available = dev->int_in_filled - dev->int_in_copied;
                size_t chunk = min(available, count);

                if (!available) {
                        /*
                         * all data has been used
                         * actual IO needs to be done
                         */
                        rv = lp_do_read_io(dev, count);
                        if (rv < 0)
                                goto exit;
                        else
                                goto retry;
                }
                /*
                 * data is available
                 * chunk tells us how much shall be copied
                 */

                if (copy_to_user(buffer,
                                 dev->int_in_buffer + dev->int_in_copied,
                                 chunk))
                        rv = -EFAULT;
                else
                        rv = chunk;

                dev->int_in_copied += chunk;

                /*
                 * if we are asked for more than we have,
                 * we start IO but don't wait
                 */
                if (available < count)
                        lp_do_read_io(dev, count - chunk);
        } else {
                /* no data in the buffer */
                rv = lp_do_read_io(dev, count);
                if (rv < 0)
                        goto exit;
                else if (!(file->f_flags & O_NONBLOCK))
                        goto retry;
                rv = -EAGAIN;
        }
exit:
        mutex_unlock(&dev->io_mutex);
        return rv;
}

static void lp_write_int_callback(struct urb *urb)
{
        struct usb_launchpad *dev;

        dev = urb->context;

        /* sync/async unlink faults aren't errors */
        if (urb->status) {
                if (!(urb->status == -ENOENT ||
                    urb->status == -ECONNRESET ||
                    urb->status == -ESHUTDOWN))
                        err("%s - nonzero write bulk status received: %d",
                            __func__, urb->status);

                spin_lock(&dev->err_lock);
                dev->errors = urb->status;
                spin_unlock(&dev->err_lock);
        }

        /* free up our allocated buffer */
        usb_buffer_free(urb->dev, urb->transfer_buffer_length,
                        urb->transfer_buffer, urb->transfer_dma);
        up(&dev->limit_sem);
}

static ssize_t lp_write(struct file *file, const char *user_buffer,
                          size_t count, loff_t *ppos)
{
        struct usb_launchpad *dev;
        int retval = 0;
        struct urb *urb = NULL;
        char *buf = NULL;
        size_t writesize = min(count, (size_t)MAX_TRANSFER);

        dev = (struct usb_launchpad *)file->private_data;

        /* verify that we actually have some data to write */
        if (count == 0)
                goto exit;

        /*
         * limit the number of URBs in flight to stop a user from using up all
         * RAM
         */
        if (!(file->f_flags & O_NONBLOCK)) {
                if (down_interruptible(&dev->limit_sem)) {
                        retval = -ERESTARTSYS;
                        goto exit;
                }
        } else {
                if (down_trylock(&dev->limit_sem)) {
                        retval = -EAGAIN;
                        goto exit;
                }
        }

        spin_lock_irq(&dev->err_lock);
        retval = dev->errors;
        if (retval < 0) {
                /* any error is reported once */
                dev->errors = 0;
                /* to preserve notifications about reset */
                retval = (retval == -EPIPE) ? retval : -EIO;
        }
        spin_unlock_irq(&dev->err_lock);
        if (retval < 0)
                goto error;

        /* create a urb, and a buffer for it, and copy the data to the urb */
        urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!urb) {
                retval = -ENOMEM;
                goto error;
        }

        buf = usb_buffer_alloc(dev->udev, writesize, GFP_KERNEL,
                               &urb->transfer_dma);
        if (!buf) {
                retval = -ENOMEM;
                goto error;
        }

        if (copy_from_user(buf, user_buffer, writesize)) {
                retval = -EFAULT;
                goto error;
        }

        /* this lock makes sure we don't submit URBs to gone devices */
        mutex_lock(&dev->io_mutex);
        if (!dev->interface) {          /* disconnect() was called */
                mutex_unlock(&dev->io_mutex);
                retval = -ENODEV;
                goto error;
        }

        /* initialize the urb properly */
        usb_fill_int_urb(urb, dev->udev,
                          usb_sndbulkpipe(dev->udev, dev->int_out_endpointAddr),
                          buf, writesize, lp_write_int_callback, dev, 10);
        urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
        usb_anchor_urb(urb, &dev->submitted);

        /* send the data out the bulk port */
        retval = usb_submit_urb(urb, GFP_KERNEL);
        mutex_unlock(&dev->io_mutex);
        if (retval) {
                err("%s - failed submitting write urb, error %d", __func__,
                    retval);
                goto error_unanchor;
        }

        /*
         * release our reference to this urb, the USB core will eventually free
         * it entirely
         */
        usb_free_urb(urb);


        return writesize;

error_unanchor:
        usb_unanchor_urb(urb);
error:
        if (urb) {
                usb_buffer_free(dev->udev, writesize, buf, urb->transfer_dma);
                usb_free_urb(urb);
        }
        up(&dev->limit_sem);

exit:
        return retval;
}

static unsigned int lp_poll(struct file *filp, poll_table *wait)
{
	unsigned int mask = 0;
	struct usb_launchpad *dev;
	dev = (struct usb_launchpad *)filp->private_data;

	//Exclusivity on.
	mutex_lock(&dev->io_mutex);
	
	if(dev->submitted_urb ==0 && !(dev->int_in_filled > dev->int_in_copied))
	{
		//need to start a request
		lp_do_read_io(dev, 3);
	}
	if(dev->submitted_urb ==1 && !(dev->int_in_filled > dev->int_in_copied))
	{
		//need to finish a request
		poll_wait(filp, &dev->inq, wait);
	}
	if (dev->int_in_filled > dev->int_in_copied)
	{
		//is there data?
		mask |= POLLIN | POLLRDNORM;
	}
	
	//Always writable :D
	mask |= POLLOUT | POLLWRNORM;

	mutex_unlock(&dev->io_mutex);
	return mask;
}

static const struct file_operations launchpad_fops = {
        .owner =        THIS_MODULE,
        .read =         lp_read,
        .write =        lp_write,
        .open =         lp_open,
        .release =      lp_release,
        .flush =        lp_flush,
		.poll =			lp_poll,
};

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver launchpad_class = {
        .name =         "launchpad%d",
        .fops =         &launchpad_fops,
        .minor_base =   USB_LP_MINOR_BASE,
};

static int lp_probe(struct usb_interface *interface,
                      const struct usb_device_id *id)
{
        struct usb_launchpad *dev;
        struct usb_host_interface *iface_desc;
        struct usb_endpoint_descriptor *endpoint;
        size_t buffer_size;
        int i;
        int retval = -ENOMEM;

        /* allocate memory for our device state and initialize it */
        dev = kzalloc(sizeof(*dev), GFP_KERNEL);
        if (!dev) {
                err("Out of memory");
                goto error;
        }
        kref_init(&dev->kref);
        sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
        mutex_init(&dev->io_mutex);
        spin_lock_init(&dev->err_lock);
        init_usb_anchor(&dev->submitted);
		init_waitqueue_head(&dev->inq);

        dev->udev = usb_get_dev(interface_to_usbdev(interface));
        dev->interface = interface;

        /* set up the endpoint information */
        /* use only the first bulk-in and bulk-out endpoints */
        iface_desc = interface->cur_altsetting;
        for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
                endpoint = &iface_desc->endpoint[i].desc;

                if (!dev->int_in_endpointAddr &&
                    usb_endpoint_is_int_in(endpoint)) {
                        /* we found a bulk in endpoint */
                        buffer_size = le16_to_cpu(endpoint->wMaxPacketSize);
                        dev->int_in_size = buffer_size;
                        dev->int_in_endpointAddr = endpoint->bEndpointAddress;
                        dev->int_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
                        if (!dev->int_in_buffer) {
                                err("Could not allocate in_buffer");
                                goto error;
                        }
                        dev->int_in_urb = usb_alloc_urb(0, GFP_KERNEL);
                        if (!dev->int_in_urb) {
                                err("Could not allocate in_urb");
                                goto error;
                        }
                }

                if (!dev->int_out_endpointAddr &&
                    usb_endpoint_is_int_out(endpoint)) {
                        /* we found a bulk out endpoint */
                        dev->int_out_endpointAddr = endpoint->bEndpointAddress;
                }
        }
        if (!(dev->int_in_endpointAddr && dev->int_out_endpointAddr)) {
                err("Could not find both bulk-in and bulk-out endpoints");
                goto error;
        }

        /* save our data pointer in this interface device */
        usb_set_intfdata(interface, dev);

        /* we can register the device now, as it is ready */
        retval = usb_register_dev(interface, &launchpad_class);
        if (retval) {
                /* something prevented us from registering this driver */
                err("Not able to get a minor for this device.");
                usb_set_intfdata(interface, NULL);
                goto error;
        }

        /* let the user know what node this device is now attached to */
        dev_info(&interface->dev,
                 "USB Launchpad device now attached to launchpad-%d",
                 interface->minor);
        return 0;

error:
        if (dev)
                /* this frees allocated memory */
                kref_put(&dev->kref, lp_delete);
        return retval;
}

static void lp_disconnect(struct usb_interface *interface)
{
        struct usb_launchpad *dev;
        int minor = interface->minor;

        dev = usb_get_intfdata(interface);
        usb_set_intfdata(interface, NULL);

        /* give back our minor */
        usb_deregister_dev(interface, &launchpad_class);

        /* prevent more I/O from starting */
        mutex_lock(&dev->io_mutex);
        dev->interface = NULL;
        mutex_unlock(&dev->io_mutex);

        usb_kill_anchored_urbs(&dev->submitted);

        /* decrement our usage count */
        kref_put(&dev->kref, lp_delete);

        dev_info(&interface->dev, "USB Launchpad #%d now disconnected", minor);
}

static void lp_draw_down(struct usb_launchpad *dev)
{
        int time;

        time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
        if (!time)
                usb_kill_anchored_urbs(&dev->submitted);
        usb_kill_urb(dev->int_in_urb);
}

static int lp_suspend(struct usb_interface *intf, pm_message_t message)
{
        struct usb_launchpad *dev = usb_get_intfdata(intf);

        if (!dev)
                return 0;
        lp_draw_down(dev);
        return 0;
}

static int lp_resume(struct usb_interface *intf)
{
        return 0;
}

static int lp_pre_reset(struct usb_interface *intf)
{
        struct usb_launchpad *dev = usb_get_intfdata(intf);

        mutex_lock(&dev->io_mutex);
        lp_draw_down(dev);

        return 0;
}

static int lp_post_reset(struct usb_interface *intf)
{
        struct usb_launchpad *dev = usb_get_intfdata(intf);

        /* we are sure no URBs are active - no locking needed */
        dev->errors = -EPIPE;
        mutex_unlock(&dev->io_mutex);

        return 0;
}

static struct usb_driver launchpad_driver = {
        .name =         "launchpad",
        .probe =        lp_probe,
        .disconnect =   lp_disconnect,
        .suspend =      lp_suspend,
        .resume =       lp_resume,
        .pre_reset =    lp_pre_reset,
        .post_reset =   lp_post_reset,
        .id_table =     lp_table,
        .supports_autosuspend = 1,
};

static int __init usb_lp_init(void)
{
        int result;

        /* register this driver with the USB subsystem */
        result = usb_register(&launchpad_driver);
        if (result)
                err("usb_register failed. Error number %d", result);

        return result;
}

static void __exit usb_lp_exit(void)
{
        /* deregister this driver with the USB subsystem */
        usb_deregister(&launchpad_driver);
}

module_init(usb_lp_init);
module_exit(usb_lp_exit);

MODULE_LICENSE("GPL");

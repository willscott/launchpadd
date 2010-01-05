#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <libusb-1.0/libusb.h>

#define USB_LP_VENDOR_ID      0x1235				/* Grabbed from lsusb -v */
#define USB_LP_PRODUCT_ID     0x000e				/* Grabbed from lsusb -v */
#define USB_LP_INTR_OUT ( 2 | LIBUSB_ENDPOINT_OUT)	/* wtf, that should be a 1 for interrupt, not a 2 for bulk */
#define USB_LP_INTR_IN  ( 1 | LIBUSB_ENDPOINT_IN )	/* but this one is as expected */
#define LP_POLL_INTERVAL	10		/* It advertises this, and it seems to work okay */

/*
 * This is the method signature for your call-back
 * you'll get a call with 0-length each time a write completes
 * and a call with non-zero lenth each time a launchpad button is pushed.
 *
 * Data is essentially midi format already, and is as-explained in the programmers
 * guide.
 */
typedef void(*launchpad_callback)(unsigned char* data, size_t len, void* user_data);

/*
 * This is the data that's held in the pointer we return to you.
 * If you're really fancy, you can dynamically change your callback
 * function or user data based on the mode you're in.  
 * The other things are our internal state, and you'll probably break
 * things if you mess with them.
 */
struct launchpad_handle {
    launchpad_callback callback;
    struct libusb_device_handle* device;
    struct libusb_transfer* rtransfer;
    struct libusb_transfer* wtransfer;
	void* user_data;
    int writing;
};

/*
 * Start a new session
 * It will return null if it fails, and a pointer to it's handle on
 * success.  Both options are things you control, the second one can
 * be null if you don't need to pass anything to your callback function
 */
struct launchpad_handle* launchpad_register(launchpad_callback e, void* user_data);

/*
 * Call this when you're done with the device.
 * This closes transfers and cleans up the memory that was allocated.
 * Make sure you're not polling at the same time (ie calling this in
 * an interrupt) or you'll get occasional deadlocks.
 */
void launchpad_deregister(struct launchpad_handle* dp);

/*
 * Write data to the launchpad.
 * You pass the handle that you got from open, and the data you want to send
 * The actual data is described in the launchpad programming manual, and
 * is as-advertised.
 */
int launchpad_write(struct launchpad_handle *dp, unsigned char* data, size_t len);

/*
 * Process any new events from the device.  This should be used in your
 * main event loop.  You can pass it whatever file-descriptors your
 * program needs to wait on.  (You pass these as an array, and set
 * num to be the length of that array.)  The function returns a
 * positive value if any of your descriptors had events, as well
 * as properly setting the revents field of the descriptors.
 * It returns 0 if it only processed launchpad events, and
 * none of your file-descriptors fired.  However, this is an indication that
 * either a read or write call-back has occured, and your program may
 * still need to do additional work at this point, so control is returned.
 */
int launchpad_poll(struct pollfd* descriptors, size_t num);
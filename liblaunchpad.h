#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <libusb-1.0/libusb.h>

#define USB_LP_VENDOR_ID      0x1235
#define USB_LP_PRODUCT_ID     0x000e
#define USB_LP_INTR_OUT ( 2 | LIBUSB_ENDPOINT_OUT)
#define USB_LP_INTR_IN  ( 1 | LIBUSB_ENDPOINT_IN )

typedef void(*launchpad_callback)(unsigned char* data, size_t len, void* user_data);
struct launchpad_handle {
    launchpad_callback callback;
    struct libusb_device_handle* device;
    struct libusb_transfer* rtransfer;
    struct libusb_transfer* wtransfer;
	void* user_data;
    int writing;
};

struct launchpad_handle* launchpad_register(launchpad_callback e, void* user_data);
void launchpad_deregister(struct launchpad_handle* dp);
int launchpad_write(struct launchpad_handle *dp, unsigned char* data, size_t len);
int launchpad_poll(struct pollfd* descriptors, size_t num);
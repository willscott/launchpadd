#include "liblaunchpad.h"

void launchpad_read_callback(struct libusb_transfer *transfer)
{
    struct launchpad_handle *dp = transfer->user_data;
    //figure out if we need to read something
    if (transfer->actual_length > 0)
    {
        dp->callback(transfer->buffer, transfer->actual_length, dp->user_data);
    }
    else if (transfer->status != LIBUSB_TRANSFER_TIMED_OUT &&
             transfer->status != LIBUSB_TRANSFER_COMPLETED)
    {
        dp->errback(dp->user_data);
        return;
    }
    //resend it
    libusb_fill_interrupt_transfer(transfer,
                                   dp->device,
                                   USB_LP_INTR_IN,
                                   transfer->buffer,
                                   transfer->length,
                                   launchpad_read_callback,
                                   dp,
                                   LP_POLL_INTERVAL);
    if (libusb_submit_transfer(transfer) != 0)
    {
        fprintf(stderr, "Couldn't send transfer\n");
        return;
    }
}

void launchpad_write_callback(struct libusb_transfer *transfer)
{
    struct launchpad_handle *dp = transfer->user_data;
    if (transfer->status != LIBUSB_TRANSFER_TIMED_OUT &&
        transfer->status != LIBUSB_TRANSFER_COMPLETED)
    {
        fprintf(stderr, "Writing failed with code %d\n", transfer->status);
        fprintf(stderr, "and that is not %d or %d...\n", LIBUSB_TRANSFER_TIMED_OUT,LIBUSB_TRANSFER_COMPLETED);
    }
    dp->writing = 0;
    dp->callback(NULL, 0, dp->user_data);
}

void launchpad_nullerrback(void* data)
{
  fprintf(stderr, "Unhandled read error\n");
}

struct launchpad_handle* launchpad_register(launchpad_callback e, void* user_data)
{
    struct launchpad_handle *dp;
    unsigned int bufferlen;
    unsigned char* buffer;
    int retval;

    //initialize usb
    if (libusb_init(NULL) != 0)
    {
        fprintf(stderr, "Unable to initialize usb\n");
        return NULL;
    }

    //allocate the handle
    dp = malloc(sizeof(struct launchpad_handle));
    if (dp == NULL)
    {
        fprintf(stderr, "Unable to allocate handle\n");
        return NULL;
    }
    dp->callback = e;
    dp->errback = launchpad_nullerrback;
    dp->user_data = user_data;

    //find the device
    dp->device = libusb_open_device_with_vid_pid(NULL, USB_LP_VENDOR_ID, USB_LP_PRODUCT_ID);
    if (dp->device == NULL) {
        fprintf(stderr, "Unable to find the launchpad\n");
        return NULL;
    }
    //claim the device
    retval = libusb_claim_interface(dp->device, 0);
    if (retval == LIBUSB_ERROR_BUSY)
    {
        retval = libusb_kernel_driver_active(dp->device, 0);
        if (retval == 1 &&
            libusb_detach_kernel_driver(dp->device, 0) == 0 &&
            libusb_claim_interface(dp->device, 0) == 0)
        {
            fprintf(stderr, "Launchpad acquired from a kernel driver.\n");
        }
        else
        {
            fprintf(stderr, "Unable to claim the launchpad from an existing driver\n");
            return NULL;
        }
    }
    else if (retval != 0)
    {
        fprintf(stderr, "Unable to claim the launchpad (errno %d) \n", retval);
        return NULL;
    }

    //create transfers
    bufferlen = libusb_get_max_packet_size(libusb_get_device(dp->device), USB_LP_INTR_OUT);
    dp->rtransfer = libusb_alloc_transfer(0);
    dp->wtransfer = libusb_alloc_transfer(0);
    buffer = malloc(sizeof(char) * bufferlen);
    if (dp->rtransfer == NULL || dp->wtransfer == NULL || buffer == NULL)
    {
        if (bufferlen == LIBUSB_ERROR_NOT_FOUND)
        {
            fprintf(stderr, "Couldn't Determine Packet Size\n");
        }
        fprintf(stderr, "Unable to allocate transfers\n");
        return NULL;
    }
    //start transfers
    dp->writing = 0;
    libusb_fill_interrupt_transfer(dp->rtransfer,
                                   dp->device,
                                   USB_LP_INTR_IN,
                                   buffer,
                                   bufferlen,
                                   launchpad_read_callback,
                                   dp,
                                   LP_POLL_INTERVAL);
    if (libusb_submit_transfer(dp->rtransfer) != 0)
    {
        fprintf(stderr, "Unable to start reading\n");
        return NULL;
    }

    return dp;
}

void launchpad_seterrback(struct launchpad_handle* dp, launchpad_errback e)
{
  dp->errback = e;
}

void launchpad_deregister(struct launchpad_handle* dp)
{
    //cancel transfers
    if(dp->writing)
    {
        libusb_cancel_transfer(dp->wtransfer);
    }
    libusb_cancel_transfer(dp->rtransfer);
    //destroy transfers
    libusb_free_transfer(dp->rtransfer);
    libusb_free_transfer(dp->wtransfer);
    //declaim the device
    libusb_release_interface(dp->device,0);
    //close the device
    libusb_close(dp->device);
    //deallocate the handle
    free(dp);
    //close usb
    libusb_exit(NULL);
}

int launchpad_write(struct launchpad_handle *dp, unsigned char* data, size_t len)
{
    int retval;

    if(dp->writing)
        return 1;
    dp->writing = 1;

    libusb_fill_interrupt_transfer(dp->wtransfer,
                                    dp->device,
                                    USB_LP_INTR_OUT,
                                    data,
                                    len,
                                    launchpad_write_callback,
                                    dp,
                                    10);
     retval = libusb_submit_transfer(dp->wtransfer);
     if(retval != 0)
     {
         printf("Device Write failed. (Error %d)\n",retval);
         dp->writing = 0;
     }
     return retval;
}

int launchpad_poll(struct pollfd* descriptors, size_t num)
{
    //lock events
    libusb_lock_events(NULL);

    struct pollfd* final;
    struct timeval tv;
    int i = 0,usbevent = 0,yourevent = 0;
    nfds_t total = 0;
    const struct libusb_pollfd** lpfds = libusb_get_pollfds(NULL);

    while(lpfds[i] != NULL) { i++; }
    total = num+i;
    final = malloc(sizeof(struct pollfd) * (i + num));
    for(i = 0; i < num; i++)
    {
        final[i].fd = descriptors[i].fd;
        final[i].events = descriptors[i].events;
    }
    for(i = 0; i+num < total; i++)
    {
        final[i+num].fd = lpfds[i]->fd;
        final[i+num].events = lpfds[i]->events;
    }
    free(lpfds);
//    tv.tv_sec = 0;
//    tv.tv_usec = 0;
//    libusb_get_next_timeout(NULL, &tv);
//    poll(final, total, tv.tv_usec + 1000 * tv.tv_sec);
    poll(final,total,120*1000);

    for(i = 0; i < num; i++) {
        descriptors[i].revents = final[i].revents;
        if(final[i].revents != 0)
            yourevent = 1;
    }
    for(i = 0; i+num < total; i++)
    {
        if(final[num+i].revents != 0) {
            usbevent = 1;
            break;
        }
    }
    if(usbevent) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if(libusb_handle_events_locked(NULL, &tv))
        {
            printf("Error handling events.\n");
        }
    }
    //unlock events
    libusb_unlock_events(NULL);

    free(final);
    final = NULL;
    return yourevent;
}

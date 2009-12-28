#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#define MAX_QUEUE 256

snd_seq_t *open_color_seq();
snd_seq_t *open_touch_seq();
void midi_receive(snd_seq_t *seq_handle, int fd);
struct point {
	int x;
	int y;
	int down;
	int control;
};
int live_push = 0;
char outbuf[MAX_QUEUE*3];
int numout = 0, posout = 0;

struct point getKey(int launchpad);
void setKey(int launchpad, int key, char c);

void midi_send(snd_seq_t *seq_handle, struct point *event);

snd_seq_t *open_color_seq() {

  snd_seq_t *seq_handle;
  int portid;

  if (snd_seq_open(&seq_handle, "hw", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
    fprintf(stderr, "Error opening ALSA sequencer.\n");
    exit(1);
  }
  snd_seq_set_client_name(seq_handle, "Novation Launchpad");
  if ((portid = snd_seq_create_simple_port(seq_handle, "Novation Launchpad",
            SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_PORT)) < 0) {
    fprintf(stderr, "Error creating sequencer port.\n");
    exit(1);
  }
  return(seq_handle);
}
snd_seq_t *open_touch_seq() {

  snd_seq_t *seq_handle;
  int portid;

  if (snd_seq_open(&seq_handle, "hw", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
    fprintf(stderr, "Error opening ALSA sequencer.\n");
    exit(1);
  }
  snd_seq_set_client_name(seq_handle, "Novation Launchpad");
  if ((portid = snd_seq_create_simple_port(seq_handle, "Novation Launchpad",
            SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_PORT)) < 0) {
    fprintf(stderr, "Error creating sequencer port.\n");
    exit(1);
  }
  return(seq_handle);
}

void midi_receive(snd_seq_t *seq_handle, int fd) {

  snd_seq_event_t *ev;

  do {
    snd_seq_event_input(seq_handle, &ev);
    switch (ev->type) {
      case SND_SEQ_EVENT_NOTEON:
        setKey(fd,ev->data.note.note,56);
        break;        
      case SND_SEQ_EVENT_NOTEOFF: 
        setKey(fd,ev->data.note.note,12);
        break;
    }
    snd_seq_free_event(ev);
  } while (snd_seq_event_input_pending(seq_handle, 0) > 0);
}
void setKey(int launchpad, int key, char c)
{
	if(numout >= MAX_QUEUE)
	{
		perror("Output Buffer Full");
		return;
	}
	int pos;
	char buf[3];
	buf[0] = 0x90;
    buf[1] = key;
	buf[2] = c;
	pos = (posout + numout)%MAX_QUEUE;
	outbuf[3*pos] = buf[0];
	outbuf[3*pos+1] = buf[1];
	outbuf[3*pos+2] = buf[2];
	numout++;
}

void midi_send(snd_seq_t *seq_handle, struct point *event)
{
    snd_seq_event_t ev;

    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, 0);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    // set event type, data
    if(event->down)
    {
        ev.type = SND_SEQ_EVENT_NOTEON;
    } else {
        ev.type = SND_SEQ_EVENT_NOTEOFF;
    }
    ev.data.note.note = event->y*16+event->x;

    snd_seq_event_output(seq_handle, &ev);
    snd_seq_drain_output(seq_handle);
}

int main(int argc, char *argv[]) {
  /* Variables */
    snd_seq_t *color_handle;
    snd_seq_t *touch_handle;
    int colornumfd,i,maxcon;
    struct pollfd *pfd;
    fd_set connections;
    int launchpad;
    struct point launchpad_event;
 
    /* Setup Midi Ports */
    color_handle = open_color_seq();
    touch_handle = open_touch_seq();
    colornumfd = snd_seq_poll_descriptors_count(color_handle, POLLIN);
    pfd = (struct pollfd *)alloca(colornumfd * sizeof(struct pollfd));
    snd_seq_poll_descriptors(color_handle, pfd, colornumfd, POLLIN);

    /* Setup Launchpad */
	if((launchpad = open("/dev/launchpad0",O_RDWR)) < 0) {
		perror("device");
		exit(1);
	}
	
    while (1) {
        /* Setup file descriptor set */
		FD_ZERO(&connections);
		FD_SET(launchpad, &connections);
		maxcon = launchpad;
		for(i = 0; i < colornumfd; ++i)
		{
			FD_SET(pfd[i].fd, &connections);
			if(pfd[i].fd > maxcon)
				maxcon = pfd[i].fd;
		}
		
		/* Wait for an action */
		select(maxcon + 1, &connections,
			(fd_set *) 0, (fd_set *) 0, NULL);
			            
        if(FD_ISSET(launchpad, &connections))
        {
            launchpad_event = getKey(launchpad);
            midi_send(touch_handle,&launchpad_event);
        }
        else
        {
            midi_receive(color_handle,launchpad);
        }
        if(numout >0)
		{
			while(numout>0)
			{
				//printf("writing buffer: [%x %x %x]\n",outbuf[3*posout],outbuf[3*posout+1],outbuf[3*posout+2]);
				
				if(write(launchpad,outbuf+3*posout,3) < 0)
				{
					perror("Invalid launchpad write");
				}
				close(launchpad);
				if((launchpad = open("/dev/launchpad0",O_RDWR)) < 0) {
					perror("device");
					exit(1);
				}
				posout++;
				numout--;
				if(posout == MAX_QUEUE)
					posout = 0;
			}
		}
    }
}

struct point getKey(int launchpad)
{
	struct point retval;
	char msg[3];
	int b;
	//printf("about to read\n");
	/* Read in next press message */
	b = read(launchpad,msg,3);
	if(b < 0) {
		perror("Read Error");
		retval.control = -1;
		return retval;
	}
	//printf("finished read\n");
	if(b==3)
	{
		live_push = (msg[0]==0xB0);
		msg[0] = msg[1];
		msg[1] = msg[2];
	}
	
	//printf("reading: [%x %x]\n",msg[0],msg[1]);
	if(live_push)
	{
		retval.control = 1;
		retval.y = 0;
		retval.x = msg[0]-104;
		retval.down = (msg[1] != 0);
		return retval;
	}
	retval.control = 0;
	retval.down = (msg[1] != 0);
	retval.x = msg[0]%16;
	retval.y = msg[0]/16;
	return retval;
}
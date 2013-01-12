#include "liblaunchpad.h"
#include <signal.h>
#include <alsa/asoundlib.h>

#define MAX_QUEUE 256

struct ud {
  struct launchpad_handle* lh;
  snd_seq_t* th;
  snd_seq_t* ch;
};

struct launchpad_handle* lhandle;
int live_push = 0;
char outbuf[MAX_QUEUE*3];
int numout = 0, posout = 0;
int running = 1;
int channel = 1;
int volume = 64;

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

void setKey(int key, char c)
{
  if(numout >= MAX_QUEUE) {
    perror("Output Buffer Full");
    return;
  }
  int pos;
  char buf[3];
  buf[0] = 0x90;
  buf[1] = key;
  buf[2] = c;
  pos = (posout + numout) % MAX_QUEUE;
  outbuf[3 * pos] = buf[0];
  outbuf[3 * pos + 1] = buf[1];
  outbuf[3 * pos + 2] = buf[2];
  numout++;
}

void midi_receive(snd_seq_t *seq_handle) {

  snd_seq_event_t *ev;
  int v;
  do {
    snd_seq_event_input(seq_handle, &ev);
    switch (ev->type) {
      case SND_SEQ_EVENT_NOTEON:
        v = ev->data.note.velocity;
        if (v == 64) {
          v = 60;
        }
        setKey(ev->data.note.note, v);
        break;
      case SND_SEQ_EVENT_NOTEOFF:
        setKey(ev->data.note.note, 12);
        break;
    }
    snd_seq_free_event(ev);
  } while (snd_seq_event_input_pending(seq_handle, 0) > 0);
}

void launchpadCallback(unsigned char* data, size_t len, void* user_data)
{
  int proc = 0;
  snd_seq_event_t ev;
  struct ud* userdata = (struct ud*)user_data;
  if(len == 0 && numout > 0) {
    if(launchpad_write(userdata->lh, outbuf + 3 * posout, 3) == 0) {
      posout++;
      numout--;
      if(posout == MAX_QUEUE)
        posout = 0;
    }
  }
  while(len - proc >= 2) {
    if(data[proc] == 0xB0) {
      live_push = 1;
      proc++;
    } else if(data[proc] == 0x90) {
      live_push = 0;
      proc++;
    } else {
      snd_seq_ev_clear(&ev);
      snd_seq_ev_set_source(&ev, 0);
      snd_seq_ev_set_subs(&ev);
      snd_seq_ev_set_direct(&ev);
      if(!live_push) {
        ev.type = (data[proc + 1] != 0)? SND_SEQ_EVENT_NOTEON : SND_SEQ_EVENT_NOTEOFF;
        ev.data.note.channel = channel;
        ev.data.note.velocity = volume;
        ev.data.note.note = data[proc];
        snd_seq_event_output(userdata->th, &ev);
        snd_seq_drain_output(userdata->th);
      } else if(data[proc + 1] != 0) { //only care about key-down
        if(data[proc] - 104 <= 1) {
          //volume
          if(data[proc] - 104 == 0) {
            volume = (volume > 116)? 127 : volume + 10;
          } else {
            volume = (volume < 11)? 0 : volume - 10;
          }
          ev.type = SND_SEQ_EVENT_CONTROLLER;
          ev.data.control.channel = channel;
          ev.data.control.param = 7;
          ev.data.control.value = volume;
        } else if(data[proc] - 104 <= 3) {
          ev.type = SND_SEQ_EVENT_CONTROLLER;
          ev.data.control.channel = channel;
          ev.data.control.param = 7;
          ev.data.control.value = volume;
          if(data[proc] - 104 == 2) {
            channel = (channel>126)?127:channel+1;
          } else {
            channel = (channel<1)?0:channel-1;
          }
        }
        snd_seq_event_output(userdata->th, &ev);
        snd_seq_drain_output(userdata->th);
      }
      proc+=2;
    }
  }
}

void leave(int s)
{
  running = 0;
}

int main(int argc, char *argv[]) {
  /* signals */
  signal (SIGINT, leave);
  signal (SIGTERM, leave);
  signal (SIGQUIT, leave);

  /* Variables */
  int colornumfd, i, maxcon;
  struct pollfd *pfd;
  struct ud userdata;

  /* Setup Midi Ports */
  userdata.ch = open_color_seq();
  userdata.th = open_touch_seq();
  colornumfd = snd_seq_poll_descriptors_count(userdata.ch, POLLIN);
  pfd = (struct pollfd *)alloca(colornumfd * sizeof(struct pollfd));
  snd_seq_poll_descriptors(userdata.ch, pfd, colornumfd, POLLIN);

  /* Setup Launchpad */
  userdata.lh = launchpad_register(&launchpadCallback, &userdata);
  if(userdata.lh == NULL) {
    return 1;
  }
  lhandle = userdata.lh;

  /* Main Loop */
  while (running) {
    if(launchpad_poll(pfd,colornumfd)) {
      midi_receive(userdata.ch);
      if(numout > 0 && launchpad_write(userdata.lh, outbuf + 3 * posout, 3) == 0) {
        posout++;
        numout--;
        if(posout == MAX_QUEUE)
          posout = 0;
      }
    }
  }

  /* Cleanup */
  launchpad_deregister(userdata.lh);
  return 0;
}

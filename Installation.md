# Introduction #

Currently launchpadd is not what one would consider a daemon.  The good news is that daemon-ness will come soon.  But, if you want to play with the code now, I would definitely appreciate the bug testing.

# Dependencies #

If you haven't built packages before you need to install gcc, and all the development headers that go along with it.

You'll need alsa and it's development headers to build the midi part.  look for libasound-dev in your package manager.

The other dependency that is worth mentioning is the new version of libusb.  You can grab it in ubuntu by running
`sudo apt-get install libusb-1.0-0-dev`

# Building #

Build the executable via the following three shell commands:

```
$ autoreconf -i
$ ./configure
$ make
```

# Running #

start the program with
`sudo ./launchpadmidi`

you should now be able to see the two midi channels that have been created by running
`aconnect -i` and `aconnect -o`
connect the two and verify that lights turn on when you press them.

## Issues? ##

Please send an email to `willscott@gmail.com` if you have problems, and I'll do what I can to help!
launchpadmidi : launchpadmidi.o liblaunchpad.o
	gcc -g -o launchpadmidi launchpadmidi.o liblaunchpad.o -lusb-1.0 -lasound

launchpadmidi.o : launchpadmidi.c
	gcc -g -c launchpadmidi.c

liblaunchpad.o : liblaunchpad.c
	gcc -g -lusb-1.0 -c -Wall liblaunchpad.c

launchpadmidi.o liblaunchpad.o : liblaunchpad.h

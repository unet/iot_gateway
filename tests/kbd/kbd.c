#include <stdio.h>

#include <string.h>     /* strerror() */
#include <errno.h>      /* errno */
#include <time.h>

#include <fcntl.h>      /* open() */
#include <unistd.h>     /* close() */
#include <sys/ioctl.h>  /* ioctl() */

#include <linux/input.h>    /* EVIOCGVERSION ++ */

#define EV_BUF_SIZE 16


void flash_led(int fd) {
	static int state=0;
	struct input_event ev[4];
	int i;
	state^=1;
	for(i=0;i<1;i++) {
		ev[i].time.tv_sec=0;
		ev[i].time.tv_usec=0;
		ev[i].type=EV_LED;
		ev[i].code=i;
		ev[i].value=state;
	}

	ev[i].type=EV_KEY;
	ev[i].code=KEY_0;
	ev[i].value=state;
	i++;

	ev[i].type=EV_SYN;
	ev[i].code=SYN_REPORT;
	ev[i].value=0;
	i++;
	int sz=write(fd, ev, sizeof(ev[0])*i);
	if(sz<0) {
		fprintf(stderr, "Error %s in write\n", strerror(errno));
	}
}

int main(int argc, char *argv[])
{
    int fd, sz;
    unsigned i;

    /* A few examples of information to gather */
    unsigned version;
    unsigned short id[4];                   /* or use struct input_id */
    char name[256] = "N/A";

    struct input_event ev[EV_BUF_SIZE]; /* Read up to N events ata time */

    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s /dev/input/eventN\n"
            "Where X = input device number\n",
            argv[0]
        );
        return EINVAL;
    }

    if ((fd = open(argv[1], O_RDWR)) < 0) {
        fprintf(stderr,
            "ERR %d:\n"
            "Unable to open `%s'\n"
            "%s\n",
            errno, argv[1], strerror(errno)
        );
        return 1;
    }
    /* Error check here as well. */
    ioctl(fd, EVIOCGVERSION, &version);
    ioctl(fd, EVIOCGID, id); 
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);

	unsigned evbits;
    ioctl(fd, EVIOCGBIT(0,sizeof(evbits)), &evbits);
	unsigned long long keybits[4]={0,0,0,0};
    ioctl(fd, EVIOCGBIT(EV_KEY,sizeof(keybits)), keybits);

    fprintf(stderr,
        "Name      : %s\n"
        "Version   : %d.%d.%d\n"
        "ID        : Bus=%04x Vendor=%04x Product=%04x Version=%04x\n"
        "EV bits   : %x\n"
        "KEY bits  : %016llx %016llx %016llx %016llx\n"
        "----------\n"
        ,
        name,

        version >> 16,
        (version >> 8) & 0xff,
        version & 0xff,

        id[ID_BUS],
        id[ID_VENDOR],
        id[ID_PRODUCT],
        id[ID_VERSION],
        evbits,
        keybits[3],keybits[2],keybits[1],keybits[0]
    );

    /* Loop. Read event file and parse result. */
	int waitsyn=0;
	while(1) {
	flash_led(fd);
	sleep(1);
}
//	flash_led(fd);
//	sleep(1);
//	flash_led(fd);
    for (;;) {
        sz = read(fd, ev, sizeof(struct input_event) * EV_BUF_SIZE);

        if (sz < (int) sizeof(struct input_event)) {
            fprintf(stderr,
                "ERR %d:\n"
                "Reading of `%s' failed\n"
                "%s\n",
                errno, argv[1], strerror(errno)
            );
            goto fine;
        }

        /* Implement code to translate type, code and value */
        for (i = 0; i < sz / sizeof(struct input_event); ++i) {
			if(waitsyn) {
				if(ev[i].type==EV_SYN) waitsyn=0;
				continue;
			}
			if(ev[i].type==EV_SYN) {
				if(ev[i].code==SYN_DROPPED) waitsyn=1;
				continue;
			} else if(ev[i].type==EV_KEY) {
	            fprintf(stderr,
	                "%ld.%06ld: "
	                "code=%02x "
	                "value=%s\n",
	                ev[i].time.tv_sec,
	                ev[i].time.tv_usec,
	                ev[i].code,
	                ev[i].value == 1 ? "pressed" : ev[i].value==0 ? "released" : "repeated"
	            );
				if(ev[i].value==1) flash_led(fd);
			} else if(ev[i].type==EV_LED) {
	            fprintf(stderr,
	                "%ld.%06ld: "
	                "LED code=%02x "
	                "value=%s\n",
	                ev[i].time.tv_sec,
	                ev[i].time.tv_usec,
	                ev[i].code,
	                ev[i].value == 1 ? "on" : "off"
	            );
			}
        }
    }

fine:
    close(fd);

    return errno;
}

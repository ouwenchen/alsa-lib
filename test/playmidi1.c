/*
 *   MIDI file player for ALSA sequencer 
 *   (type 0 only!, the library that is used doesn't support merging of tracks)
 *
 *   Copyright (c) 1998 by Frank van de Pol <F.K.W.van.de.Pol@inter.nl.net>
 *
 *   Modified so that this uses alsa-lib
 *   1999 Jan. by Isaku Yamahata <yamahata@kusm.kyoto-u.ac.jp>
 *
 *   19990604	Takashi Iwai <iwai@ww.uni-erlangen.de>
 *	- use blocking mode
 *	- fix tempo event bug
 *	- add command line options
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "midifile.h"		/* SMF library header */
#include "midifile.c"		/* SMF library code */

#include "../include/asoundlib.h"

/* define this if you want to send real-time time stamps instead of midi ticks to the ALSA sequencer */
/* #define USE_REALTIME */

/* define this if you want to control event buffering by blocking mode */
#define USE_BLOCKING_MODE

/* default destination queue, client and port numbers */
#define DEST_QUEUE_NUMBER	7
#define DEST_CLIENT_NUMBER	65
#define DEST_PORT_NUMBER	0

/* event pool size */
#define WRITE_POOL_SIZE		200
#define WRITE_POOL_SPACE	10
#define READ_POOL_SIZE		10	/* we need read pool only for echoing */

static FILE *F;
static snd_seq_t *seq_handle = NULL;
static int ppq = 96;

static double local_secs = 0;
static int local_ticks = 0;
static int local_tempo = 500000;

static int dest_queue = DEST_QUEUE_NUMBER;
static int dest_client = DEST_CLIENT_NUMBER;
static int dest_port = DEST_PORT_NUMBER;
static int source_channel = 0;
static int source_port = 0;

static int verbose = 0;
static int slave   = 0;		/* allow external sync */

#define VERB_INFO	1
#define VERB_MUCH	2
#define VERB_EVENT	3

static void alsa_start_timer(void);
static void alsa_stop_timer(void);
static void wait_start(void);


static inline double tick2time_dbl(int tick)
{
	return local_secs + ((double) (tick - local_ticks) * (double) local_tempo * 1.0E-6 / (double) ppq);
}

#ifdef USE_REALTIME
static void tick2time(snd_seq_real_time_t * tm, int tick)
{
	double secs = tick2time_dbl(tick);
	tm->tv_sec = secs;
	tm->tv_nsec = (secs - tm->tv_sec) * 1.0E9;
}
#endif

#ifdef USE_BLOCKING_MODE
/* write event - using blocking mode */
static void write_ev_im(snd_seq_event_t *ev)
{
	int written;

	written = snd_seq_event_output(seq_handle, ev);
	if (written < 0) {
		printf("written = %i (%s)\n", written, snd_strerror(written));
		exit(1);
	}
}
#else
/* write event - using select syscall */
static void write_ev_im(snd_seq_event_t *ev)
{
	int rc;

	while ((rc = snd_seq_event_output(seq_handle, ev)) < 0) {
		int seqfd;
		fd_set fds;
		seqfd = snd_seq_file_descriptor(seq_handle);
		FD_ZERO(&fds);
		FD_SET(seqfd, &fds);
		if ((rc = select(seqfd + 1, NULL, &fds, NULL, NULL)) < 0) {
			printf("select error = %i (%s)\n", rc, snd_strerror(rc));
			exit(1);
		}
	}
}
#endif

/* write event to ALSA sequencer */
static void write_ev(snd_seq_event_t * ev)
{
	ev->flags &= ~SND_SEQ_EVENT_LENGTH_MASK;
	ev->flags |= SND_SEQ_EVENT_LENGTH_FIXED;
	write_ev_im(ev);
}

/* write variable length event to ALSA sequencer */
static void write_ev_var(snd_seq_event_t * ev, int len, void *ptr)
{
	ev->flags &= ~SND_SEQ_EVENT_LENGTH_MASK;
	ev->flags |= SND_SEQ_EVENT_LENGTH_VARIABLE;
	ev->data.ext.len = len;
	ev->data.ext.ptr = ptr;
	write_ev_im(ev);
}

/* read byte */
static int mygetc(void)
{
	return getc(F);
}

/* print out text */
static void mytext(int type, int leng, char *msg)
{
	char *p;
	char *ep = msg + leng;

	if (verbose >= VERB_INFO) {
		for (p = msg; p < ep; p++)
			putchar(isprint(*p) ? *p : '?');
		putchar('\n');
	}
}

static void do_header(int format, int ntracks, int division)
{
	snd_seq_queue_tempo_t	tempo;

	if (verbose >= VERB_INFO)
		printf("smf format %d, %d tracks, %d ppq\n", format, ntracks, division);
	ppq = division;

	if (format != 0 || ntracks != 1) {
		printf("This player does not support merging of tracks.\n");
		alsa_stop_timer();
		exit(1);
	}
	/* set ppq */
	/* ppq must be set before starting timer */
	if (snd_seq_get_queue_tempo(seq_handle, dest_queue, &tempo) < 0) {
    		perror("get_queue_tempo");
    		exit(1);
	}
	if (tempo.ppq != ppq) {
		tempo.ppq = ppq;
		if (snd_seq_set_queue_tempo(seq_handle, dest_queue, &tempo) < 0) {
    			perror("set_queue_tempo");
    			if (!slave)
    				exit(1);
		}
		if (verbose >= VERB_INFO)
			printf("ALSA Timer updated, PPQ = %d\n", tempo.ppq);
	}

	/* start playing... */
	if (slave) {
		if (verbose >= VERB_INFO)
			printf("Wait till timer starts...\n");	
		wait_start();
		if (verbose >= VERB_INFO)
			printf("Go!\n");	
	} else
		alsa_start_timer();
}

/* fill normal event header */
static void set_event_header(snd_seq_event_t *ev, int type, int chan)
{
	ev->source.port = source_port;
	ev->source.channel = source_channel;

	ev->dest.queue = dest_queue;
	ev->dest.client = dest_client;
	ev->dest.port = dest_port;
	ev->dest.channel = chan;

#ifdef USE_REALTIME
	ev->flags = SND_SEQ_TIME_STAMP_REAL | SND_SEQ_TIME_MODE_ABS;
	tick2time(&ev->time.real, Mf_currtime);
#else
	ev->flags = SND_SEQ_TIME_STAMP_TICK | SND_SEQ_TIME_MODE_ABS;
	ev->time.tick = Mf_currtime;
#endif

	ev->type = type;
}

/* fill timer event header */
static void set_timer_event_header(snd_seq_event_t *ev, int type)
{
	ev->source.port = source_port;
	ev->source.channel = 0;

	ev->dest.queue = dest_queue;
	ev->dest.client = SND_SEQ_CLIENT_SYSTEM;	/* system */
	ev->dest.port = SND_SEQ_PORT_SYSTEM_TIMER;	/* timer */
	ev->dest.channel = 0;	/* don't care */

#ifdef USE_REALTIME
	ev->flags = SND_SEQ_TIME_STAMP_REAL | SND_SEQ_TIME_MODE_ABS;
	tick2time(&ev->time.real, Mf_currtime);
#else
	ev->flags = SND_SEQ_TIME_STAMP_TICK | SND_SEQ_TIME_MODE_ABS;
	ev->time.tick = Mf_currtime;
#endif

	ev->type = type;
}

/* start timer */
static void alsa_start_timer(void)
{
	snd_seq_event_t ev;

	set_timer_event_header(&ev, SND_SEQ_EVENT_START);
#ifdef USE_REALTIME
	ev.flags = SND_SEQ_TIME_STAMP_REAL | SND_SEQ_TIME_MODE_REL;
	ev.time.real.tv_sec = 0;
	ev.time.real.tv_nsec = 0;
#else
	ev.flags = SND_SEQ_TIME_STAMP_TICK | SND_SEQ_TIME_MODE_REL;
	ev.time.tick = Mf_currtime;
#endif

	write_ev(&ev);
}

/* stop timer */
static void alsa_stop_timer(void)
{
	snd_seq_event_t ev;

	set_timer_event_header(&ev, SND_SEQ_EVENT_STOP);
	write_ev(&ev);
}

/* change tempo */
static void do_tempo(int us)
{
	snd_seq_event_t ev;

	if (verbose >= VERB_MUCH) {
		double bpm;
		bpm = 60.0E6 / (double) us;
		printf("Tempo %d us/beat, %.2f bpm\n", us, bpm);
	}

	/* store new tempo and timestamp of tempo change */
	local_secs = tick2time_dbl(Mf_currtime);
	local_ticks = Mf_currtime;
	local_tempo = us;

	set_timer_event_header(&ev, SND_SEQ_EVENT_TEMPO);
	ev.data.control.value = us;
	if (!slave)
		write_ev(&ev);
}

static void do_noteon(int chan, int pitch, int vol)
{
	snd_seq_event_t ev;

	if (verbose >= VERB_EVENT)
		printf("NoteOn (%d) %d %d\n", chan, pitch, vol);
	set_event_header(&ev, SND_SEQ_EVENT_NOTEON, chan);
	ev.data.note.note = pitch;
	ev.data.note.velocity = vol;

	write_ev(&ev);
}


static void do_noteoff(int chan, int pitch, int vol)
{
	snd_seq_event_t ev;

	if (verbose >= VERB_EVENT)
		printf("NoteOff (%d) %d %d\n", chan, pitch, vol);
	set_event_header(&ev, SND_SEQ_EVENT_NOTEOFF, chan);
	ev.data.note.note = pitch;
	ev.data.note.velocity = vol;

	write_ev(&ev);
}


static void do_program(int chan, int program)
{
	snd_seq_event_t ev;

	if (verbose >= VERB_EVENT)
		printf("Program (%d) %d\n", chan, program);
	set_event_header(&ev, SND_SEQ_EVENT_PGMCHANGE, chan);
	ev.data.control.value = program;

	write_ev(&ev);
}


static void do_parameter(int chan, int control, int value)
{
	snd_seq_event_t ev;

	if (verbose >= VERB_EVENT)
		printf("Control (%d) %d %d\n", chan, control, value);
	set_event_header(&ev, SND_SEQ_EVENT_CONTROLLER, chan);
	ev.data.control.param = control;
	ev.data.control.value = value;

	write_ev(&ev);
}


static void do_pitchbend(int chan, int lsb, int msb)
{	/* !@#$% lsb & msb are in wrong order in docs */
	snd_seq_event_t ev;

	if (verbose >= VERB_EVENT)
		printf("Pitchbend (%d) %d %d\n", chan, lsb, msb);
	set_event_header(&ev, SND_SEQ_EVENT_PITCHBEND, chan);
	ev.data.control.value = (lsb + (msb << 7)) - 8192;

	write_ev(&ev);
}

static void do_pressure(int chan, int pitch, int pressure)
{
	snd_seq_event_t ev;

	if (verbose >= VERB_EVENT)
		printf("KeyPress (%d) %d %d\n", chan, pitch, pressure);
	set_event_header(&ev, SND_SEQ_EVENT_KEYPRESS, chan);
	ev.data.control.param = pitch;
	ev.data.control.value = pressure;

	write_ev(&ev);
}

static void do_chanpressure(int chan, int pressure)
{
	snd_seq_event_t ev;

	if (verbose >= VERB_EVENT)
		printf("ChanPress (%d) %d\n", chan, pressure);
	set_event_header(&ev, SND_SEQ_EVENT_CHANPRESS, chan);
	ev.data.control.value = pressure;

	write_ev(&ev);
}

static void do_sysex(int len, char *msg)
{
	snd_seq_event_t ev;

	if (verbose >= VERB_MUCH) {
		int c;
		printf("Sysex, len=%d\n", len);
		for (c = 0; c < len; c++) {
			printf(" %02x", (unsigned char)msg[c]);
			if (c % 16 == 15)
				putchar('\n');
		}
		if (c % 16 != 15)
			putchar('\n');
	}

	set_event_header(&ev, SND_SEQ_EVENT_SYSEX, 0);
	write_ev_var(&ev, len, msg);
}

/* synchronize to the end of event */
static void alsa_sync(void)
{
	int left;
	snd_seq_event_t* input_event;
	snd_seq_event_t ev;
  
	/* send echo event to self client. */
	if (verbose >= VERB_MUCH)
		printf("alsa_sync syncing... send ECHO(%d) event to myself. time=%f\n",
		       SND_SEQ_EVENT_ECHO, (double) Mf_currtime+1);
	ev.source.port = source_port;
	ev.source.channel = source_channel;
	ev.dest.queue = dest_queue;
	ev.dest.client = snd_seq_client_id(seq_handle);
	ev.dest.port = source_port;
	ev.dest.channel = 0;	/* don't care */

#ifdef USE_REALTIME
	ev.flags = SND_SEQ_TIME_STAMP_REAL | SND_SEQ_TIME_MODE_ABS;
	tick2time(&ev.time.real, Mf_currtime+1);
#else
	ev.flags = SND_SEQ_TIME_STAMP_TICK | SND_SEQ_TIME_MODE_ABS;
	ev.time.tick = Mf_currtime+1;
#endif
	ev.type = SND_SEQ_EVENT_ECHO;
	write_ev(&ev);
  
	/* dump buffer */
	left = snd_seq_flush_output(seq_handle);

	/* wait for the timer start event */
#ifdef USE_BLOCKING_MODE
	/* read event - blocked until any event is read */
	left = snd_seq_event_input(seq_handle, &input_event);
#else
	/* read event - using select syscall */
	while ((left = snd_seq_event_input(seq_handle, &input_event)) >= 0 &&
	       input_event == NULL) {
		int seqfd;
		fd_set fds;
		seqfd = snd_seq_file_descriptor(seq_handle);
		FD_ZERO(&fds);
		FD_SET(seqfd, &fds);
		if ((left = select(seqfd + 1, &fds, NULL, NULL, NULL)) < 0) {
			printf("select error = %i (%s)\n", left, snd_strerror(left));
			exit(1);
		}
	}
#endif
	if (left < 0) {
		printf("alsa_sync error!:%s\n", snd_strerror(left));
		return;
	}

	if (verbose >= VERB_MUCH)
		printf("alsa_sync got event. type=%d, flags=%d\n",
		       input_event->type, input_event->flags);
	snd_seq_free_event(input_event);
  
	if (verbose >= VERB_MUCH)
		printf("alsa_sync synced\n");
}


/* wait for start of the queue */
static void wait_start(void)
{
	int left;
	snd_seq_event_t* input_event;
  
	/* wait the start event from the system timer */
  	do {
#ifdef USE_BLOCKING_MODE
		/* read event - blocked until any event is read */
		left = snd_seq_event_input(seq_handle, &input_event);
#else
		/* read event - using select syscall */
		while ((left = snd_seq_event_input(seq_handle, &input_event)) >= 0 &&
		       input_event == NULL) {
			int seqfd;
			fd_set fds;
			seqfd = snd_seq_file_descriptor(seq_handle);
			FD_ZERO(&fds);
			FD_SET(seqfd, &fds);
			if ((left = select(seqfd + 1, &fds, NULL, NULL, NULL)) < 0) {
				printf("select error = %i (%s)\n", left, snd_strerror(left));
				exit(1);
			}
		}
#endif
		if (left < 0) {
			printf("alsa_sync error!:%s\n", snd_strerror(left));
			return;
		}

		if (verbose >= VERB_MUCH)
			printf("wait_start got event. type=%d, flags=%d\n",
			       input_event->type, input_event->flags);
		snd_seq_free_event(input_event);
	
	} while (input_event->type != SND_SEQ_EVENT_START);
			
	if (verbose >= VERB_MUCH)
		printf("start received\n");
}


/* print usage */
static void usage(void)
{
	fprintf(stderr, "usage: playmidi1 [options] [file]\n");
	fprintf(stderr, "  options:\n");
	fprintf(stderr, "  -v: verbose mode\n");
	fprintf(stderr, "  -a queue:client:port : set destination address (default=%d:%d:%d)\n",
		DEST_QUEUE_NUMBER, DEST_CLIENT_NUMBER, DEST_PORT_NUMBER);
	fprintf(stderr, "  -s: slave mode (allow external clock synchronisation)\n");
		
}

/* parse destination address (-a option) */
void parse_address(char *arg, int *queuep, int *clientp, int *portp)
{
	char *next;

	*queuep = atoi(arg);
	if ((next = strchr(arg, ':')) != NULL) {
		*clientp = atoi(next + 1);
		if ((next = strchr(next + 1, ':')) != NULL)
			*portp = atoi(next + 1);
	}
}

int main(int argc, char *argv[])
{
	snd_seq_client_info_t inf;
	snd_seq_port_info_t src_port_info;
	snd_seq_queue_client_t queue_info;
	snd_seq_port_subscribe_t subscribe;
	snd_seq_client_pool_t pool;
	int tmp;
	int c;

	while ((c = getopt(argc, argv, "sa:v")) != -1) {
		switch (c) {
		case 'v':
			verbose++;
			break;
		case 'a':
			parse_address(optarg, &dest_queue, &dest_client, &dest_port);
			break;
		case 's':
			slave = 1;
			break;
		default:
			usage();
			exit(1);
		}
	}

	if (verbose >= VERB_INFO) {
#ifdef USE_REALTIME
		printf("ALSA MIDI Player, feeding events to real-time queue\n");
#else
		printf("ALSA MIDI Player, feeding events to song queue\n");
#endif
	}

	/* open sequencer device */
	/* Here we open the device read/write mode. */
	/* Because we write SND_SEQ_EVENT_ECHO to myself to sync. */
	tmp = snd_seq_open(&seq_handle, SND_SEQ_OPEN);
	if (tmp < 0) {
		perror("open /dev/snd/seq");
		exit(1);
	}
	
#ifdef USE_BLOCKING_MODE
	tmp = snd_seq_block_mode(seq_handle, 1);
#else
	tmp = snd_seq_block_mode(seq_handle, 0);
#endif
	if (tmp < 0) {
		perror("block_mode");
		exit(1);
	}
			
	/* set name */
	/* set event filter to recieve only echo event */
	/* if running in slave mode also listen for START event */
	memset(&inf, 0, sizeof(snd_seq_client_info_t));
	inf.filter |= SND_SEQ_FILTER_USE_EVENT;
	memset(&inf.event_filter, 0, sizeof(inf.event_filter));
	snd_seq_set_bit(SND_SEQ_EVENT_ECHO, inf.event_filter);
	if (slave)
		snd_seq_set_bit(SND_SEQ_EVENT_START, inf.event_filter);
	strcpy(inf.name, "MIDI file player");
	if (snd_seq_set_client_info(seq_handle, &inf) < 0) {
		perror("ioctl");
		exit(1);
	}

	/* create port */
	memset(&src_port_info, 0, sizeof(snd_seq_port_info_t));
	src_port_info.capability = SND_SEQ_PORT_CAP_OUT | SND_SEQ_PORT_CAP_IN;
	src_port_info.type = SND_SEQ_PORT_TYPE_MIDI_GENERIC;
	src_port_info.midi_channels = 16;
	src_port_info.synth_voices = 0;
	src_port_info.kernel = NULL;
	tmp = snd_seq_create_port(seq_handle, &src_port_info);
	if (tmp < 0) {
		perror("creat port");
		exit(1);
	}
	source_port = src_port_info.port;
	
	/* setup queue */
	bzero(&queue_info,sizeof(queue_info));
	queue_info.used = 1;
	tmp = snd_seq_set_queue_client(seq_handle, dest_queue, &queue_info);
	if (tmp < 0) {
		perror("queue_client");
		exit(1);
	}

	/* setup subscriber */
	bzero(&subscribe,sizeof(subscribe));
	if (verbose >= VERB_INFO)
		printf("debug subscribe src_port_info.client=%d\n",
		       src_port_info.client);
	subscribe.sender.client = snd_seq_client_id(seq_handle);
	subscribe.sender.queue = dest_queue;
	subscribe.sender.port = src_port_info.port;
	subscribe.dest.client = dest_client;
	subscribe.dest.port = dest_port;
	subscribe.dest.queue = dest_queue;
	subscribe.realtime = 1;
	subscribe.exclusive = 0;
	tmp = snd_seq_subscribe_port(seq_handle, &subscribe);
	if (tmp < 0) {
		perror("subscribe");
		exit(1);
	}

	/* subscribe for timer START event */	
	if (slave) {	
		subscribe.sender.client = SND_SEQ_CLIENT_SYSTEM;
		subscribe.sender.queue = dest_queue;
		subscribe.sender.port = SND_SEQ_PORT_SYSTEM_TIMER;
		subscribe.dest.client = snd_seq_client_id(seq_handle);
		subscribe.dest.port = src_port_info.port;
		subscribe.dest.queue = dest_queue;
		subscribe.realtime = 0;
		subscribe.exclusive = 0;
		tmp = snd_seq_subscribe_port(seq_handle, &subscribe);
		if (tmp < 0) {
			perror("subscribe");
			exit(1);
		}	
	}
	
	/* change pool size */
	bzero(&pool,sizeof(pool));
	pool.output_pool = WRITE_POOL_SIZE;
	pool.input_pool = READ_POOL_SIZE;
	pool.output_room = WRITE_POOL_SPACE;
	tmp = snd_seq_set_client_pool(seq_handle, &pool);
	if (tmp < 0) {
		perror("pool");
		exit(1);
	}
	
	if (optind < argc) {
		F = fopen(argv[optind], "r");
		if (F == NULL) {
			fprintf(stderr, "playmidi1: can't open file %s\n", argv[optind]);
			exit(1);
		}
	} else
		F = stdin;

	Mf_header = do_header;
	Mf_tempo = do_tempo;
	Mf_getc = mygetc;
	Mf_text = mytext;

	Mf_noteon = do_noteon;
	Mf_noteoff = do_noteoff;
	Mf_program = do_program;
	Mf_parameter = do_parameter;
	Mf_pitchbend = do_pitchbend;
	Mf_pressure = do_pressure;
	Mf_chanpressure = do_chanpressure;
	Mf_sysex = do_sysex;

	/* go.. go.. go.. */
	mfread();

	alsa_sync();
	alsa_stop_timer();

	snd_seq_close(seq_handle);

	if (verbose >= VERB_INFO) {
		printf("Stopping at %f s,  tick %f\n",
		       tick2time_dbl(Mf_currtime + 1), (double) (Mf_currtime + 1));
	}

	exit(0);
}

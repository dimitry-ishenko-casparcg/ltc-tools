/*
 * Copyright (C) 2006, 2008, 2010, 2012 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2008-2009 Jan <jan@geheimwerk.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <getopt.h>
#include <ltc.h>
#include <sndfile.h>

#define FPRNT_TIME "%lf"
#define TIME_DELIM	"\t"

#define QUEUE_LENGTH 16

#define BUFFER_SIZE 1024
#define TIME_CODE_STRING_SIZE 12

int print_audacity_labels = 0;
int detect_discontinuities = 0;
int verbosity = 2;
int use_date = 1;

void print_header(FILE *outfile) {
	fprintf(outfile, "#");
	if (use_date)
		fprintf(outfile, "%-10s %-5s ", "Date", "Zone");
	fprintf(outfile, "%-10s | %17s\n", "Timecode", "Pos. (samples)");
}

void print_audacity_label(FILE *outfile, LTCDecoder* d, long int startInt, long int endInt, char *label) {
	double start, end;
#if 0
	start = SMPTEDecoderSamplesToSeconds(d, startInt);
	end = SMPTEDecoderSamplesToSeconds(d, endInt);
#else
	start = end = 0; // XXX
#endif

	fprintf(outfile, "" FPRNT_TIME TIME_DELIM FPRNT_TIME TIME_DELIM "%s\n", start, end, label);
}

void print_LTC_error(FILE *outfile, LTCDecoder* d, long int startInt, long int endInt, char *label) {
	if (print_audacity_labels) {
		print_audacity_label(outfile, d, startInt, endInt, label);
	} else {
		if (use_date)
			fprintf(outfile, "%-16s ", "");
		fprintf(outfile, "%-20s %8lu %8lu\n", label, startInt, endInt);
	}
}

void print_LTC_info(FILE *outfile, LTCDecoder* d, LTCFrameExt frame, SMPTETimecode stime) {
	if (print_audacity_labels) {
		char timeCodeString[TIME_CODE_STRING_SIZE];
		snprintf(timeCodeString, TIME_CODE_STRING_SIZE,
				 "%02d:%02d:%02d:%02d",
				 stime.hours, stime.mins,
				 stime.secs, stime.frame
				 );
		print_audacity_label(outfile, d, frame.off_start, frame.off_end, timeCodeString);
	} else {
		if (use_date)
			fprintf(outfile, "%04d-%02d-%02d %s ",
				((stime.years < 67) ? 2000+stime.years : 1900+stime.years),
				stime.months,
				stime.days,
				stime.timezone
				);
		fprintf(outfile, "%02d:%02d:%02d%c%02d | %8lld %8lld%s\n",
				stime.hours,
				stime.mins,
				stime.secs,
				(frame.ltc.dfbit) ? '.' : ':',
				stime.frame,
				frame.off_start,
				frame.off_end,
				frame.reverse ? " R" : "  "
				);
	}
}

int ltcdump(char *filename, int fps_num, int fps_den, int channel) {
	ltcsnd_sample_t sound[BUFFER_SIZE];
	float *interleaved = NULL;
	FILE * outfile = stdout;

	size_t n;
	size_t total = 0;

	SNDFILE * m_sndfile;
	SF_INFO sfinfo;

	LTCDecoder *decoder;
	LTCFrameExt frame;
	int print_missing_frame_info;

	m_sndfile = sf_open(filename, SFM_READ, &sfinfo);

	if (SF_ERR_NO_ERROR != sf_error( m_sndfile)) {
		fprintf(stderr, "Error: This is not a sndfile supported audio file format\n");
		return -1;
	}
	if (sfinfo.frames==0) {
		fprintf(stderr, "Error: This is an empty audio file\n");
		return -1;
	}

	if (channel > sfinfo.channels) channel=sfinfo.channels;
	if (channel < 1) channel=1;

	if (sfinfo.channels!=1 && verbosity > 0) {
		fprintf(stderr, "Note: This is not a mono audio file - using channel %i\n", channel);
	}

	interleaved=calloc(sfinfo.channels*BUFFER_SIZE, sizeof(float));

	if (print_audacity_labels) {
		verbosity = 0;
		print_missing_frame_info = 1;
	} else {
		print_missing_frame_info = (verbosity > 1);
	}

	if (verbosity > 1) {
		fprintf(outfile, "#SND: file = %s\n", filename);
		fprintf(outfile, "#LTC: analyzed channel = %d\n", channel);
		fprintf(outfile, "#SND: sample rate = %i\n", sfinfo.samplerate);
	}

	if (verbosity > 2) {
		fprintf(outfile, "#LTC: frames/sec = %i/%i\n", fps_num, fps_den);
	}

	if (!print_audacity_labels) {
		print_header(outfile);
	}

	long int ltc_frame_length_samples = sfinfo.samplerate * fps_den / fps_num;
	long int ltc_frame_length_fudge = (ltc_frame_length_samples * 101 / 100);

	long int prev_read = ltc_frame_length_samples;
	LTCFrame prev_time;

	decoder = ltc_decoder_create(sfinfo.samplerate * fps_den / fps_num, QUEUE_LENGTH);
	channel-=1; // channel-number starts counting at 1.

	do {
		int i;
		n = sf_readf_float(m_sndfile, interleaved, BUFFER_SIZE);
		for (i=0;i<n; i++)
			sound[i]= 128 + interleaved[sfinfo.channels*i+channel] * 127;

		ltc_decoder_write(decoder, sound, n, total);

		if (print_missing_frame_info) {
			long int fudge = prev_read + ltc_frame_length_fudge;
			if (total > fudge) {
				print_LTC_error(stdout, decoder, prev_read, prev_read + ltc_frame_length_samples, "No LTC frame found");
				prev_read = total;
			}
		}

		while (ltc_decoder_read(decoder,&frame)) {
			SMPTETimecode stime;

			ltc_frame_to_time(&stime, &frame.ltc, use_date);

#if 0
			SMPTEDecoderFrameToMillisecs(decoder, &frame, &ms);
			if (1) { // print start time referece in audio-samples
				 // for use with 'sndfile-timeref'
				uint64_t off = ms;
				off *= sfinfo.samplerate;
				off /= 1000;
				off -= frame.off_start;
				printf("%llu\n", off);
				return 0;
			}
#endif

#if 1 // TODO detect fps: use the
      // maximum frameno found in stream during the last n seconds
		if (detect_discontinuities) {
			ltc_frame_increment(&prev_time, ceil(fps_num/fps_den) , use_date);
			if (memcmp(&prev_time, &frame, sizeof(LTCFrame))) {
				fprintf(outfile, "#DISCONTINUITY\n");
			}
			memcpy(&prev_time, &frame, sizeof(LTCFrame));
		}
#endif

			print_LTC_info(stdout, decoder, frame, stime);
			prev_read = frame.off_end;
		}

		total += n;

	} while (n);

	sf_close(m_sndfile);
	free(interleaved);

	return 0;
}

static void usage (int status) {
	printf ("ltcdump - parse linear time code from a audio-file.\n\n");
	printf ("Usage: ltcdump [ OPTIONS ] <filename>\n\n");
	printf ("Options:\n\
  -c, --channel <num>        decode LTC from given audio-channel.\n\
  -f, --fps  <num>[/den]     set expected framerate.\n\
  -h, --help                 display this help and exit\n\
  -V, --version              print version information and exit\n\
\n");
	printf ("\n\
Channel count starts at '1', which is also the default channel to analyze.\n\
The fps option is only needed to properly track the first LTC frame.\n\
The LTC-decoder detects and tracks the speed but it takes a few samples\n\
to establish initial synchronization. Setting fps to the expected fps\n\
speeds up the initial sync process. The default is 25/1.\n\
\n");
	printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
	        "Website and manual: <https://github.com/x42/ltc-tools>\n");
	exit (status);
}

static struct option const long_options[] =
{
	{"help", no_argument, 0, 'h'},
	{"output", required_argument, 0, 'o'},
	{"signals", no_argument, 0, 's'},
	{"version", no_argument, 0, 'V'},
	{NULL, 0, NULL, 0}
};

int main(int argc, char **argv) {
	char* filename;
	int channel = 1;
	int fps_num=25;
	int fps_den=1;

	int c;
	while ((c = getopt_long (argc, argv,
			   "c:"	/* channel */
			   "f:"	/* fps */
			   "h"	/* help */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF)
	{
		switch (c) {
			case 'c':
				channel = atoi(optarg);
				break;

			case 'f':
				{
				fps_num = atoi(argv[3]);
				char *tmp = strchr(argv[3], '/');
				if (tmp) fps_den=atoi(++tmp);
				}
				break;

			case 'V':
				printf ("ltcdump version %s\n\n", VERSION);
				printf ("Copyright (C) GPL 2006,2012 Robin Gareus <robin@gareus.org>\n");
				exit (0);

			case 'h':
				usage (0);

			default:
				usage (EXIT_FAILURE);
		}
	}

	if (optind >= argc) {
		usage (EXIT_FAILURE);
	}

	filename = argv[optind];

	return ltcdump(filename, fps_num, fps_den, channel);
}
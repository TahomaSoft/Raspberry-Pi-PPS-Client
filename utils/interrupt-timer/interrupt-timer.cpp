/*
 * interrupt-timer.cpp
 *
 * Created on: Apr 7, 2016
 * Copyright (C) 2016  Raymond S. Connell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sched.h>
#include <math.h>
#include <sys/time.h>

#define INTRPT_DISTRIB_LEN 61
#define SECS_PER_DAY 86400
#define SECS_PER_MIN 60
#define ON_TIME 3
#define DELAYED 1
#define NONE 2
#define FILE_NOT_FOUND -1
#define FILEBUF_SZ 5000
#define STRBUF_SZ 200

#define USECS_PER_SEC 1000000
#define START_SAVE 20
#define START 10

int sysCommand(const char *cmd){
	int rv = system(cmd);
	if (rv == -1 || WIFEXITED(rv) == false){
		printf("system command failed: %s\n", cmd);
		return -1;
	}
	return 0;
}

struct interruptTimerGlobalVars {
	int outFormat;

	char strbuf[STRBUF_SZ];
	char filebuf[FILEBUF_SZ];
	int seconds;
	int minutes;
	int days;

	int scaleCenter;

	int intrptCount;
	int interruptDistrib[INTRPT_DISTRIB_LEN];

	int lastIntrptFileno;

	int tolerance[5];

	bool showAllTols;
} g;

const char *timer_distrib_file = "/var/local/timer-distrib-forming";
const char *last_timer_distrib_file = "/var/local/timer-distrib";

const char *version = "interrupt-timer v1.0.0";
const char *timefmt = "%F %H:%M:%S";

/**
 * Reads the major number assigned to interrupt-timer
 * from "/proc/devices" as a string which is
 * returned in the majorPos char pointer. This
 * value is used to load the hardware driver that
 * interrupt-timer requires.
 */
char *copyMajorTo(char *majorPos){

	struct stat stat_buf;
	int rv;

	const char *filename = "/run/shm/proc_devices";

	rv = sysCommand("cat /proc/devices > /run/shm/proc_devices"); 	// "/proc/devices" can't be handled like
	if (rv == -1){													// a normal file so we copy it to a file.
		return NULL;
	}

	int fd = open(filename, O_RDONLY);
	if (fd == -1){
		printf("Unable to open %s\n", filename);
		return NULL;
	}

	fstat(fd, &stat_buf);
	int sz = stat_buf.st_size;

	char *fbuf = new char[sz+1];

	rv = read(fd, fbuf, sz);
	if (rv == -1){
		printf("Unable to read file: %s\n", filename);
		close(fd);
		remove(filename);
		delete(fbuf);
		return NULL;
	}
	close(fd);
	remove(filename);

	fbuf[sz] = '\0';

	char *pos = strstr(fbuf, "interrupt-timer");
	if (pos == NULL){
		printf("Can't find interrupt-timer in \"/run/shm/proc_devices\"\n");
		delete fbuf;
		return NULL;
	}
	char *end = pos - 1;
	*end = 0;

	pos -= 2;
	char *pos2 = pos;
	while (pos2 == pos){
		pos -= 1;
		pos2 = strpbrk(pos,"0123456789");
	}
	strcpy(majorPos, pos2);

	delete fbuf;
	return majorPos;
}

/**
 * Loads the hardware driver required by interrupt-timer which
 * is expected to be available in the file:
 * "/lib/modules/'uname -r'/kernel/drivers/misc/interrupt-timer.ko".
 */
int driver_load(char *gpio){

	memset(g.strbuf, 0, STRBUF_SZ * sizeof(char));

	char *insmod = g.strbuf;
	strcpy(insmod, "/sbin/insmod /lib/modules/`uname -r`/kernel/drivers/misc/interrupt-timer.ko gpio_num=");
	strcat(insmod, gpio);

	int rv = sysCommand("rm -f /dev/interrupt-timer");	// Clean up any old device files.
	if (rv == -1){
		return -1;
	}
	rv = sysCommand(insmod);								// Issue the insmod command
	if (rv == -1){
		return -1;
	}

	char *mknod = g.strbuf;
	strcpy(mknod, "mknod /dev/interrupt-timer c ");
	char *major = copyMajorTo(mknod + strlen(mknod));
	if (major == NULL){									// No major found! insmod failed.
		printf("driver_load() error: No major found!\n");
		sysCommand("/sbin/rmmod interrupt-timer");
		return -1;
	}
	strcat(mknod, " 0");

	rv = sysCommand(mknod);										// Issue the mknod command
	if (rv == -1){
		return -1;
	}

	rv = sysCommand("chgrp root /dev/interrupt-timer");
	if (rv == -1){
		return -1;
	}
	rv = sysCommand("chmod 664 /dev/interrupt-timer");
	if (rv == -1){
		return -1;
	}

	return 0;
}

/**
 * Unloads the interrupt-timer kernel driver.
 */
void driver_unload(void){
	int rv = sysCommand("/sbin/rmmod interrupt-timer");
	if (rv == -1){
		exit(EXIT_FAILURE);
	}
	rv = sysCommand("rm -f /dev/interrupt-timer");
	if (rv == -1){
		exit(EXIT_FAILURE);
	}
}

/**
 * Writes an accumulating statistical distribution at regular intervals
 * to disk and rolls over the accumulating data to a new file every
 * epochInterval days and begins a new distribution file.
 */
void writeDistribution(int distrib[], int len, int scaleZero, int epochInterval,
		int *last_epoch, const char *distrib_file, const char *last_distrib_file){

	remove(distrib_file);
	int fd = open(distrib_file, O_CREAT | O_WRONLY | O_APPEND, S_IRUSR | S_IWUSR | S_IROTH);
	if (fd == -1){
		return;
	}
	int rv;
	for (int i = 0; i < len; i++){
		sprintf(g.strbuf, "%d %d\n", i-scaleZero, distrib[i]);
		rv = write(fd, g.strbuf, strlen(g.strbuf));
		if (rv == -1){
			printf("writeDistribution() write to %s failed\n", distrib_file);
			close(fd);
			return;
		}
	}
	close(fd);

	int epoch = g.days / epochInterval;
	if (epoch != *last_epoch ){
		*last_epoch = epoch;
		remove(last_distrib_file);
		rename(distrib_file, last_distrib_file);
		memset(distrib, 0, len * sizeof(int));
	}
}

/**
 * Writes a distribution to disk containing 60 additional
 * interrupt delays approximately every minute. Collects
 * one day of interrupt delay samples before rolling over
 * a new file.
 */
void writeInterruptDistribFile(void){
	int scaleZero = -(g.scaleCenter - (INTRPT_DISTRIB_LEN - 1) / 3);
	writeDistribution(g.interruptDistrib, INTRPT_DISTRIB_LEN, scaleZero, 1,
			&g.lastIntrptFileno, timer_distrib_file, last_timer_distrib_file);
}

/**
 * Accumulates a distribution of interrupt delay.
 */
void buildInterruptDistrib(int intrptDelay){
	int len = INTRPT_DISTRIB_LEN - 1;
	int idx;

	if (g.intrptCount == 0){
		g.scaleCenter = intrptDelay;
		g.intrptCount += 1;
		return;
	}
	else if (g.intrptCount < 60){				// During the first 60 counts get a rough scale center.
		if (intrptDelay > g.scaleCenter){
			g.scaleCenter += 1;
		}
		else if (intrptDelay < g.scaleCenter){
			g.scaleCenter -= 1;
		}
		g.intrptCount += 1;
		return;
	}

	idx = intrptDelay - g.scaleCenter;			// Normalize idx to the scale center.
												// Since that normalizes it to zero,
	idx = idx + len / 3;						// offset idx to the lower third of
												// the g.interruptDistrib array.
	if (idx > len){
		idx = len;
	}
	else if (idx < 0){
		idx = 0;
	}

	g.intrptCount += 1;
	g.interruptDistrib[idx] += 1;
}

/**
 * Reads the sysDelay value recorded by
 * pps-client.
 */
int getSysDelay(int *sysDelay){

	const char* filename = "/run/shm/pps-sysDelay";
	int delay_fd = open(filename, O_RDONLY);
	if (delay_fd == -1){
		printf("Error: pps-client is not running.\n");
		return -1;
	}
	int rv = read(delay_fd, g.strbuf, 50);
	if (rv == -1){
		printf("getSysDelay() Unable to read %s\n", filename);
		return -1;
	}

	g.strbuf[50] = '\0';
	close(delay_fd);

	char *end = strchr(g.strbuf, '#');
	end[0] = '\0';
	sscanf(g.strbuf, "%d", sysDelay);
	return 0;
}

/**
 * Calculates the tolerance on an interrupt
 * event at the given probability from a
 * saved distribution of previous interrupt
 * events at constant delay.
 */
int calcTolerance(double probability, int idx){

	int fd = open(last_timer_distrib_file, O_RDONLY);
	if (fd == -1){
		strcpy(g.strbuf, "File not found: ");
		strcat(g.strbuf, last_timer_distrib_file);
		strcat(g.strbuf, "\n");
		printf("%s", g.strbuf);
		return -1;
	}

	int sz = FILEBUF_SZ;
	int len = INTRPT_DISTRIB_LEN;
	double tmp;

	int rv = read(fd, (void *)g.filebuf, sz);					// Read the sample distrib into g.filebuf
	close(fd);
	if (rv == -1){
		printf("calcTolerance() Unable to read %s\n", last_timer_distrib_file);
		return -1;
	}

	char *lines[len];
	double probDistrib[len];

	lines[0] = strtok(g.filebuf, "\r\n\0");				// Separate the file lines
	for (int i = 1; i < len; i++){
		lines[i] = strtok(NULL, "\r\n\0");
	}

	double sum = 0.0;									// Get the array values
	for (int i = 0; i < len; i++){
		sscanf(lines[i], "%lf %lf", &tmp, probDistrib + i);
		sum += probDistrib[i];
	}

	double norm = 1.0 / sum;							// Normalize to get a prob density

	for (int i = 0; i < len; i++){
		probDistrib[i] *= norm;
	}

	double tailProb = 1.0 - probability;
	double accumProb = 0.0;
	double minProb = 1.0;
	int minIdx = 0;

	while (accumProb < tailProb){						// Zero the probs whose sum is less than tailProb
		for (int i = 0; i < len; i++){					// in ascending order of probability.
			if (probDistrib[i] != 0.0 && probDistrib[i] < minProb){
				minProb = probDistrib[i];
				minIdx = i;
			}
		}
		accumProb += probDistrib[minIdx];
		if (accumProb < tailProb){
			probDistrib[minIdx] = 0.0;
		}
		minProb = 1.0;
	}

	int lowIdx = 0, maxIdx = 0, hiIdx = len - 1;
	double maxVal = 0.0;

	for (int i = 0; i < len; i++){
		if (probDistrib[i] > maxVal){
			maxVal = probDistrib[i];
			maxIdx = i;
		}
	}

	for (int i = 0; i < len; i++){						// Get the low side of the prob range as the
		if (probDistrib[i] > 0.0){						// index preceeding the first non-zero prob.
			lowIdx = i - 1;
			break;
		}
	}
	for (int i = len - 1; i >= 0; i--){					// Get the high side of the prob range as the
		if (probDistrib[i] > 0.0){						// index following the last non-zero prob.
			hiIdx = i + 1;
			break;
		}
	}

	int hiTol = hiIdx - maxIdx;
	int lowTol = maxIdx - lowIdx;

	g.tolerance[idx] = hiTol;							// Publish as a symmetric tolerance
	if (lowTol > hiTol){
		g.tolerance[idx] = lowTol;
	}

	return 0;
}

/**
 * Outputs the captured event time in date
 * format or as seconds since the Unix epoch
 * date Jan 1, 1970 and outputs the tolerance
 * on the event time the corresponds to the
 * probability requested on program startup.
 */
int outputSingeEventTime(int tm[], double prob, int idx){
	char timeStr[50];

	if (g.outFormat == 0){						// Print in date-time format
		strftime(timeStr, 50, timefmt, localtime((const time_t*)(&tm[0])));
		printf("%s.%06d ±0.%06d with probability %lg\n", timeStr, tm[1], g.tolerance[idx], prob);
	}
	else {										// Print as seconds
		double time = (double)tm[0] + 1e-6 * tm[1];
		printf("%lf ±0.%06d with probability %lg\n", time, g.tolerance[idx], prob);
	}

	return 0;
}

/**
 * Outputs the captured event time in date
 * format or as seconds since the Unix epoch
 * date Jan 1, 1970 and saves a distribution
 * of the fractional part of the second.
 */
int outputRepeatingEventTime(int tm[], int seq_num){
	char timeStr[50];

	buildInterruptDistrib(tm[1]);

	if (g.outFormat == 0){						// Print in date-time format
		strftime(timeStr, 50, timefmt, localtime((const time_t*)(&tm[0])));
		printf("%s.%06d\n", timeStr, tm[1]);
	}
	else {										// Print as seconds
		double time = (double)tm[0] + 1e-6 * tm[1];
		printf("%lf\n", time);
	}

	return 0;
}

/**
 * Sets a nanosleep() time delay equal to the time remaining
 * in the second from the time recorded as fracSec plus an
 * adjustment value of timeAt in microseconds. The purpose
 * of the delay is to put the program to sleep until just
 * before the time when a interrupt timing will be
 * delivered by the interrupt-timer device driver.
 *
 * @param[in] timeAt The adjustment value.
 *
 * @param[in] fracSec The fractional second part of
 * the system time.
 *
 * @returns The length of time to sleep.
 */
struct timespec setSyncDelay(int timeAt, int fracSec){

	struct timespec ts;

	int timerVal = USECS_PER_SEC + timeAt - fracSec;

	if (timerVal >= USECS_PER_SEC){
		ts.tv_sec = 1;
		ts.tv_nsec = (timerVal - USECS_PER_SEC) * 1000;
	}
	else if (timerVal < 0){
		ts.tv_sec = 0;
		ts.tv_nsec = (USECS_PER_SEC + timerVal) * 1000;
	}
	else {
		ts.tv_sec = 0;
		ts.tv_nsec = timerVal * 1000;
	}

	return ts;
}

int main(int argc, char *argv[]){

	int tm[2];
	int sysDelay;
	int outFormat = 0;
	bool singleEvent = false;
	bool argRecognized = false;
	bool showAllTols = false;
	bool noWait = false;
	double probability = 0.0;
	double probs[] = {0.9, 0.95, 0.99, 0.995, 0.999};

	struct sched_param param;								// Process must be run as
	param.sched_priority = 99;								// root to change priority.
	sched_setscheduler(0, SCHED_FIFO, &param);

	if (argc > 1){
		if (strcmp(argv[1], "load-driver") == 0){
			if (geteuid() != 0){
				printf("Requires superuser privileges. Please sudo this command.\n");
				return 0;
			}
			if (argv[2] == NULL){
				printf("GPIO number is a required second arg.\n");
				printf("Could not load driver.\n");
				return 0;
			}
			if (driver_load(argv[2]) == -1){
				printf("Could not load interrupt-timer driver. Exiting.\n");
				return 1;
			}

			printf("interrupt-timer: driver loaded\n");
			return 0;
		}
		if (strcmp(argv[1], "unload-driver") == 0){
			if (geteuid() != 0){
				printf("Requires superuser privileges. Please sudo this command.\n");
				return 0;
			}
			printf("interrupt-timer: driver unloading\n");
			driver_unload();
			return 0;
		}

		for (int i = 1; i < argc; i++){

			if (strcmp(argv[i], "-s") == 0){
				outFormat = 1;
				argRecognized = true;
				continue;
			}
			if (strcmp(argv[i], "-p") == 0){

				if (i == argc - 1){
					showAllTols = true;
				}
				else {
					sscanf(argv[i+1], "%lf", &probability);
					if (probability == 0){
						showAllTols = true;
					}
					if (probability > 0.999){
						probability = 0.999;
					}
				}

				singleEvent = true;
				argRecognized = true;
				continue;
			}
			if (strcmp(argv[i], "-n") == 0){
				noWait = true;
				argRecognized = true;
			}
		}

		if (argRecognized){
			goto start;
		}

		printf("Usage:\n");
		printf("  sudo interrupt-timer load-driver <gpio-number>\n");
		printf("where gpio-number is the GPIO of the pin on which\n");
		printf("the interrupt will be captured.\n\n");
		printf("After loading the driver, calling interrupt-timer\n");
		printf("causes it to wait for interrupts then output the\n");
		printf("date-time when each occurs with options set by the\n");
		printf("following command line args:\n");
		printf("  -s Outputs time in seconds since the Linux epoch.\n");
		printf("otherwise outputs in date format (default).\n");
		printf("  -n removes the default sleep masking on repetitive\n");
		printf("events in order to simulate single event distribution.\n");

		// The following is not yet practical because timer PD is still
		// too irregular. Expect this to improve as Linux kernel evolves
		// at wich point this code could be activated:

//		printf("  -p [probability] causes interrupt-timer to time\n");
//		printf("single events and output both the event time and a\n");
//		printf("time range where the requested probability is the\n");
//		printf("probability (<= 0.999) that the event time is within\n");
//		printf("that range. If a probability value is zero or missing,\n");
//		printf("a list of time ranges and probabilities is generated.\n\n");

		printf("The program will exit on ctrl-c or when no interrupts\n");
		printf("are received within 5 minutes. When done, unload the\n");
		printf("driver with,\n");
		printf("  sudo interrupt-timer unload-driver\n");

		return 0;
	}

start:
	if (geteuid() != 0){
		printf("Requires superuser privileges. Please sudo this command.\n");
		return 0;
	}

	memset(&g, 0, sizeof(struct interruptTimerGlobalVars));
	g.showAllTols = showAllTols;
	g.outFormat = outFormat;

	if (singleEvent){
		if (! g.showAllTols){
			if (calcTolerance(probability, 0) == -1){
				return 0;
			}
		}
		else {
			for (int i = 0; i < 5; i++){
				if (calcTolerance(probs[i], i) == -1){
					return 0;
				}
			}
		}
	}

	printf("%s\n", version);

	int intrpt_fd = open("/dev/interrupt-timer", O_RDONLY); // Open the interrupt-timer device driver.
	if (intrpt_fd == -1){
		printf("interrupt-timer: Driver is not loaded. Exiting.\n");
		return 1;
	}

	struct timespec ts2;

	struct timeval tv1;
	int wake = 0;
	int seq_num = 0;
	int start = START;

	for (;;){
		if (noWait == false && singleEvent == false && seq_num > start){
			nanosleep(&ts2, NULL);
		}

		int dvrv = read(intrpt_fd, (void *)tm, 2 * sizeof(int));  // Read the time the interrupt occurred.
		if (dvrv > 0){
			int rv = getSysDelay(&sysDelay);
			if (rv == -1){
				return 1;
			}
			tm[1] -= sysDelay;								// Time in microseconds corrected for system interrupt sysDelay.
			if (tm[1] < 0){									// If negative after correction, adjustment both
				tm[1] = 1000000 + tm[1];					// fractional second
				tm[0] -= 1;									// and whole second to make the fractional second positive.
			}

			if (singleEvent == false){

				rv = outputRepeatingEventTime(tm, seq_num);
				if (rv == -1){
					return 1;
				}
			}
			else {
				if (! g.showAllTols){
					rv = outputSingeEventTime(tm, probability, 0);
					if (rv == -1){
						return 1;
					}
				}
				else {
					for (int i = 0; i < 5; i++){
						rv = outputSingeEventTime(tm, probs[i], i);
						if (rv == -1){
							return 1;
						}
					}
					printf("\n");
				}
			}
		}
		else {
			printf("No interrupt: Driver timeout at 5 minutes.\n");
			break;
		}

		if (singleEvent == false){
			g.seconds += 1;
			if (g.seconds % SECS_PER_MIN == 0){
				if (g.minutes > 1){
					writeInterruptDistribFile();
				}
				g.minutes += 1;
			}
			if (g.seconds % SECS_PER_DAY == 0){
				g.days += 1;
			}

			if (noWait == false && seq_num >= start){
				wake = tm[1] - 150;				// Sleep until 150 usec before the next expected pulse time
				gettimeofday(&tv1, NULL);
				ts2 = setSyncDelay(wake, tv1.tv_usec);
			}
			seq_num += 1;
		}
	}
	close(intrpt_fd);

	return 0;
}


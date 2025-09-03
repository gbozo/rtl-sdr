/*
 * Copyright (C) 2014 by Kyle Keen <keenerd@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* a collection of user friendly tools
 * todo: use strtol for more flexible int parsing
 * */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#if defined __linux__
#include <limits.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif
#include <unistd.h>
#else
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#define _USE_MATH_DEFINES
#endif

#include <math.h>

#include "rtl-sdr.h"

double atofs(char *s)
/* standard suffixes */
{
	char last;
	int len;
	double suff = 1.0;
	len = strlen(s);
	last = s[len-1];
	s[len-1] = '\0';
	switch (last) {
		case 'g':
		case 'G':
			suff *= 1e3;
			/* fall-through */
		case 'm':
		case 'M':
			suff *= 1e3;
			/* fall-through */
		case 'k':
		case 'K':
			suff *= 1e3;
			suff *= atof(s);
			s[len-1] = last;
			return suff;
	}
	s[len-1] = last;
	return atof(s);
}

double atoft(char *s)
/* time suffixes, returns seconds */
{
	char last;
	int len;
	double suff = 1.0;
	len = strlen(s);
	last = s[len-1];
	s[len-1] = '\0';
	switch (last) {
		case 'h':
		case 'H':
			suff *= 60;
			/* fall-through */
		case 'm':
		case 'M':
			suff *= 60;
			/* fall-through */
		case 's':
		case 'S':
			suff *= atof(s);
			s[len-1] = last;
			return suff;
	}
	s[len-1] = last;
	return atof(s);
}

double atofp(char *s)
/* percent suffixes */
{
	char last;
	int len;
	double suff = 1.0;
	len = strlen(s);
	last = s[len-1];
	s[len-1] = '\0';
	switch (last) {
		case '%':
			suff *= 0.01;
			suff *= atof(s);
			s[len-1] = last;
			return suff;
	}
	s[len-1] = last;
	return atof(s);
}

int nearest_gain(rtlsdr_dev_t *dev, int target_gain)
{
	int i, r, err1, err2, count, nearest;
	int* gains;
	r = rtlsdr_set_tuner_gain_mode(dev, 1);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to enable manual gain.\n");
		return r;
	}
	count = rtlsdr_get_tuner_gains(dev, NULL);
	if (count <= 0) {
		return 0;
	}
	gains = malloc(sizeof(int) * count);
	count = rtlsdr_get_tuner_gains(dev, gains);
	nearest = gains[0];
	for (i=0; i<count; i++) {
		err1 = abs(target_gain - nearest);
		err2 = abs(target_gain - gains[i]);
		if (err2 < err1) {
			nearest = gains[i];
		}
	}
	free(gains);
	return nearest;
}

int verbose_set_frequency(rtlsdr_dev_t *dev, uint32_t frequency)
{
	int r;
	r = rtlsdr_set_center_freq(dev, frequency);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set center freq.\n");
	} else {
		fprintf(stderr, "Tuned to %u Hz.\n", frequency);
	}
	return r;
}

int verbose_set_sample_rate(rtlsdr_dev_t *dev, uint32_t samp_rate)
{
	int r;
	r = rtlsdr_set_sample_rate(dev, samp_rate);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");
	} else {
		fprintf(stderr, "Sampling at %u S/s.\n", samp_rate);
	}
	return r;
}

int verbose_direct_sampling(rtlsdr_dev_t *dev, int on)
{
	int r;
	r = rtlsdr_set_direct_sampling(dev, on);
	if (r != 0) {
		fprintf(stderr, "WARNING: Failed to set direct sampling mode.\n");
		return r;
	}
	if (on == 0) {
		fprintf(stderr, "Direct sampling mode disabled.\n");}
	if (on == 1) {
		fprintf(stderr, "Enabled direct sampling mode, input 1/I.\n");}
	if (on == 2) {
		fprintf(stderr, "Enabled direct sampling mode, input 2/Q.\n");}
	return r;
}

int verbose_offset_tuning(rtlsdr_dev_t *dev)
{
	int r;
	r = rtlsdr_set_offset_tuning(dev, 1);
	if (r != 0) {
		fprintf(stderr, "WARNING: Failed to set offset tuning.\n");
	} else {
		fprintf(stderr, "Offset tuning mode enabled.\n");
	}
	return r;
}

int verbose_auto_gain(rtlsdr_dev_t *dev)
{
	int r;
	r = rtlsdr_set_tuner_gain_mode(dev, 0);
	if (r != 0) {
		fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
	} else {
		fprintf(stderr, "Tuner gain set to automatic.\n");
	}
	return r;
}

int verbose_gain_set(rtlsdr_dev_t *dev, int gain)
{
	int r;
	r = rtlsdr_set_tuner_gain_mode(dev, 1);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to enable manual gain.\n");
		return r;
	}
	r = rtlsdr_set_tuner_gain(dev, gain);
	if (r != 0) {
		fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
	} else {
		fprintf(stderr, "Tuner gain set to %0.2f dB.\n", gain/10.0);
	}
	return r;
}

int verbose_ppm_set(rtlsdr_dev_t *dev, int ppm_error)
{
	int r;
	if (ppm_error == 0) {
		return 0;}
	r = rtlsdr_set_freq_correction(dev, ppm_error);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set ppm error.\n");
	} else {
		fprintf(stderr, "Tuner error set to %i ppm.\n", ppm_error);
	}
	return r;
}

int verbose_reset_buffer(rtlsdr_dev_t *dev)
{
	int r;
	r = rtlsdr_reset_buffer(dev);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to reset buffers.\n");}
	return r;
}

#if defined __linux__
static int read_uint8_from_file(const char *path, uint8_t *value)
{
	FILE *f;
	int r, v;
	f = fopen(path, "r");
	if (!f)
		return -1;
	r = fscanf(f, "%d", &v);
	fclose(f);
	if (r != 1) {
		return -1;
	}
	if (v < 0 || v > 255) {
		return -1;
	}
	*value = (uint8_t) v;
	return 0;
}


static int get_usb_bus_address_by_path(const char *device_path,
				       uint8_t *bus_number,
				       uint8_t *device_address)
{
	struct stat st;
	int r;
	int maj, min;
	ssize_t ssz;
	char p1[PATH_MAX], p2[PATH_MAX];
	char *ptr = NULL;
	uint8_t busnum, devnum;

	/* device_path should be a character-special device */
	r = stat(device_path, &st);
	if (r || !S_ISCHR(st.st_mode))
		return -1;
	maj = major(st.st_rdev);
	min = minor(st.st_rdev);

	/* Check that it's a USB device /sys/bus/usb */
	r = snprintf(p1, sizeof(p1), "/sys/dev/char/%d:%d/subsystem", maj, min);
	if (r < 0 || r >= (int)sizeof(p1))
		return -1;
	ptr = realpath(p1, p2);
	if (!ptr)
		return -1;
	if (strcmp("/sys/bus/usb", p2))
		return -1;

	/* Read the bus number */
	r = snprintf(p1, sizeof(p1), "/sys/dev/char/%d:%d/busnum", maj, min);
	if (r < 0 || r >= (int)sizeof(p1))
		return -1;
	r = read_uint8_from_file(p1, &busnum);
	if (r < 0)
		return -1;

	/* Read the device number */
	r = snprintf(p1, sizeof(p1), "/sys/dev/char/%d:%d/devnum", maj, min);
	if (r < 0 || r >= (int)sizeof(p1))
		return -1;
	r = read_uint8_from_file(p1, &devnum);
	if (r < 0)
		return -1;

	*bus_number = busnum;
	*device_address = devnum;
	return 0;
}

static int get_index_by_device_path(const char *device_path)
{
	int r, index;
	uint8_t busnum, devnum;
	r = get_usb_bus_address_by_path(device_path, &busnum, &devnum);
	if (r < 0)
		return -1;
	return rtlsdr_get_index_by_device_address(busnum, devnum);
}
#endif /* defined __linux__ */

int verbose_device_search(char *s)
{
	int i, r, device_count, device, offset;
	char *s2;
	char vendor[256], product[256], serial[256];
	device_count = rtlsdr_get_device_count();
	if (!device_count) {
		fprintf(stderr, "No supported devices found.\n");
		return -1;
	}
	fprintf(stderr, "Found %d device(s):\n", device_count);
	for (i = 0; i < device_count; i++) {
		r = rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		if (r < 0) {
			/* Couldn't get device strings; Use generic device name */
			fprintf(stderr, "  %d:  (%s)\n", i, rtlsdr_get_device_name(i));
			continue;
		}
		fprintf(stderr, "  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
	}
	fprintf(stderr, "\n");
#if defined(__linux__)
	/* does the string look like a device node */
	if (s[0] == '/') {
		fprintf(stderr, "Want device: %s\n", s);
		device = get_index_by_device_path(s);
		if (device >= 0) {
			fprintf(stderr, "Using device %d: %s\n",
				device, rtlsdr_get_device_name((uint32_t)device));
			return device;
		}
	}
#endif
	/* does string look like raw id number */
	device = (int)strtol(s, &s2, 0);
	if (s2[0] == '\0' && device >= 0 && device < device_count) {
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	/* does string exact match a serial */
	for (i = 0; i < device_count; i++) {
		r = rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		if (r < 0) {
			continue;}
		if (strcmp(s, serial) != 0) {
			continue;}
		device = i;
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	/* does string prefix match a serial */
	for (i = 0; i < device_count; i++) {
		r = rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		if (r < 0) {
			continue;}
		if (strncmp(s, serial, strlen(s)) != 0) {
			continue;}
		device = i;
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	/* does string suffix match a serial */
	for (i = 0; i < device_count; i++) {
		r = rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		if (r < 0) {
			continue;}
		offset = strlen(serial) - strlen(s);
		if (offset < 0) {
			continue;}
		if (strncmp(s, serial+offset, strlen(s)) != 0) {
			continue;}
		device = i;
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	fprintf(stderr, "No matching devices found.\n");
	return -1;
}

// vim: tabstop=8:softtabstop=8:shiftwidth=8:noexpandtab

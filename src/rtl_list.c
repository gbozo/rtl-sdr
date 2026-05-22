/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 *
 * rtl_list, device listing utility for RTL-SDR devices
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <getopt.h>
#else
#include <windows.h>
#include "getopt/getopt.h"
#endif

#include "rtl-sdr.h"

static void usage(void)
{
	fprintf(stderr,
		"rtl_list, a tool for listing connected RTL-SDR devices\n\n"
		"Usage:\trtl_list [-j] [-p] [-h]\n\n"
		"Options:\n"
		"\t-j, --json\tOutput as JSON\n"
		"\t-p, --pretty\tPretty-print JSON output\n"
		"\t-h, --help\tShow this help\n");
	exit(1);
}

struct json_state {
	int first;
	int pretty;
};

struct count_ctx {
	int count;
};

static int count_cb(int index, const char *name,
		    const char *manufact, const char *product,
		    const char *serial, void *ctx)
{
	(void)index;
	(void)name;
	(void)manufact;
	(void)product;
	(void)serial;
	((struct count_ctx *)ctx)->count++;
	return 0;
}

static int text_cb(int index, const char *name,
		   const char *manufact, const char *product,
		   const char *serial, void *ctx)
{
	(void)ctx;
	printf("  %d:  %s", index, name ? name : "unknown");
	if (serial)
		printf(", SN: %s", serial);
	printf("\n");
	if (manufact)
		printf("       Manufacturer: %s\n", manufact);
	if (product)
		printf("       Product: %s\n", product);
	if (serial)
		printf("       Serial: %s\n", serial);
	printf("\n");
	return 0;
}

static void json_field(FILE *f, const char *label, const char *val,
		       int *comma, int pretty, int indent)
{
	if (*comma)
		fprintf(f, ",");
	*comma = 1;
	if (pretty)
		fprintf(f, "\n%*s", indent, "");
	fprintf(f, "\"%s\": ", label);
	if (val)
		fprintf(f, "\"%s\"", val);
	else
		fprintf(f, "null");
}

static void json_index(FILE *f, int val, int *comma, int pretty, int indent)
{
	if (*comma)
		fprintf(f, ",");
	*comma = 1;
	if (pretty)
		fprintf(f, "\n%*s", indent, "");
	fprintf(f, "\"index\": %d", val);
}

static int json_cb(int index, const char *name,
		   const char *manufact, const char *product,
		   const char *serial, void *ctx)
{
	struct json_state *js = (struct json_state *)ctx;
	int comma = 0;

	if (!js->first)
		printf(",");
	js->first = 0;

	if (js->pretty) {
		printf("\n    {");
		json_index(stdout, index, &comma, js->pretty, 6);
		json_field(stdout, "name", name, &comma, js->pretty, 6);
		json_field(stdout, "manufacturer", manufact, &comma, js->pretty, 6);
		json_field(stdout, "product", product, &comma, js->pretty, 6);
		json_field(stdout, "serial", serial, &comma, js->pretty, 6);
		printf("\n    }");
	} else {
		printf("{");
		fprintf(stdout, "\"index\":%d", index);
		comma = 1;
		json_field(stdout, "name", name, &comma, 0, 0);
		json_field(stdout, "manufacturer", manufact, &comma, 0, 0);
		json_field(stdout, "product", product, &comma, 0, 0);
		json_field(stdout, "serial", serial, &comma, 0, 0);
		printf("}");
	}
	return 0;
}

int main(int argc, char **argv)
{
	int json_mode = 0;
	int pretty = 0;
	int count;
	int opt;

	static struct option long_options[] = {
		{"json",   no_argument, NULL, 'j'},
		{"pretty", no_argument, NULL, 'p'},
		{"help",   no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "jph", long_options, NULL)) != -1) {
		switch (opt) {
		case 'j':
			json_mode = 1;
			break;
		case 'p':
			pretty = 1;
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}

	if (json_mode) {
		struct json_state js;
		js.first = 1;
		js.pretty = pretty;

		if (pretty)
			printf("{\n  \"devices\": [");
		else
			printf("{\"devices\":[");

		count = rtlsdr_device_enumerate(json_cb, &js);

		if (pretty)
			printf("\n  ]\n}\n");
		else
			printf("]}\n");
	} else {
		struct count_ctx cnt;
		cnt.count = 0;
		rtlsdr_device_enumerate(count_cb, &cnt);

		if (cnt.count > 0) {
			printf("Found %d device(s):\n\n", cnt.count);
			rtlsdr_device_enumerate(text_cb, NULL);
		} else {
			printf("No supported devices found.\n");
		}
	}

	return 0;
}

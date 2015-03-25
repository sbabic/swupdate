/*
 * (C) Copyright 2012
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <errno.h>
#include "util.h"

#define GPIO_PATH		"/sys/class/gpio/"
#define GPIO_EXPORT_PATH	"/sys/class/gpio/export"
#define GPIO_UNEXPORT_PATH	"/sys/class/gpio/unexport"

static int gpio_export_unexport(int gpio, int export)
{
	int fd, ret;
	const char *fname;
	char gpio_number[10];

	if (export)
		fname = GPIO_EXPORT_PATH;
	else
		fname = GPIO_UNEXPORT_PATH;

	fd = open(fname, O_WRONLY);
	if (!fd) {
		TRACE("I cannot open %s to set GPIO\n", fname);
		return -1;
	}

	snprintf(gpio_number, sizeof(gpio_number) - 2, "%d\n", gpio);

	ret = write(fd, gpio_number, strlen(gpio_number));
	close(fd);

	if (ret != strlen(gpio_number))
		return -1;

	return 0;
}

static int gpio_direction(int gpio_number, int out)
{
	int fd, ret;
	char fname[50];
	char buf[8];

	memset(fname, 0, sizeof(fname));
	memset(buf, 0, sizeof(buf));
	snprintf(fname, sizeof(fname),
		"%sgpio%d/direction", GPIO_PATH, gpio_number);

	fd = open(fname, O_WRONLY);
	if (!fd) {
		TRACE("I cannot open %s to set GPIO\n", fname);
		return -1;
	}

	if (out)
		strcpy(buf, "out");
	else
		strcpy(buf, "in");

	ret = write(fd,buf, strlen(buf));
	close(fd);

	if (ret != strlen(buf)) {
		TRACE("Error setting direction gpio %d %s %d\n",
			gpio_number, out ? "out" : "in", ret);
		return -1;
	}

	return 0;
}

int gpio_set_value(int gpio_number, int value)
{
	int fd, ret;
	char fname[50];
	char buf[8];

	memset(fname, 0, sizeof(fname));
	memset(buf, 0, sizeof(buf));
	snprintf(fname, sizeof(fname),
		"%sgpio%d/value", GPIO_PATH, gpio_number);

	fd = open(fname, O_RDWR);
	if (!fd) {
		TRACE("I cannot open %s to set GPIO\n", fname);
		return -1;
	}

	if (value)
		strcpy(buf, "1");
	else
		strcpy(buf, "0");

	ret = write(fd,buf, strlen(buf));
	close(fd);

	if (ret != strlen(buf)) {
		TRACE("Error setting value gpio %d %d\n",
			gpio_number, value);
		return -1;
	}

	return 0;
}

int gpio_get_value(int gpio_number)
{
	int fd, ret;
	char fname[50];
	char buf[8];

	memset(fname, 0, sizeof(fname));
	memset(buf, 0, sizeof(buf));
	snprintf(fname, sizeof(fname),
		"%sgpio%d/value", GPIO_PATH, gpio_number);

	fd = open(fname, O_RDONLY);
	if (!fd) {
		TRACE("I cannot open %s to get GPIO\n", fname);
		return -1;
	}

	ret = read(fd, buf, 1);
	close(fd);

	if (ret < 0) 
		return -ENODEV;

	if (buf[0] == '1')
		return 1;

	return 0;
}

int gpio_direction_input(int gpio_number)
{
	return gpio_direction(gpio_number, 0);
}

int gpio_direction_output(int gpio_number, int value)
{
	int ret;

	ret = gpio_direction(gpio_number, 1);
	if (ret < 0)
		return ret;

	return gpio_set_value(gpio_number, value);
}

int gpio_export(int gpio)
{
	return gpio_export_unexport(gpio, 1);
}

int gpio_unexport(int gpio)
{
	return gpio_export_unexport(gpio, 0);
}

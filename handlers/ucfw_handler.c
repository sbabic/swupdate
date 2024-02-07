/*
 * (C) Copyright 2018
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This handler allows SWUpdate to update the firmware on a microcontroller
 * connected to the main controller via UART.
 * Parameters for setup are passed via
 * sw-description file. Its behavior can be extended to be
 * more general.
 * The protocol is ASCII based. There is a sequence to be done
 * to put the microcontroller in programming mode. After that
 * the handler sends the data and waits for an ACK from the
 * microcontroller.
 *
 * The programming of the firmware shall be:
 * 1. Enter firmware update mode (bootloader)
 * 	1. Set "reset line" to logical "low"
 * 	2. Set "update line" to logical "low"
 * 	3. Set "reset line" to logical "high"
 * 2. Send programming message $PROG;<<CS>><CR><LF> to the microcontroller.
 * (microcontroller will remain in programming state)
 * 3. microcontroller confirms with $READY;<<CS>><CR><LF>
 * 4. Data transmissions package based from mainboard to microcontroller
 * package definition:
 *   - within a package the records are sent one after another without the end of line marker <CR><LF>
 *   - the package is completed with <CR><LF>
 *   5. The microcontroller requests the next package with $READY;<<CS>><CR><LF>
 *   6. Repeat step 4 and 5 until the complete firmware is transmitted.
 *   7. The keypad confirms the firmware completion with $COMPLETED;<<CS>><CR><LF>
 *   8. Leave firmware update mode
 *   	1. Set "Update line" to logical "high"
 *   	2. Perform a reset over the "reset line"
 *
 * <<CS>> : checksum. The checksum is calculated as the two's complement of
 * the modulo-256 sum over all bytes of the message
 * string except for the start marker "$".
 *
 * The handler expects to get in the properties the setup for the reset
 * and prog gpios. They should be in this format:
 *
 * properties = {
 * 	reset = "<gpiodevice>:<gpionumber>:<activelow>";
 * 	prog = "<gpiodevice>:<gpionumber>:<activelow>";
 * }
 *
 * Example:
 *
 * properties = {
 *	reset =  "/dev/gpiochip0:38:false";
 *      prog =  "/dev/gpiochip0:39:false";
 * }
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <termios.h>
#include <gpiod.h>

#include "swupdate_image.h"
#include "handler.h"
#include "util.h"

#define MODE_PROG	0
#define MODE_NORMAL 	1

#define RESET_CONSUMER	"swupdate-uc-handler"
#define PROG_CONSUMER	RESET_CONSUMER
#define DEFAULT_TIMEOUT 2

/*
 * Use GPIOD_LINE_BULK_MAX_LINES in order to determine,
 * whether this is compiled using libgpio v1 or v2.
 */
#ifdef GPIOD_LINE_BULK_MAX_LINES
#define USE_GPIOD_API_V1
#endif

void ucfw_handler(void);

/*
 * struct for properties how to set
 * GPIOs to put the microcontroller in
 * programmming mode.
 */
struct mode_setup {
	char gpiodev[SWUPDATE_GENERAL_STRING_SIZE];
	unsigned int offset;
	bool active_low;
#ifdef USE_GPIOD_API_V1
	struct gpiod_chip *chip;
	struct gpiod_line *line;
#else
	struct gpiod_line_request *request;
#endif
};

enum {
	DEVGPIO,
	GPIONUM,
	ACTIVELOW
};

struct handler_priv {
	struct mode_setup reset;
	struct mode_setup prog;
	int fduart;
	bool debug;
	unsigned int timeout;
	char buf[1024];	/* enough for 3 records */
	unsigned int nbytes;
};

#ifdef USE_GPIOD_API_V1
static void free_gpios(struct handler_priv *priv) {
	if (priv->prog.chip && (priv->prog.chip != priv->reset.chip)){
		gpiod_chip_close(priv->prog.chip);
		priv->reset.chip = NULL;
	}
	if (priv->reset.chip) {
		gpiod_chip_close(priv->reset.chip);
		priv->reset.chip = NULL;
	}
}

static int register_gpios(struct handler_priv *priv){
	int ret = 0;
	int status;

	priv->reset.chip = gpiod_chip_open(priv->reset.gpiodev);
	if (strcmp(priv->reset.gpiodev, priv->prog.gpiodev))
		priv->prog.chip = gpiod_chip_open(priv->prog.gpiodev);
	else
		priv->prog.chip = priv->reset.chip;

	if (!priv->reset.chip || !priv->prog.chip) {
		ERROR("Cannot open gpio driver");
		ret  =-ENODEV;
		goto freegpios;
	}

	priv->reset.line = gpiod_chip_get_line(priv->reset.chip, priv->reset.offset);
	priv->prog.line = gpiod_chip_get_line(priv->prog.chip, priv->prog.offset);

	if (!priv->reset.line || !priv->prog.line) {
		ERROR("Cannot get requested GPIOs: %d on %s and %d on %s",
			priv->reset.offset, priv->reset.gpiodev,
			priv->prog.offset, priv->prog.gpiodev);
		ret  =-ENODEV;
		goto freegpios;
	}

	status = gpiod_line_request_output(priv->reset.line, RESET_CONSUMER, 0);
	if (status) {
		ret  =-ENODEV;
		ERROR("Cannot request reset line");
		goto freegpios;
	}
	status = gpiod_line_request_output(priv->prog.line, PROG_CONSUMER, 0);
	if (status) {
		ret  =-ENODEV;
		ERROR("Cannot request prog line");
		goto freegpios;
	}

	return ret;
freegpios:
	free_gpios(priv);
	return ret;
}

static int switch_mode(struct handler_priv *priv, int mode)
{
	int ret = 0;
	if (!priv->reset.line || !priv->prog.line) return -ENODEV;

	/*
	 * A reset is always done
	 */
	ret = gpiod_line_set_value(priv->reset.line, 0);
	if (ret){
		ERROR("unable to set reset to 0");
		return ret;
	}

	/* Set programming mode */
	ret = gpiod_line_set_value(priv->prog.line, mode);
	if (ret){
		ERROR("unable to set prog to %i",mode);
		return ret;
	}

	usleep(20000);

	/* Remove reset */
	ret = gpiod_line_set_value(priv->reset.line, 1);
	if (ret){
		ERROR("unable to set reset to 1");
		return ret;
	}

	usleep(20000);

	return ret;
}

#else
/* Implementation for LIBGPIOD V2 api*/

static void free_gpios(struct handler_priv *priv) {
	if(priv->reset.request){
		gpiod_line_request_release(priv->reset.request);
		priv->reset.request = NULL;
	}
	if(priv->prog.request){
		gpiod_line_request_release(priv->prog.request);
		priv->prog.request = NULL;
	}
}

static int register_gpios(struct handler_priv *priv) {
	struct gpiod_line_settings *settings;
	struct gpiod_request_config *req_cfg;
	struct gpiod_line_config *reset_cfg, *prog_cfg;
	struct gpiod_chip *chip;
	int ret = 0;

	settings = gpiod_line_settings_new();
	if (!settings) {
		ERROR("Unable to allocate line settings");
		return -ENODEV;
	}
	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);

	req_cfg = gpiod_request_config_new();
	if (!req_cfg) {
		ERROR("Unable to allocate the request config structure");
		ret  =-ENODEV;
		goto freesettings;
	}
	gpiod_request_config_set_consumer(req_cfg, RESET_CONSUMER);

	reset_cfg = gpiod_line_config_new();
	if (!reset_cfg) {
		ERROR("Unable to allocate the reset line config structure");
		ret  =-ENODEV;
		goto freerequestconfig;
	}
	ret = gpiod_line_config_add_line_settings(reset_cfg, &priv->reset.offset, 1, settings);
	if (ret) {
		ERROR("Unable to add reset line settings");
		goto freeresetconfig;
	}

	prog_cfg = gpiod_line_config_new();
	if (!prog_cfg) {
		ERROR("Unable to allocate the prog line config structure");
		ret  =-ENODEV;
		goto freeresetconfig;
	}

	ret = gpiod_line_config_add_line_settings(prog_cfg, &priv->prog.offset, 1, settings);
	if (ret) {
		ERROR("Unable to add reset line settings");
		goto freeprogconfig;
	}

	TRACE("Request lines for reset");
	chip = gpiod_chip_open(priv->reset.gpiodev);
	if (!chip) {
		ERROR("Unable to open chip '%s'",priv->reset.gpiodev);
		ret  =-ENODEV;
		goto freeprogconfig;
	}

	priv->reset.request = gpiod_chip_request_lines(chip, req_cfg, reset_cfg);
	gpiod_chip_close(chip);
	if (!priv->reset.request) {
		ERROR("Unable to request lines on chip '%s'", priv->reset.gpiodev);
		goto freeprogconfig;
	}

	TRACE("Request lines for prog");
	chip = gpiod_chip_open(priv->prog.gpiodev);
	if (!chip) {
		ERROR("Unable to open chip '%s'", priv->prog.gpiodev);
		ret  =-ENODEV;
		goto freelinerequest;
	}

	priv->prog.request = gpiod_chip_request_lines(chip, req_cfg, prog_cfg);
	gpiod_chip_close(chip);
	if (!priv->prog.request) {
		ERROR("unable to request lines on chip '%s'", priv->prog.gpiodev);
		ret  =-ENODEV;
	}

	goto freeprogconfig; // clean up everything except for the gpiod_line_requests

freelinerequest:
	gpiod_line_request_release(priv->reset.request);
	priv->reset.request = NULL;
freeprogconfig:
	gpiod_line_config_free(prog_cfg);
freeresetconfig:
	gpiod_line_config_free(reset_cfg);
freerequestconfig:
	gpiod_request_config_free(req_cfg);
freesettings:
	gpiod_line_settings_free(settings);
	return ret;
}


static int switch_mode(struct handler_priv *priv, int mode)
{

	int ret = 0;

	/*
	 * A reset is always done
	 */
	ret = gpiod_line_request_set_value(priv->reset.request, priv->reset.offset, 0);
	if (ret){
		ERROR("Unable to set reset to 0");
		return ret;
	}

	/* Set programming mode */
	ret = gpiod_line_request_set_value(priv->prog.request, priv->prog.offset, mode);
	if (ret){
		ERROR("Unable to set prog to %i", mode);
		return ret;
	}

	usleep(20000);

	/* Remove reset */
	ret = gpiod_line_request_set_value(priv->reset.request, priv->reset.offset, 1);
	if (ret){
		ERROR("Unable to set reset to 1");
		return ret;
	}

	usleep(20000);

	return ret;
}
#endif

static bool verify_chksum(char *buf, unsigned int *size)
{
	int i;
	uint16_t chksum = 0;
	int len = *size;

	if (len < 3)
		return false;
	while (buf[len - 1] == '\r' || (buf[len - 1] == '\n'))
			len--;
	chksum = from_ascii(&buf[len - 2], 2, LG_16);
	len -= 2;
	for (i = 1; i < len; i++)
		chksum += buf[i];

	chksum &= 0xff;

	if (chksum)
		ERROR("Wrong checksum received: %x", chksum);

	/*
	 * Drop checksum after verification
	 */
	buf[len] = '\0';

	*size = len;

	return (chksum == 0);
}

/*
 * Compute checksum as two's complement
 * excluding prefix $ and add CR/LF at the end
 */

static int insert_chksum(char *buf, unsigned int len)
{
	uint16_t chksum = 0;
	unsigned int i;

	/* Skip first char */
	for (i = 1; i < len; i++)
		chksum += buf[i];
	chksum = (~chksum + 1);
	sprintf(&buf[len], "%02X", chksum & 0xFF);
	len += 2;

	buf[len++] = '\r';
	buf[len++] = '\n';

	return len;
}

static int set_uart (int fd)
{
	struct termios tty;

	if (tcgetattr (fd, &tty) < 0) {
		printf ("Error from tcgetattr: %s\n", strerror (errno));
		return -1;
	}

	 cfsetospeed (&tty, (speed_t) B115200);
	 cfsetispeed (&tty, (speed_t) B115200);

	 tty.c_cflag |= (CLOCAL | CREAD);	/* ignore modem controls */
	 tty.c_cflag &= ~CSIZE;
	 tty.c_cflag |= CS8;		/* 8-bit characters */
	 tty.c_cflag &= ~PARENB;	/* no parity bit */
	 tty.c_cflag &= ~CSTOPB;	/* only need 1 stop bit */
	 tty.c_cflag &= ~CRTSCTS;	/* no hardware flowcontrol */

	 /* setup for non-canonical mode */
	 tty.c_iflag &=
	   ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	 tty.c_iflag |= (IGNBRK | IGNPAR);
	 tty.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | ICANON | ISIG | IEXTEN);
	 tty.c_oflag &= ~(OPOST | ONLCR);

	 /* fetch bytes as they become available */
	 tty.c_cc[VMIN] = 1;
	 tty.c_cc[VTIME] = 1;

	 if (tcsetattr (fd, TCSANOW, &tty) != 0) {
	     printf ("Error from tcsetattr: %s\n", strerror (errno));
	     return -1;
	 }

	tcflush(fd, TCIFLUSH);
	tcflush(fd, TCOFLUSH);

	return 0;
}

/*
 * This is just for debug purposes
 */
static void dump_ascii(bool rxdir, char *buf, int count)
{
	int i;
	char *outbuf = (char *)malloc(count + 40);
	char *tmp = outbuf;
	int len;

	if (!outbuf)
		return;

	len = sprintf(tmp, "%cX: %d bytes:",
			rxdir ? 'R' : 'T',
			count);
	tmp += len;
	for (i=0; i < count; i++) {
		sprintf(tmp, "%c", buf[i]);
		tmp++;
	}

	TRACE("%s", outbuf);

	free(outbuf);
}

static int receive_msg(int fd, char *rx, size_t size,
			unsigned int timeout, bool debug)
{
	fd_set fds;
	struct timeval tv;
	int ret;
	unsigned int count;

	/* Initialize structures for select */
	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	/*
	 * Microcontrolle answers very fast,
	 * Timeout is just to take care if no answer is
	 * sent
	 */
	tv.tv_sec = timeout;
	tv.tv_usec = 0; /* Check for not more as 10 mSec */

	ret = select(fd + 1, &fds, NULL, NULL, &tv);
	if (ret == 0) {
		ERROR("Timeout, no answer from microcontroller");
		return -EPROTO;
	}

	ret = read(fd, rx, size);
	if (ret < 3) {
		ERROR("Error in read: %d", ret);
		return -EBADMSG;
	}
	count = ret;

	if (debug)
		dump_ascii(true, rx, count);

	/*
	 * Try some syntax check
	 */
	if (rx[0] != '$') {
		ERROR("First byte is not '$' but '%c'", rx[0]);
		return -EBADMSG;
	}

	if (!verify_chksum(rx, &count)) {
		return -EBADMSG;
	}

	return 0;
}

static int write_data(int fd, char *buf, size_t size)
{
	size_t written;
	written = write(fd, buf, size);
	if (written != size) {
		ERROR("Error in write %zu", written);
		return -EFAULT;
	}

	return 0;
}

static int write_msg(int fd, const char *msg)
{
	int len, ret;
	char *buf;

	buf = strdup(msg);

	len = insert_chksum(buf, strlen(buf));
	ret = write_data(fd, buf, len);
	free(buf);
	return ret;
}

static int prepare_update(struct handler_priv *priv, 
			  struct img_type *img)
{
	int ret;
	char msg[128];
	int len;

	ret = register_gpios(priv);
	if (ret < 0) {
		return -ENODEV;
	}

	ret = switch_mode(priv, MODE_PROG);
	if (ret < 0) {
		return -ENODEV;
	}

	DEBUG("Using %s", img->device);

	priv->fduart = open(img->device, O_RDWR);

	if (priv->fduart < 0) {
		ERROR("Cannot open UART %s", img->device);
		return -ENODEV;
	}

	set_uart(priv->fduart);

	/* No FW data to be sent */
	priv->nbytes = 0;

	write_msg(priv->fduart, "$PROG;");

	len = receive_msg(priv->fduart, msg, sizeof(msg), priv->timeout, priv->debug);
	if (len < 0 || strcmp(msg, "$READY;"))
		return -EBADMSG;

	return 0;
}

static int update_fw(void *data, const void *buffer, size_t size)
{
	int cnt = 0;
	char c;
	int ret;
	char msg[80];
	struct handler_priv *priv = (struct handler_priv *)data;
	const char *buf = (const char *)buffer;

	while (size > 0) {
		c = buf[cnt++];
		priv->buf[priv->nbytes++] = c;
		size--;
		if (c == '\n') {
			/* Send data */
			if (priv->debug)
				dump_ascii(false, priv->buf, priv->nbytes);
			ret = write_data(priv->fduart, priv->buf, priv->nbytes);
			if (ret < 0)
				return ret;
			priv->buf[priv->nbytes] = '\0';
			msg[0] = '\0';
			ret = receive_msg(priv->fduart, msg, sizeof(msg),
					  priv->timeout, priv->debug);
			priv->nbytes = 0;
			if (ret < 0) {
				return ret;
			}
			if (!strcmp(msg, "$READY;"))
				continue;
			if (!strcmp(msg, "$COMPLETED;"))
				return 0;
		}
	}
	return 0;
}

static int finish_update(struct handler_priv *priv)
{
	int ret;

	close(priv->fduart);
	ret = switch_mode(priv, MODE_NORMAL);
	free_gpios(priv);
	if (ret < 0) {
		return -ENODEV;
	}
	return 0;
}

static int get_gpio_from_property(struct dict_list *prop, struct mode_setup *gpio)
{
	struct dict_list_elem *entry;
	int i;

	memset(gpio, 0, sizeof(*gpio));

	LIST_FOREACH(entry, prop, next) {
		char *sstore = strdup(entry->value);
		char *s = sstore;
		for (i = 0; i < 3; i++) {
			char *t = strchr(s, ':');

			if (t) *t = '\0';
			switch (i) {
			case 0:
				strlcpy(gpio->gpiodev, s, sizeof(gpio->gpiodev));
				break;
			case 1:
				errno = 0;
				gpio->offset = strtoul(s,  NULL, 10);
				if (errno == EINVAL)
					return -EINVAL;
				break;
			case 2:
				gpio->active_low = strtobool(s);
				break;
			}

			if (!t)
				break;
			s = ++t;
		}
		free(sstore);
	}

	return 0;
}

static int install_uc_firmware_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct handler_priv hnd_data;
	struct dict_list *properties;
	struct dict_list_elem *entry;
	unsigned int cnt;
	int ret = 0;
	struct mode_setup *gpio;

	memset(&hnd_data, 0, sizeof(hnd_data));
	hnd_data.timeout = DEFAULT_TIMEOUT;

	const char *properties_list[] = { "reset", "prog"};

	for (cnt = 0; cnt < ARRAY_SIZE(properties_list); cnt++) {
		/*
		 * Getting GPIOs from sw-description
		 */
		properties = dict_get_list(&img->properties,
					   properties_list[cnt]);
		if (!properties) {
			ERROR("MIssing setup for %s GPIO", properties_list[cnt]);
			return -EINVAL;
		}

		gpio = (cnt == 0) ? &hnd_data.reset : &hnd_data.prog;

		ret = get_gpio_from_property(properties, gpio) < 0;
		if (ret < 0) {
			ERROR("Cannot extract GPIO from properties");
			return ret;
		}

		DEBUG("line %s : device %s, num = %d, active_low = %s",
			properties_list[cnt],
			gpio->gpiodev,
			gpio->offset,
			gpio->active_low ? "true" : "false");
	}

	properties = dict_get_list(&img->properties, "debug");
	if (properties) {
		entry = LIST_FIRST(properties);
		if (entry && strtobool(entry->value))
			hnd_data.debug = true;
	}

	properties = dict_get_list(&img->properties, "timeout");
	if (properties) {
		entry = LIST_FIRST(properties);
		if (entry && (strtoul(entry->value, NULL, 10) > 0))
			hnd_data.timeout = strtoul(entry->value, NULL, 10);
	}

	ret = prepare_update(&hnd_data, img);
	if (ret) {
		ERROR("Prepare failed !!");
		goto handler_exit;
	}

	ret = copyimage(&hnd_data, img, update_fw);
	if (ret) {
		ERROR("Transferring image to uController was not successful");
		goto handler_exit;
	}

handler_exit:


	finish_update(&hnd_data);
	return ret;
}

__attribute__((constructor))
void ucfw_handler(void)
{
	register_handler("ucfw", install_uc_firmware_image,
				IMAGE_HANDLER, NULL);
}

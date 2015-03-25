/*
 * (C) Copyright 2012-2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */


#ifndef _HANDLER_H
#define _HANDLER_H

typedef enum {
	NONE,
	PREINSTALL,
	POSTINSTALL
} script_fn ;

typedef int (*handler)(struct img_type *img, void *data);
struct installer_handler{
	char	desc[64];
	handler installer;
	void	*data;
};

int register_handler(const char *desc, 
		handler installer, void *data);

struct installer_handler *find_handler(struct img_type *img);
void print_registered_handlers(void);
#endif

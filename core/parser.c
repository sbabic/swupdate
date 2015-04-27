/*
 * (C) Copyright 2013
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <unistd.h>
#include <fcntl.h>
#include "swupdate.h"
#include "parsers.h"

int parse(struct swupdate_cfg *sw, const char *descfile)
{
	int ret = -1;

#ifdef CONFIG_LIBCONFIG
	ret = parse_cfg(sw, descfile);
	if (ret == 0) return 0;
#endif
#ifdef CONFIG_JSON
	ret = parse_json(sw, descfile);
	if (ret == 0) return 0;
#endif
#ifdef CONFIG_LUAEXTERNAL
	ret = parse_external(sw, descfile);
	if (ret == 0) return 0;
#endif
	return ret;
}

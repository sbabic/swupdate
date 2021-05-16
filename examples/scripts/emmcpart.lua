-- SPDX-FileCopyrightText: 2014-2021 Stefano Babic <sbabic@denx.de>
--
-- SPDX-License-Identifier: CC0-1.0
--
function os.capture(cmd, raw)
	local f = assert(io.popen(cmd, 'r'))
	local s = assert(f:read('*a'))
	f:close()
	if (raw) then return s end
	s = string.gsub(s, '^%s+', '')
	s = string.gsub(s, '%s+$', '')
	s = string.gsub(s, '[\n\r]+', ' ')
	return s
end


function preinst()
	partitions = "# partition table of /dev/mmcblk0\n"..
		"unit: sectors\n\n"..
		"/dev/mmcblk0p1 : start=       16, size=  7812528, Id=83\n" ..
		"/dev/mmcblk0p2 : start=  7812544, size=  7293504, Id=83\n" ..
		"/dev/mmcblk0p3 : start=        0, size=        0, Id= 0\n" ..
		"/dev/mmcblk0p4 : start=        0, size=        0, Id= 0\n"

	f = io.output("/tmp/partitions")
	f:write(partitions)
	f:close()

	local out = os.capture("/usr/sbin/sfdisk /dev/mmcblk0 < /tmp/partitions", 1)
	local mkfs1 = os.capture("/sbin/mkfs.ext3 /dev/mmcblk0p2", 1)
	out = out .. mkfs1

	return true, out
end

function postinst()
	local out = "Post installed script called"

	return true, out
end

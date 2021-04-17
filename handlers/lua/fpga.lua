-- SPDX-FileCopyrightText: 2014 Stefano Babic <sbabic@denx.de>
--
-- SPDX-License-Identifier: CC0-1.0

require ("swupdate")

fpga_handler = function(image)
	print("Install FPGA Software ")
	swupdate.notify(swupdate.RECOVERY_STATUS.IDLE, 0, "register Lua handler")
	print ("hello world!")
	print ("RECOVERY_STATUS.IDLE: ".. swupdate.RECOVERY_STATUS.IDLE)
	print ("RECOVERY_STATUS.START: ".. swupdate.RECOVERY_STATUS.START)
	print ("RECOVERY_STATUS.RUN: ".. swupdate.RECOVERY_STATUS.RUN)
	print ("RECOVERY_STATUS.SUCCESS: ".. swupdate.RECOVERY_STATUS.SUCCESS)
	print ("RECOVERY_STATUS.FAILURE: ".. swupdate.RECOVERY_STATUS.FAILURE)

	for k,l in pairs(image) do
		print("image[" .. tostring(k) .. "] = " .. tostring(l))
		swupdate.notify(swupdate.RECOVERY_STATUS.RUN, 0, "image[" .. tostring(k) .. "] = " .. tostring(l))
	end

	err, msg = image:read(function(data) print(data) end)
	if err ~= 0 then
		swupdate.error(string.format("Error reading image: %s", msg))
		return 1
	end
	return 0
end

swupdate.register_handler("fpga", fpga_handler, swupdate.HANDLER_MASK.IMAGE_HANDLER)

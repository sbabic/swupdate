require ("swupdate")

fpga_handler = function(image)
        print("Install FPGA Software ")
	swupdate.notify(swupdate.RECOVERY_STATUS.IDLE,0,"register lua handle")
	print ("hello world!")
	print ("RECOVERY_STATUS.IDLE: ".. swupdate.RECOVERY_STATUS.IDLE)
	print ("RECOVERY_STATUS.START: ".. swupdate.RECOVERY_STATUS.START)
	print ("RECOVERY_STATUS.RUN: ".. swupdate.RECOVERY_STATUS.RUN)
	print ("RECOVERY_STATUS.SUCCESS: ".. swupdate.RECOVERY_STATUS.SUCCESS)
	print ("RECOVERY_STATUS.FAILURE: ".. swupdate.RECOVERY_STATUS.FAILURE)


        for k,l in pairs(image) do
                print("image[" .. tostring(k) .. "] = " .. tostring(l) )
                swupdate.notify(swupdate.RECOVERY_STATUS.RUN,0,"image[" .. tostring(k) .. "] = " .. tostring(l))
        end

        return 0
end

swupdate.register_handler("fpga",fpga_handler)

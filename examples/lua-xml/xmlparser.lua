-- SPDX-FileCopyrightText: 2014-2021 Stefano Babic <sbabic@denx.de>
--
-- SPDX-License-Identifier: CC0-1.0
--
----------------------------------------------
-- Test XML parsing with Lua
----------------------------------------------

lxp = require "lxp"
lxp.lom = require "lxp.lom"

function XMLSWDescParse(name)
	io.input(name)
	s = io.read("*all")
	return lxp.lom.parse(s)
end

function find_by_attr(tab, tag, attr, value)
      	for i = 1, #tab do
		if (type(tab[i]) == "table") then
			if (tab[i]["tag"] == tag and tab[i]["attr"][attr] == value) then
				return tab[i]
			end
		end
	end
end

function node_by_path(tab, p)
	t = string.find(p, "/")
	if (t == 1) then
		return node_by_path(tab, string.sub(p, t + 1))
	end

	if (t) then
		tag = string.sub(p, 1, t - 1)
	else
		tag = p
	end

	if (tab["tag"] == tag) then
		return node_by_path(tab, string.sub(p, t+1))
	end

      	for i = 1, #tab do
		if (type(tab[i]) == "table") then
			if (tab[i]["tag"] == tag) then
				if (t == nil) then
					return(tab[i])
				else
					return node_by_path(tab[i], string.sub(p, t+1),
						       	attrname, value)
				end
			end
		end
	end

	print ("Not found "..tag)
	return nil
end

function stream_by_device(tab, device)

	if (tab == nil or tab["tag"] == nil) then
		return nil
	end

      	for i = 1, #tab do
		if (type(tab[i]) == "table") then
			if (tab[i]["attr"]["device"] == device) then
				return tab[i]
			end
		end
	end

	return nil

end


function xmlparser(name, device)
	local tab = XMLSWDescParse(name)

	images = node_by_path(tab, "software/images")

	if (images == nil) then
		print("No image found")
		return nil
	end

	streams = stream_by_device(images, device)
	local count = 0
	img = {}
	if (streams) then
		for i = 1, #streams do
			if (streams[i] and streams[i]["tag"] == "stream") then
				print ("Name :"..streams[i]["attr"]["name"].." type:"..
					streams[i]["attr"]["type"])
				count = count + 1
				img[count] = {}
				img[count]["name"] = streams[i]["attr"]["name"]
				img[count]["type"] = streams[i]["attr"]["type"]
				--- check for all types that are scripts, for now only Lua scripts
				if (streams[i]["attr"]["type"] == "lua") then
					img[count]["script"] = 1
				end
				if (streams[i]["attr"]["volume"]) then
					img[count]["volume"] = streams[i]["attr"]["volume"]
				end
				if (streams[i]["attr"]["mtdname"]) then
					img[count]["mtdname"] = streams[i]["attr"]["mtdname"]
				end
				if (streams[i]["attr"]["dest"]) then
					img[count]["dest"] = streams[i]["attr"]["dest"]
				end
				if (streams[i]["attr"]["device_id"]) then
					img[count]["device_id"] = streams[i]["attr"]["device_id"]
				end

			end
		end
	end

	name = node_by_path(tab, "software/name")[1]
	version = tab["attr"]["version"]

	print (name..version)
	return name, version, count, img
end

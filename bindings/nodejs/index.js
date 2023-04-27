/*!
 * Copyright (C) 2023 Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier: MIT
 */

var net = require('net');
var c = require('c-struct');

const msgsize = 2416;

var progressmsg = new c.Schema({
    magic: c.type.uint32,
    status: c.type.uint32,
    dwl_percent : c.type.uint32,
    dwl_bytes_0 : c.type.uint32,
    dwl_bytes_1 : c.type.uint32,
    dwl_bytes_2 : c.type.uint32,
    nsteps : c.type.uint32,
    curstep : c.type.uint32,
    curpercent : c.type.uint32,
    curimage : c.type.string(256),
    handler : c.type.string(64),
    source : c.type.uint32,
    infolen : c.type.uint32,
    info : c.type.string(2048)
})

c.register('Progress', progressmsg);

const statusMap = [
   "IDLE",
   "START",
   "RUN",
   "SUCCESS",
   "FAILURE",
   "DOWNLOAD",
   "DONE",
   "SUBPROGRESS",
   "PROGRESS"
];

class SWUpdate {
  constructor(ctrlpath, progresspath, mycallback) {
    this.ctrlsocket = ctrlpath;
    this.progresssocket = progresspath;
    this.mycallback = mycallback;
    this.connected = false;
    if (typeof this.mycallback != 'function')
	console.log("Wrong callback !");
  }

  isConnected() {
	return this.connected;
  }

  receive = (msgsize, buffer) => {
	var out = [];
	if (!msgsize)
	    return;
	out = (c.unpackSync('Progress', buffer))
	var status = out.status;
	out.status = statusMap[out.status];
	var json = JSON.stringify(out);
	this.mycallback(json);
  }

  progress() {
    if (this.connected) {
      console.log('WRONG: already connected to SWUpdate');
      return false;
    }
    var clientProgress = net.createConnection({
	path : this.progresssocket,
	onread: {
		buffer : Buffer.alloc(msgsize),
		callback : this.receive
	}
    }, () => {
        this.connected = true;
    });

    clientProgress.on('end', () => {
      console.log('disconnected from server');
    });
    clientProgress.on('close', () => {
      this.connected = false;
      console.log('close from server');
    });
    clientProgress.on('error', () => {
      console.log('error connecting to SWUpdate');
    });
  }
}

module.exports = (ctrl, progress, fun) => {return new SWUpdate(ctrl, progress, fun)};

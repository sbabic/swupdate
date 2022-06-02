#!/usr/bin/python -B
#
# Author: Christian Storm <christian.storm@siemens.com>
# Copyright (C) 2022, Siemens AG
#
# SPDX-License-Identifier:     GPL-2.0-or-later

"""
Suricatta General HTTP Server Mock

Server mock implementation modeled after the 'General Purpose HTTP Server'.
"""

import os
import sys
import argparse
import hashlib
import pathlib
import logging

try:
    import bottle
except ModuleNotFoundError:
    server_dir = os.path.dirname(os.path.abspath(__file__))
    sys.exit(f"Install bottle, e.g., 'cd {server_dir}; wget http://bottlepy.org/bottle.py'")

BOTTLE_SCHEME = "http"
RETRY_BUSY = 10


class server(object):
    firmware_dir = None
    url = None
    debug = False


def log(message, loglevel=logging.INFO):
    logcolor = {
        logging.ERROR: 31,
        logging.WARN: 33,
        logging.INFO: 32,
        logging.DEBUG: 30,
    }
    loglevel = logging.ERROR if str(message).startswith(("400", "500")) else loglevel
    color = logcolor.get(loglevel, 1)
    print(f"\033[{color}m{message}\033[0m")
    return message


def logresult(func):
    def decorator(*args, **kwargs):
        return log(func(*args, **kwargs))

    return decorator


def extract_device_name(query):
    device_name = bottle.request.headers.get("name")
    if device_name is None and len(query) > 0:
        device_name = "".join(ch for ch in "".join(map("".join, sorted(query.items()))) if ch.isalnum())
    return device_name


@bottle.error(500)
@logresult
def error500(error):
    bottle.response.set_header("Content-Type", "text/plain; charset=utf-8")
    bottle.response.status = "500 Oops."
    return bottle.response.status


@bottle.error(404)
@logresult
def error404(error):
    bottle.response.set_header("Content-Type", "text/plain; charset=utf-8")
    bottle.response.status = "404 Not found."
    return bottle.response.status


@bottle.route("/log", method="PUT")
@logresult
def route_log():
    bottle.response.set_header("Content-Type", "text/plain; charset=utf-8")
    bottle.response.status = "200 Log received."
    log(">>> Log received: {}".format((bottle.request.body.read()).decode("UTF-8")))
    return bottle.response.status


@bottle.route("/", method="GET")
@logresult
def route_main():
    if server.debug:
        headers = ["{}: {}".format(h, bottle.request.headers.get(h)) for h in bottle.request.headers.keys()]
        log(
            ">>> {} {}\n{}".format(bottle.request.method, bottle.request.url, "\n".join(headers)),
            logging.DEBUG,
        )

    bottle.response.set_header("Content-Type", "text/plain; charset=utf-8")

    device_name = extract_device_name(bottle.request.query)
    if device_name is None:
        bottle.response.status = "400 HTTP Header 'name' or 'identify' cfg section error."
        return bottle.response.status

    firmware = os.path.join(server.firmware_dir, device_name)
    firmware_relative_path = pathlib.Path(firmware).relative_to(os.getcwd())
    if not os.path.isfile(firmware) or not os.access(firmware, os.R_OK):
        log(f">>> Place firmware file at '{firmware_relative_path}' to update device {device_name}.")
        # Send back the client name ID served just for the client's information.
        bottle.response.set_header("Served-Client", device_name)
        bottle.response.status = f"404 No update is available for {device_name}"
        return bottle.response.status
    log(f">>> Firmware file found at {firmware_relative_path}.")

    try:
        with open(firmware, "rb") as file:
            bottle.response.set_header("Content-Md5", hashlib.md5(file.read()).hexdigest())
    except FileNotFoundError:
        bottle.response.status = f"500 Firmware file not found: {firmware_relative_path}"
        return bottle.response.status

    bottle.response.status = "302 Update available."
    bottle.response.set_header("Location", f"{server.url}/firmware/{device_name}")
    return bottle.response.status


@bottle.route("/firmware/<filepath:path>")
def route_download(filepath):
    if bottle.request.method == "GET":
        log(f">>> Serving firmware file: {filepath}")
    return bottle.static_file(filepath, root=server.firmware_dir)


def runserver():
    parser = argparse.ArgumentParser(
        add_help=True,
        description="""SWUpdate Suricatta 'General HTTP Server' Mock.""",
        epilog="""""",
    )
    parser.add_argument(
        "-x",
        metavar="BOTTLE_HOST",
        dest="BOTTLE_HOST",
        help="host to bind to (default: %(default)s)",
        default="localhost",
    )
    parser.add_argument(
        "-p",
        metavar="BOTTLE_PORT",
        dest="BOTTLE_PORT",
        help="port to bind to (default: %(default)s)",
        default="8080",
    )
    parser.add_argument(
        "-d",
        dest="debug",
        help="Enable debug logging",
        default=False,
        action="store_true",
    )
    parser.add_argument(
        "-f",
        metavar="FIRMWARE_DIR",
        dest="FIRMWARE_DIR",
        help="path to firmware files" " directory (default: %(default)s)",
        default=os.path.join(os.getcwd(), "firmwares"),
    )
    args = parser.parse_args()

    global server
    server.debug = args.debug
    server.url = f"{BOTTLE_SCHEME}://{args.BOTTLE_HOST}:{args.BOTTLE_PORT}"
    server.firmware_dir = args.FIRMWARE_DIR

    if not pathlib.Path(server.firmware_dir).is_dir():
        sys.exit(f"Path {server.firmware_dir} is not an existing directory.")

    log(f"Debug logging: {server.debug}.", logging.INFO)
    log(f"Serving firmware files from {server.firmware_dir}")
    bottle.run(host=args.BOTTLE_HOST, port=args.BOTTLE_PORT, debug=False)


if __name__ == "__main__":
    try:
        runserver()
    except OSError as e:
        if e.errno != 98:
            raise
        print("ERROR: Address already in use, server already running?")

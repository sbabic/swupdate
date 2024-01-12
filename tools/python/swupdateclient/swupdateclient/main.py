# SPDX-FileCopyrightText: 2018 Stefano Babic <stefano.babic@swupdate.org>
# SPDX-FileCopyrightText: 2021 Blueye Robotics AS
#
# SPDX-License-Identifier:     GPL-2.0-only
#

import argparse
import asyncio
import json
import logging
import os
import sys
import string
from swupdateclient import __about__
from typing import List, Optional, Tuple, Union


import requests
from termcolor import colored
import websockets


LOGGING_MAPPING = {
    "3": logging.ERROR,
    "4": logging.WARNING,
    "6": logging.INFO,
    "7": logging.DEBUG,
}


class ColorFormatter(logging.Formatter):
    """Custom logging formatter with colorized output"""

    COLORS = {
        logging.DEBUG: None,
        logging.INFO: None,
        logging.WARNING: "yellow",
        logging.ERROR: "red",
    }

    ATTRIBUTES = {
        logging.DEBUG: [],
        logging.INFO: ["bold"],
        logging.WARNING: ["bold"],
        logging.ERROR: ["bold"],
    }

    def format(self, record):
        return logging.Formatter(
            colored(
                "%(levelname)s:%(name)s:%(message)s",
                self.COLORS[record.levelno],
                attrs=self.ATTRIBUTES[record.levelno],
            )
        ).format(record)


class SWUpdater:
    """Python helper class for SWUpdate"""

    url_upload = "http://{}:{}{}/upload"
    url_status = "ws://{}:{}{}/ws"

    def __init__(
        self,
        path_image,
        host_name,
        port=8080,
        path="",
        logger=None,
        log_level=logging.DEBUG,
    ):
        self._image = path_image
        self._host_name = host_name
        self._port = port
        self._path = path
        if logger is not None:
            self._logger = logger
        else:
            handler = logging.StreamHandler()
            handler.setFormatter(ColorFormatter())
            self._logger = logging.getLogger("swupdate")
            self._logger.addHandler(handler)
            self._logger.setLevel(log_level)

    async def wait_update_finished(self):
        self._logger.info("Waiting for messages on websocket connection")
        try:
            async with websockets.connect(
                self.url_status.format(self._host_name, self._port, self._path)
            ) as websocket:
                while True:
                    try:
                        message = await websocket.recv()
                        message = "".join(
                            filter(lambda x: x in set(string.printable), message)
                        )
                    except Exception:
                        self._logger.exception("Unknown exception")
                        continue

                    try:
                        data = json.loads(message)
                    except json.decoder.JSONDecodeError:
                        # As of 2021.04, the version info message contains invalid json
                        self._logger.warning("json parse error: %s", message)
                        continue

                    if data["type"] != "message":
                        continue

                    self._logger.log(LOGGING_MAPPING[data["level"]], data["text"])

                    if "SWUPDATE successful" in data["text"]:
                        return True
                    if "Installation failed" in data["text"]:
                        return False
        except Exception:
            self._logger.exception("Unknown exception")
            return False

    def sync_upload(self, swu_file, timeout):
        return requests.post(
            self.url_upload.format(self._host_name, self._port, self._path),
            files={"file": swu_file},
            headers={"Cache-Control": "no-cache"},
            timeout=timeout,
        )

    async def upload(self, timeout):
        self._logger.info("Start uploading image...")
        try:
            with open(self._image, "rb") as swu_file:
                loop = asyncio.get_event_loop()
                response = await loop.run_in_executor(
                    None, self.sync_upload, swu_file, timeout
                )

            if response.status_code != 200:
                self._logger.error(
                    "Cannot upload software image: %s", response.status_code
                )
                return False

            self._logger.info(
                "Software image uploaded successfully."
                "Wait for installation to be finished..."
            )
            return True
        except ValueError:
            self._logger.error("No connection to host, exit")
        except FileNotFoundError:
            self._logger.error("swu file not found")
        except requests.exceptions.ConnectionError:
            self._logger.exception("Connection error")
        return False

    async def start_tasks(self, timeout):
        ws_task = asyncio.create_task(self.wait_update_finished())
        upload_task = asyncio.create_task(self.upload(timeout))

        if not await upload_task:
            self._logger.info("Cancelling websocket task")
            ws_task.cancel()
            return False

        try:
            result = await asyncio.wait_for(ws_task, timeout=timeout)
        except asyncio.TimeoutError:
            self._logger.error("timeout!")
            return False

        return result

    def update(self, timeout=300):
        return asyncio.run(self.start_tasks(timeout))


def client(args: List[str]) -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("swu_file", help="Path to swu image")
    parser.add_argument("host_name", help="Host name")
    parser.add_argument("port", help="Port", type=int, default=8080, nargs="?")
    parser.add_argument(
        "path",
        help="Name of the webserver-path (e.g. /PATH)",
        type=str,
        default="",
        nargs="?",
    )
    parser.add_argument(
        "--timeout",
        help="Timeout for the whole swupdate process",
        type=int,
        default=300,
        nargs="?",
    )
    parser.add_argument(
        "--log-level",
        help="change log level (error, info, warning, debug)",
        type=str,
        metavar="[LEVEL]",
        choices=["error", "info", "warning", "debug"],
        default="debug",
    )
    parser.add_argument(
        "--color",
        help="colorize messages (auto, always or never)",
        type=str,
        metavar="[WHEN]",
        choices=["auto", "always", "never"],
        default="auto",
    )

    args = parser.parse_args()

    # Configure logging colors
    if args.color == "always":
        os.environ["FORCE_COLOR"] = "yes"
    elif args.color == "never":
        os.environ["NO_COLOR"] = "yes"

    updater = SWUpdater(
        args.swu_file,
        args.host_name,
        args.port,
        args.route,
        log_level=args.log_level.upper(),
    )
    updater.update(timeout=args.timeout)


def main():
    client(sys.argv[1:])


if __name__ == "__main__":
    main()

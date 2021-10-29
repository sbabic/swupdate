#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2018 Stefano Babic <sbabic@denx.de>
# SPDX-FileCopyrightText: 2021 Blueye Robotics AS
#
# SPDX-License-Identifier:     GPL-2.0-only
#

import asyncio
import json
import requests
import websockets
import logging
import string
import argparse
import sys


class SWUpdater:
    """Python helper class for SWUpdate"""

    url_upload = "http://{}:{}/upload"
    url_status = "ws://{}:{}/ws"

    def __init__(self, path_image, host_name, port=8080, logger=None):
        self._image = path_image
        self._host_name = host_name
        self._port = port
        if logger is not None:
            self._logger = logger
        else:
            logging.basicConfig(stream=sys.stdout, level=logging.INFO)
            self._logger = logging.getLogger("swupdate")

    async def wait_update_finished(self):
        self._logger.info("Waiting for messages on websocket connection")
        try:
            async with websockets.connect(
                self.url_status.format(self._host_name, self._port)
            ) as websocket:
                while True:
                    try:
                        message = await websocket.recv()
                        message = "".join(
                            filter(lambda x: x in set(string.printable), message)
                        )

                    except Exception as err:
                        self._logger.warning(err)
                        continue

                    try:
                        data = json.loads(message)
                    except json.decoder.JSONDecodeError:
                        # As of 2021.04, the version info message contains invalid json
                        self._logger.warning(f"json parse error: {message}")
                        continue

                    if data["type"] != "message":
                        continue

                    self._logger.info(data["text"])
                    if "SWUPDATE successful" in data["text"]:
                        return True
                    if "Installation failed" in data["text"]:
                        return False

        except Exception as err:
            self._logger.error(err)
            return False

    def sync_upload(self, swu_file, timeout):
        return requests.post(
            self.url_upload.format(self._host_name, self._port),
            files={"file": swu_file},
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
                    "Cannot upload software image: {}".format(response.status_code)
                )
                return False

            self._logger.info(
                "Software image uploaded successfully."
                "Wait for installation to be finished..."
            )
            return True
        except ValueError:
            self._logger.info("No connection to host, exit")
        except FileNotFoundError:
            self._logger.info("swu file not found")
        except requests.exceptions.ConnectionError as e:
            self._logger.info("Connection Error:\n%s" % str(e))
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
            self._logger.info("timeout!")
            return False

        return result

    def update(self, timeout=300):
        return asyncio.run(self.start_tasks(timeout))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("swu_file", help="Path to swu image")
    parser.add_argument("host_name", help="Host name")
    parser.add_argument("port", help="Port", type=int, default=8080, nargs="?")
    parser.add_argument(
        "--timeout",
        help="Timeout for the whole swupdate process",
        type=int,
        default=300,
        nargs="?",
    )

    args = parser.parse_args()
    updater = SWUpdater(args.swu_file, args.host_name, args.port)
    updater.update(timeout=args.timeout)

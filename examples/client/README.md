<!--
SPDX-FileCopyrightText: 2021 Blueye Robotics AS

SPDX-License-Identifier: GPL-2.0-only
-->

## Install dependencies

### apt
```
sudo apt update
sudo apt install python3-websockets python3-requests
```

### pip
```
pip install websockets requests
```

### pipenv
```
pipenv install
```

## Usage

### apt/pip
```
./swupdate_client.py <path-to-swu> <host_name> [port]
```

### pipenv
```
pipenv run ./swupdate_client.py <path-to-swu> <host_name> [port]
```


## Development
### Import from another python program
```
from swupdate_client import SWUpdater

updater = SWUpdater("path-to-swu", "host-name")
if updater.update():
    print("Update successful!")
else:
    print("Update failed!")
```

### Formatting
```
black swupdate_client.py
```
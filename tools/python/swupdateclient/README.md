<!--
SPDX-FileCopyrightText: 2021 Blueye Robotics AS

SPDX-License-Identifier: GPL-2.0-only
-->

## Installing swupdateclient

### pip
```
cd tools/python/swupdateclient
pip3 install .
```

### pipenv
```
pipenv install
```

## Usage

### pip
```
swupdateclient <path-to-swu> <host_name> [port] [path]
```

### pipenv
```
pipenv run swupdateclient <path-to-swu> <host_name> [port] [path]
```


## Development
### Import from another python program
```
from swupdateclient.main import SWUpdater

updater = SWUpdater("path-to-swu", "host-name")
if updater.update():
    print("Update successful!")
else:
    print("Update failed!")
```

### Formatting
```
black swupdateclient
```

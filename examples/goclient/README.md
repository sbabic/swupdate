# goclient
## About
An example swupdate client written in go. This client does not connect to the websocket for status but does illustrate how to POST the SWU file to `/upload`. It also has the nice feature that it is a streaming client, which is to say that it does not load the whole SWU file into RAM.

## Building
```
go build
```

## Running
Just like the python example client:
```
./goclient /some/path/to/some/swupade.swu hostname
```

It does include command line help:
```
Usage of ./goclient [-port PORT] <path to image> <hostname>:
  -port int
        The port to connect to. (default 8080)
```

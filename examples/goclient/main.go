package main

import (
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
)

type commandLineOptions struct {
	hostname string
	path     string
	port     int
}

func parseCommandLine() *commandLineOptions {
	options := new(commandLineOptions)

	flag.Usage = func() {
		fmt.Fprintf(flag.CommandLine.Output(), "Usage of %s [-port PORT] <path to image> <hostname>:\n", os.Args[0])
		flag.PrintDefaults()
	}

	flag.IntVar(&options.port, "port", 8080,
		"The port to connect to.")
	flag.Parse()
	if options.port < 1 || options.port > 65535 {
		fmt.Fprintf(os.Stderr, "A valid port is required.\n")
		flag.PrintDefaults()
		os.Exit(1)
	}

	args := flag.Args()
	if len(args) != 2 {
		fmt.Fprintf(os.Stderr, "<path to image> <hostname> are required.\n")
		flag.PrintDefaults()
		os.Exit(1)
	}
	options.path = args[0]
	options.hostname = args[1]

	return options
}

func main() {
	options := parseCommandLine()

	file, err := os.Open(options.path)
	defer file.Close()
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s\n", err)
		os.Exit(1)
	}

	contentType, contentLength, body, err := bodyMultiReader(file)
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s\n", err)
		os.Exit(1)
	}

	url := fmt.Sprintf("http://%s:%d/upload", options.hostname, options.port)
	req, err := http.NewRequest(http.MethodPost, url, body)
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s\n", err)
		os.Exit(1)
	}
	req.Header.Set("Content-type", contentType)

	req.ContentLength = contentLength

	fmt.Printf("attempting to swupdate %s\n", options.hostname)
	client := &http.Client{}
	resp, err := client.Do(req)
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s\n", err)
		os.Exit(1)
	}

	if resp.StatusCode == http.StatusOK {
		fmt.Printf("%s swupdate POST succeeded!\n", options.hostname)
	} else {
		fmt.Fprintf(os.Stderr, "%s swupdate POST failed with %d\n", url, resp.StatusCode)
		os.Exit(1)
	}
}

// returns contentType, contentLength, body
//
// Further reading:
// https://medium.com/akatsuki-taiwan-technology/uploading-large-files-in-golang-without-using-buffer-or-pipe-9b5aafacfc16
func bodyMultiReader(file *os.File) (string, int64, io.Reader, error) {
	fileInfo, err := file.Stat()
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s\n", err)
		return "", int64(0), nil, err
	}

	boundary := "superCalifragilisticExpialiBoundary"
	fileHeader := "Content-type: application/octet-stream"
	// swupdate doesn't seem to care about name or filename but we include them to be polite
	fileFormat := "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n%s\r\n\r\n"
	filePart := fmt.Sprintf(fileFormat, boundary, fileInfo.Name(), fileHeader)
	bodyBottom := fmt.Sprintf("\r\n--%s--\r\n", boundary)
	body := io.MultiReader(strings.NewReader(filePart), file, strings.NewReader(bodyBottom))
	contentType := fmt.Sprintf("multipart/form-data; boundary=%s", boundary)

	contentLength := int64(len(filePart)) + fileInfo.Size() + int64(len(bodyBottom))

	return contentType, contentLength, body, nil
}

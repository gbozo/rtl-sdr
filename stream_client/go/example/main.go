package main

import (
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"syscall"

	"github.com/gbozo/rtl-sdr/stream_client/go/rtlstream"
)

func main() {
	if len(os.Args) < 4 {
		fmt.Fprintf(os.Stderr, "Usage: %s <host> <port> <freq_hz>\n", os.Args[0])
		os.Exit(1)
	}

	host := os.Args[1]
	port, _ := strconv.Atoi(os.Args[2])
	freq, _ := strconv.ParseUint(os.Args[3], 10, 64)

	client, err := rtlstream.Connect(host, port)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to connect: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()

	err = client.Request(freq, 2400000, 200000, rtlstream.ModeIQ)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to send request: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Connected to rtl_stream at %s:%s, freq=%d Hz\n", host, os.Args[2], freq)
	fmt.Println("Receiving I/Q frames (Ctrl+C to stop)...")

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)

	samples := make([]int16, 4096)

	for {
		select {
		case <-sig:
			fmt.Println("\nDisconnected.")
			return
		default:
			hdr, n, err := client.ReadIQ(samples)
			if err != nil {
				fmt.Fprintf(os.Stderr, "Read error: %v\n", err)
				return
			}
			fmt.Printf("I/Q frame: freq=%d rate=%d seq=%d nsamples=%d sample[0]=(%d,%d)\n",
				hdr.Freq, hdr.Rate, hdr.Seq, n, samples[0], samples[1])
		}
	}
}

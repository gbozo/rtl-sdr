package main

import (
	"errors"
	"fmt"
	"os"
	"time"

	"github.com/gbozo/rtl-sdr/stream_client/go/rtlstream"
)

const (
	host    = "127.0.0.1"
	iqPort  = 1234
	fftPort = 1235
	freq    = 100600000
	rate    = 240000
	bw      = 192000
)

func main() {
	iqDone := make(chan error, 1)
	fftDone := make(chan error, 1)

	go func() {
		iqDone <- doIQ()
	}()

	go func() {
		fftDone <- doFFT()
	}()

	for i := 0; i < 2; i++ {
		select {
		case err := <-iqDone:
			if err != nil {
				fmt.Fprintf(os.Stderr, "I/Q error: %v\n", err)
			}
		case err := <-fftDone:
			if err != nil {
				fmt.Fprintf(os.Stderr, "FFT error: %v\n", err)
			}
		}
	}
}

func doIQ() error {
	c, err := rtlstream.Connect(host, iqPort)
	if err != nil {
		return fmt.Errorf("IQ connect: %w", err)
	}
	defer c.Close()

	if err := c.Request(freq, rate, bw, rtlstream.ModeIQ); err != nil {
		return fmt.Errorf("IQ request: %w", err)
	}
	fmt.Printf("IQ: connected, tuned to %.1f MHz\n", float64(freq)/1e6)

	samples := make([]int16, rtlstream.MaxIQComponents)

	for i := 0; i < 5; i++ {
		hdr, n, err := c.ReadIQ(samples)
		var event *rtlstream.Event
		if errors.As(err, &event) {
			if event.Type == rtlstream.StreamEnd {
				return nil
			}
			fmt.Printf("IQ frequency change: %d Hz\n", event.Freq)
			continue
		}
		if err != nil {
			return fmt.Errorf("IQ read: %w", err)
		}
		if n == 0 {
			fmt.Printf("IQ frame %d: freq=%.1f MHz seq=%d empty\n",
				i, float64(hdr.Freq)/1e6, hdr.Seq)
			continue
		}
		mean := 0.0
		for _, s := range samples[:n] {
			if s < 0 {
				mean -= float64(s)
			} else {
				mean += float64(s)
			}
		}
		mean /= float64(n)
		fmt.Printf("IQ frame %d: freq=%.1f MHz seq=%d mean_mag=%.1f\n",
			i, float64(hdr.Freq)/1e6, hdr.Seq, mean)
	}

	return nil
}

func doFFT() error {
	c, err := rtlstream.Connect(host, fftPort)
	if err != nil {
		return fmt.Errorf("FFT connect: %w", err)
	}
	defer c.Close()

	if err := c.Request(0, 0, 0, rtlstream.ModeFFT); err != nil {
		return fmt.Errorf("FFT request: %w", err)
	}
	fmt.Printf("FFT: connected, waiting for frames...\n")

	power := make([]float32, 4096)

	peak := 0.0
	lastPeakFreq := 0.0
	centerFreq := 100e6

	for i := 0; i < 10; i++ {
		hdr, power, err := c.ReadFFT(power)
		var event *rtlstream.Event
		if errors.As(err, &event) {
			if event.Type == rtlstream.StreamEnd {
				return nil
			}
			fmt.Printf("FFT frequency change: %d Hz\n", event.Freq)
			continue
		}
		if err != nil {
			return fmt.Errorf("FFT read: %w", err)
		}

		total := float64(0)
		peakVal := float32(-1e30)
		peakIdx := 0
		for j, v := range power {
			total += float64(v)
			if v > peakVal {
				peakVal = v
				peakIdx = j
			}
		}

		binWidth := float64(hdr.Rate) / float64(hdr.Bins)
		signedBin := peakIdx
		if peakIdx >= int(hdr.Bins)/2 {
			signedBin -= int(hdr.Bins)
		}
		peakFreq := float64(hdr.Freq) + float64(signedBin)*binWidth
		peak = float64(peakVal)
		lastPeakFreq = peakFreq

		avg := total / float64(hdr.Bins)
		fmt.Printf("FFT frame %d: seq=%d bins=%d avg=%.1f peak=%.1f @ %.1f MHz\n",
			i, hdr.Seq, hdr.Bins, avg, peak, peakFreq/1e6)

		time.Sleep(100 * time.Millisecond)
	}

	_ = centerFreq
	_ = lastPeakFreq

	return nil
}

package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func run(args []string) error {
	if len(args) == 0 {
		return errors.New("usage: appvault-api init|serve|rotate-token [options]")
	}
	switch args[0] {
	case "init":
		flags := flag.NewFlagSet("init", flag.ContinueOnError)
		data := flags.String("data", "", "new server data directory")
		vault := flags.String("vault", "", "source encrypted .lvault file")
		if err := flags.Parse(args[1:]); err != nil {
			return err
		}
		token, err := initialize(*data, *vault)
		if err != nil {
			return err
		}
		fmt.Printf("API token (shown once): %s\n", token)
		return nil
	case "rotate-token":
		flags := flag.NewFlagSet("rotate-token", flag.ContinueOnError)
		data := flags.String("data", "", "server data directory")
		if err := flags.Parse(args[1:]); err != nil {
			return err
		}
		if *data == "" {
			return errors.New("-data is required")
		}
		token, err := rotateToken(*data)
		if err != nil {
			return err
		}
		fmt.Printf("New API token (shown once): %s\n", token)
		return nil
	case "serve":
		flags := flag.NewFlagSet("serve", flag.ContinueOnError)
		data := flags.String("data", "", "server data directory")
		address := flags.String("listen", "127.0.0.1:8080", "HTTP listen address")
		cert := flags.String("cert", "", "optional TLS certificate")
		key := flags.String("key", "", "TLS private key")
		if err := flags.Parse(args[1:]); err != nil {
			return err
		}
		if *data == "" || (*cert == "") != (*key == "") {
			return errors.New("-data is required; -cert and -key must be specified together")
		}
		unlock, err := acquireServerLock(*data)
		if err != nil {
			return err
		}
		defer unlock()
		handler, err := newHandler(*data)
		if err != nil {
			return err
		}
		server := &http.Server{
			Addr:              *address,
			Handler:           handler,
			ReadHeaderTimeout: 5 * time.Second,
			ReadTimeout:       30 * time.Second,
			WriteTimeout:      30 * time.Second,
			IdleTimeout:       60 * time.Second,
			MaxHeaderBytes:    16 << 10,
		}
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer stop()
		done := make(chan struct{})
		defer close(done)
		go func() {
			select {
			case <-ctx.Done():
				shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
				defer cancel()
				_ = server.Shutdown(shutdown)
			case <-done:
			}
		}()
		log.Printf("Serving encrypted vault at %s/vault", *address)
		if *cert != "" {
			err = server.ListenAndServeTLS(*cert, *key)
		} else {
			err = server.ListenAndServe()
		}
		if errors.Is(err, http.ErrServerClosed) {
			return nil
		}
		return err
	default:
		return fmt.Errorf("unknown command %q", args[0])
	}
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		log.Print(err)
		os.Exit(1)
	}
}

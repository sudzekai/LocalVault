package main

import (
	"fmt"
	"os"
	"path/filepath"
	"syscall"
)

func acquireServerLock(dataPath string) (func(), error) {
	file, err := os.OpenFile(filepath.Join(dataPath, ".server.lock"), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(file.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		file.Close()
		return nil, fmt.Errorf("data directory is already in use: %w", err)
	}
	return func() { file.Close() }, nil
}

func replaceFile(source, target string) error {
	if err := os.Rename(source, target); err != nil {
		return err
	}
	dir, err := os.Open(filepath.Dir(target))
	if err != nil {
		return err
	}
	defer dir.Close()
	return dir.Sync()
}

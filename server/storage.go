package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

const maxFileSize int64 = (16 << 20) + 156

type credentials struct {
	TokenHash string `json:"token_hash"`
}

func generateCredentials() (string, credentials) {
	secret := make([]byte, 32)
	if _, err := rand.Read(secret); err != nil {
		panic(err)
	}
	token := base64.RawURLEncoding.EncodeToString(secret)
	sum := sha256.Sum256([]byte(token))
	return token, credentials{TokenHash: hex.EncodeToString(sum[:])}
}

func readBounded(path string) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		return nil, errors.New("vault must be a regular file")
	}
	data, err := io.ReadAll(io.LimitReader(file, maxFileSize+1))
	if err != nil {
		return nil, err
	}
	if len(data) == 0 || int64(len(data)) > maxFileSize {
		return nil, errors.New("vault size must be between 1 byte and 16 MiB plus its header")
	}
	return data, nil
}

func writeAtomic(path string, data []byte) error {
	dir := filepath.Dir(path)
	temporary, err := os.CreateTemp(dir, ".appvault-write-*")
	if err != nil {
		return err
	}
	name := temporary.Name()
	defer os.Remove(name)
	defer temporary.Close()
	if err := temporary.Chmod(0600); err != nil {
		return err
	}
	if _, err := temporary.Write(data); err != nil {
		return err
	}
	if err := temporary.Sync(); err != nil {
		return err
	}
	if err := temporary.Close(); err != nil {
		return err
	}
	return replaceFile(name, path)
}

func initialize(dataPath, source string) (string, error) {
	if source == "" || dataPath == "" {
		return "", errors.New("both -data and -vault are required")
	}
	absolute, err := filepath.Abs(dataPath)
	if err != nil {
		return "", err
	}
	if _, err := os.Lstat(absolute); !errors.Is(err, os.ErrNotExist) {
		return "", errors.New("the -data directory must not already exist")
	}
	encrypted, err := readBounded(source)
	if err != nil {
		return "", fmt.Errorf("read source file: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(absolute), 0700); err != nil {
		return "", err
	}
	staging, err := os.MkdirTemp(filepath.Dir(absolute), ".appvault-init-*")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(staging)
	token, config := generateCredentials()
	encoded, err := json.MarshalIndent(config, "", "  ")
	if err != nil {
		return "", err
	}
	if err := os.WriteFile(filepath.Join(staging, "credentials.json"), encoded, 0600); err != nil {
		return "", err
	}
	if err := os.WriteFile(filepath.Join(staging, "vault.lvault"), encrypted, 0600); err != nil {
		return "", err
	}
	if err := os.Rename(staging, absolute); err != nil {
		return "", err
	}
	return token, nil
}

func loadTokenHash(dataPath string) ([32]byte, error) {
	var result [32]byte
	file, err := os.Open(filepath.Join(dataPath, "credentials.json"))
	if err != nil {
		return result, err
	}
	defer file.Close()
	var config credentials
	decoder := json.NewDecoder(io.LimitReader(file, 4096))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&config); err != nil {
		return result, err
	}
	hash, err := hex.DecodeString(config.TokenHash)
	if err != nil || len(hash) != len(result) {
		return result, errors.New("invalid token hash in credentials.json")
	}
	copy(result[:], hash)
	return result, nil
}

func rotateToken(dataPath string) (string, error) {
	unlock, err := acquireServerLock(dataPath)
	if err != nil {
		return "", err
	}
	defer unlock()
	if _, err := loadTokenHash(dataPath); err != nil {
		return "", err
	}
	token, config := generateCredentials()
	encoded, err := json.MarshalIndent(config, "", "  ")
	if err != nil {
		return "", err
	}
	if err := writeAtomic(filepath.Join(dataPath, "credentials.json"), encoded); err != nil {
		return "", err
	}
	return token, nil
}

package main

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

type vaultHandler struct {
	path      string
	tokenHash [32]byte
	mu        sync.RWMutex
}

func entityTag(data []byte) string {
	sum := sha256.Sum256(data)
	return "\"" + hex.EncodeToString(sum[:]) + "\""
}

func (h *vaultHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	if r.URL.Path != "/vault" || r.URL.RawQuery != "" {
		http.NotFound(w, r)
		return
	}
	authorization := r.Header.Get("Authorization")
	token, bearer := strings.CutPrefix(authorization, "Bearer ")
	sum := sha256.Sum256([]byte(token))
	if !bearer || len(token) > 1024 || subtle.ConstantTimeCompare(sum[:], h.tokenHash[:]) != 1 {
		w.Header().Set("WWW-Authenticate", "Bearer")
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	switch r.Method {
	case http.MethodGet:
		h.get(w, r)
	case http.MethodPost:
		h.post(w, r)
	default:
		w.Header().Set("Allow", "GET, POST")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (h *vaultHandler) get(w http.ResponseWriter, r *http.Request) {
	h.mu.RLock()
	data, err := readBounded(h.path)
	h.mu.RUnlock()
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			http.Error(w, "vault unavailable", http.StatusNotFound)
		} else {
			http.Error(w, "vault read failed", http.StatusInternalServerError)
		}
		return
	}
	w.Header().Set("ETag", entityTag(data))
	w.Header().Set("Content-Type", "application/octet-stream")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(data)
}

func (h *vaultHandler) post(w http.ResponseWriter, r *http.Request) {
	expected := r.Header.Get("If-Match")
	if expected == "" || expected == "*" || len(expected) > 128 {
		http.Error(w, "a strong If-Match header is required", http.StatusPreconditionRequired)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxFileSize)
	data, err := io.ReadAll(r.Body)
	if err != nil {
		var tooLarge *http.MaxBytesError
		if errors.As(err, &tooLarge) {
			http.Error(w, "file too large", http.StatusRequestEntityTooLarge)
		} else {
			http.Error(w, "invalid request body", http.StatusBadRequest)
		}
		return
	}
	if len(data) == 0 {
		http.Error(w, "empty file", http.StatusBadRequest)
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	current, err := readBounded(h.path)
	if err != nil {
		http.Error(w, "vault unavailable", http.StatusServiceUnavailable)
		return
	}
	tag := entityTag(current)
	replacementTag := entityTag(data)
	// An exact retry is safe if the first response was lost after committing.
	if tag == replacementTag {
		w.Header().Set("ETag", tag)
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if expected != tag {
		w.Header().Set("ETag", tag)
		http.Error(w, "vault changed; reload before saving", http.StatusPreconditionFailed)
		return
	}
	if err := writeAtomic(h.path, data); err != nil {
		http.Error(w, "vault write failed", http.StatusInternalServerError)
		return
	}
	w.Header().Set("ETag", replacementTag)
	w.WriteHeader(http.StatusNoContent)
}

func newHandler(dataPath string) (*vaultHandler, error) {
	hash, err := loadTokenHash(dataPath)
	if err != nil {
		return nil, err
	}
	path := filepath.Join(dataPath, "vault.lvault")
	if _, err := readBounded(path); err != nil {
		return nil, err
	}
	return &vaultHandler{path: path, tokenHash: hash}, nil
}

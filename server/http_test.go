package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func fixture(t *testing.T) (string, string, *vaultHandler) {
	t.Helper()
	parent := t.TempDir()
	source := filepath.Join(parent, "source.lvault")
	if err := os.WriteFile(source, []byte("opaque test file\x00\x01"), 0600); err != nil {
		t.Fatal(err)
	}
	directory := filepath.Join(parent, "data")
	token, err := initialize(directory, source)
	if err != nil {
		t.Fatal(err)
	}
	handler, err := newHandler(directory)
	if err != nil {
		t.Fatal(err)
	}
	return directory, token, handler
}

func request(t *testing.T, handler http.Handler, method, token, tag string, body []byte) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(method, "/vault", bytes.NewReader(body))
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	if tag != "" {
		r.Header.Set("If-Match", tag)
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, r)
	return response
}

func TestAuthorizationAndOpaqueFiles(t *testing.T) {
	directory, token, handler := fixture(t)
	original, _ := os.ReadFile(handler.path)
	for _, method := range []string{http.MethodGet, http.MethodPost} {
		for _, secret := range []string{"", "incorrect-token"} {
			result := request(t, handler, method, secret, entityTag(original), []byte("replacement"))
			if result.Code != http.StatusUnauthorized || bytes.Contains(result.Body.Bytes(), original) {
				t.Fatal("unauthorized access accepted")
			}
		}
	}
	result := request(t, handler, http.MethodGet, token, "", nil)
	if result.Code != http.StatusOK || !bytes.Equal(result.Body.Bytes(), original) ||
		result.Header().Get("ETag") != entityTag(original) || result.Header().Get("Cache-Control") != "no-store" {
		t.Fatal("GET did not preserve file bytes or headers")
	}
	config, err := os.ReadFile(filepath.Join(directory, "credentials.json"))
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256([]byte(token))
	if bytes.Contains(config, []byte(token)) || !bytes.Contains(config, []byte(hex.EncodeToString(sum[:]))) {
		t.Fatal("server must store only the token hash")
	}
	r := httptest.NewRequest(http.MethodGet, "/credentials.json", nil)
	r.Header.Set("Authorization", "Bearer "+token)
	hidden := httptest.NewRecorder()
	handler.ServeHTTP(hidden, r)
	if hidden.Code != http.StatusNotFound {
		t.Fatal("server exposed private data")
	}
	r = httptest.NewRequest(http.MethodGet, "/vault?token="+token, nil)
	hidden = httptest.NewRecorder()
	handler.ServeHTTP(hidden, r)
	if hidden.Code != http.StatusNotFound {
		t.Fatal("token query string is accepted")
	}
}

func TestPostVersionAndAtomicReplacement(t *testing.T) {
	directory, token, handler := fixture(t)
	original, _ := os.ReadFile(handler.path)
	configBefore, _ := os.ReadFile(filepath.Join(directory, "credentials.json"))
	replacement := []byte("new encrypted file\x00")
	if result := request(t, handler, http.MethodPost, token, "", replacement); result.Code != http.StatusPreconditionRequired {
		t.Fatal("POST without If-Match accepted")
	}
	if result := request(t, handler, http.MethodPost, token, "\"stale\"", replacement); result.Code != http.StatusPreconditionFailed {
		t.Fatal("stale POST accepted")
	}
	if result := request(t, handler, http.MethodPost, token, entityTag(original), nil); result.Code != http.StatusBadRequest {
		t.Fatal("empty POST accepted")
	}
	result := request(t, handler, http.MethodPost, token, entityTag(original), replacement)
	saved, _ := os.ReadFile(handler.path)
	if result.Code != http.StatusNoContent || !bytes.Equal(saved, replacement) ||
		result.Header().Get("ETag") != entityTag(replacement) {
		t.Fatal("POST did not replace the file")
	}
	retry := request(t, handler, http.MethodPost, token, entityTag(original), replacement)
	if retry.Code != http.StatusNoContent {
		t.Fatal("retry of a committed request failed")
	}
	configAfter, _ := os.ReadFile(filepath.Join(directory, "credentials.json"))
	if !bytes.Equal(configBefore, configAfter) {
		t.Fatal("file upload replaced server credentials")
	}
	temporary, _ := filepath.Glob(filepath.Join(directory, ".appvault-write-*"))
	if len(temporary) != 0 {
		t.Fatal("temporary file leaked")
	}
}

func TestConcurrentClients(t *testing.T) {
	_, token, handler := fixture(t)
	server := httptest.NewServer(handler)
	defer server.Close()
	original, _ := os.ReadFile(handler.path)
	outcomes := make(chan int, 2)
	var wg sync.WaitGroup
	for _, body := range []string{"encrypted client A", "encrypted client B"} {
		wg.Add(1)
		go func(body string) {
			defer wg.Done()
			r, err := http.NewRequest(http.MethodPost, server.URL+"/vault", strings.NewReader(body))
			if err != nil {
				outcomes <- 0
				return
			}
			r.Header.Set("Authorization", "Bearer "+token)
			r.Header.Set("If-Match", entityTag(original))
			response, err := server.Client().Do(r)
			if err != nil {
				outcomes <- 0
				return
			}
			defer response.Body.Close()
			_, _ = io.Copy(io.Discard, response.Body)
			outcomes <- response.StatusCode
		}(body)
	}
	wg.Wait()
	close(outcomes)
	statuses := map[int]int{}
	for result := range outcomes {
		statuses[result]++
	}
	if statuses[http.StatusNoContent] != 1 || statuses[http.StatusPreconditionFailed] != 1 {
		t.Fatalf("unexpected concurrent outcomes: %v", statuses)
	}
}

func TestLimitsMethodsAndTokenRotation(t *testing.T) {
	directory, token, handler := fixture(t)
	original, _ := os.ReadFile(handler.path)
	oversized := bytes.Repeat([]byte{1}, int(maxFileSize+1))
	if response := request(t, handler, http.MethodPost, token, entityTag(original), oversized); response.Code != http.StatusRequestEntityTooLarge {
		t.Fatal("oversized upload accepted")
	}
	if response := request(t, handler, http.MethodDelete, token, "", nil); response.Code != http.StatusMethodNotAllowed {
		t.Fatal("unexpected method accepted")
	}
	unlock, err := acquireServerLock(directory)
	if err != nil {
		t.Fatal(err)
	}
	if release, err := acquireServerLock(directory); err == nil {
		release()
		t.Fatal("two servers acquired the same directory")
	}
	if _, err := rotateToken(directory); err == nil {
		t.Fatal("rotation accepted while server is using credentials")
	}
	unlock()
	next, err := rotateToken(directory)
	if err != nil || next == token {
		t.Fatal("token rotation failed", err)
	}
	handler, err = newHandler(directory)
	if err != nil {
		t.Fatal(err)
	}
	if response := request(t, handler, http.MethodGet, token, "", nil); response.Code != http.StatusUnauthorized {
		t.Fatal("old token still accepted")
	}
	if response := request(t, handler, http.MethodGet, next, "", nil); response.Code != http.StatusOK ||
		!bytes.Equal(response.Body.Bytes(), original) {
		t.Fatal("new token could not read original file")
	}
}

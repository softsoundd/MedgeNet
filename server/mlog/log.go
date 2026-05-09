package mlog

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	maxLogFiles        = 10
	logTimestampLayout = "20060102_150405"
)

type Logger struct {
	mu   sync.Mutex
	file *os.File
	path string
}

func New(path string) (*Logger, error) {
	dir := filepath.Dir(path)
	if dir != "." {
		if err := os.MkdirAll(dir, 0755); err != nil {
			return nil, err
		}
	}

	ext := filepath.Ext(path)
	base := path[:len(path)-len(ext)]
	if ext == "" {
		ext = ".log"
	}
	actual := fmt.Sprintf("%s_%s%s", base, time.Now().Format(logTimestampLayout), ext)
	f, err := os.OpenFile(actual, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		return nil, err
	}

	l := &Logger{file: f, path: actual}
	l.Write("INIT", "Log file: "+actual)
	cleanupOldLogs(base, ext, actual, maxLogFiles)
	return l, nil
}

type generatedLog struct {
	path      string
	timestamp time.Time
}

func cleanupOldLogs(base string, ext string, current string, keep int) {
	if keep < 1 {
		return
	}

	dir := filepath.Dir(base)
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}

	prefix := filepath.Base(base) + "_"
	logs := make([]generatedLog, 0, len(entries))
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}

		name := entry.Name()
		if !strings.HasPrefix(name, prefix) || !strings.HasSuffix(name, ext) {
			continue
		}

		stamp := strings.TrimSuffix(strings.TrimPrefix(name, prefix), ext)
		timestamp, err := time.Parse(logTimestampLayout, stamp)
		if err != nil {
			continue
		}

		logs = append(logs, generatedLog{
			path:      filepath.Join(dir, name),
			timestamp: timestamp,
		})
	}

	sort.Slice(logs, func(i int, j int) bool {
		return logs[i].timestamp.After(logs[j].timestamp)
	})
	if len(logs) <= keep {
		return
	}

	current = filepath.Clean(current)
	for _, log := range logs[keep:] {
		if filepath.Clean(log.path) == current {
			continue
		}
		_ = os.Remove(log.path)
	}
}

func (l *Logger) Path() string {
	return l.path
}

func (l *Logger) Close() error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.file == nil {
		return nil
	}
	err := l.file.Close()
	l.file = nil
	return err
}

func (l *Logger) Write(tag string, msg string) {
	line := fmt.Sprintf("[%s] [%s] %s", time.Now().Format("15:04:05.000"), tag, msg)
	l.mu.Lock()
	defer l.mu.Unlock()
	fmt.Println(line)
	if l.file != nil {
		_, _ = l.file.WriteString(line + "\n")
		_ = l.file.Sync()
	}
}

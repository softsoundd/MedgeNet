package config

import (
	"bufio"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type Config struct {
	BindHost            string
	FESLPort            int
	HTTPPort            int
	LockerURL           string
	MemcheckIntervalSec int
	DBPath              string
	GhostDir            string
	LogPath             string
	GameVersion         string
	MaxUploadBytes      int64
	MaxIdentifierLength int
}

func Load() Config {
	cfg := defaults()
	path := getenv("MEDGENET_CONFIG", "server.ini")
	if values, err := readINI(path); err == nil {
		cfg.BindHost = lookup(values, "network.bind_host", cfg.BindHost)
		cfg.FESLPort = lookupInt(values, "network.fesl_port", cfg.FESLPort)
		cfg.HTTPPort = lookupInt(values, "network.http_port", cfg.HTTPPort)
		cfg.LockerURL = lookup(values, "network.locker_url", cfg.LockerURL)
		cfg.MemcheckIntervalSec = lookupInt(values, "network.memcheck_interval", cfg.MemcheckIntervalSec)
		cfg.DBPath = lookup(values, "paths.db_path", cfg.DBPath)
		cfg.GhostDir = lookup(values, "paths.ghost_dir", cfg.GhostDir)
		cfg.LogPath = lookup(values, "paths.log_path", cfg.LogPath)
		cfg.GameVersion = lookup(values, "game.version", cfg.GameVersion)
		cfg.MaxUploadBytes = int64(lookupInt(values, "security.max_upload_bytes", int(cfg.MaxUploadBytes)))
		cfg.MaxIdentifierLength = lookupInt(values, "security.max_identifier_length", cfg.MaxIdentifierLength)
	}

	cfg.BindHost = getenv("MEDGENET_BIND_HOST", cfg.BindHost)
	cfg.FESLPort = getenvInt("MEDGENET_FESL_PORT", cfg.FESLPort)
	cfg.HTTPPort = getenvInt("MEDGENET_HTTP_PORT", cfg.HTTPPort)
	cfg.LockerURL = getenv("MEDGENET_LOCKER_URL", cfg.LockerURL)
	cfg.MemcheckIntervalSec = getenvInt("MEDGENET_MEMCHECK_INTERVAL", cfg.MemcheckIntervalSec)
	cfg.DBPath = getenv("MEDGENET_DB_PATH", cfg.DBPath)
	cfg.GhostDir = getenv("MEDGENET_GHOST_DIR", cfg.GhostDir)
	cfg.LogPath = getenv("MEDGENET_LOG_PATH", cfg.LogPath)
	cfg.GameVersion = getenv("MEDGENET_GAME_VERSION", cfg.GameVersion)
	cfg.MaxUploadBytes = int64(getenvInt("MEDGENET_MAX_UPLOAD_BYTES", int(cfg.MaxUploadBytes)))
	cfg.MaxIdentifierLength = getenvInt("MEDGENET_MAX_IDENTIFIER_LENGTH", cfg.MaxIdentifierLength)

	cfg.DBPath = cleanPath(cfg.DBPath)
	cfg.GhostDir = cleanPath(cfg.GhostDir)
	cfg.LogPath = cleanPath(cfg.LogPath)
	return cfg
}

func defaults() Config {
	return Config{
		BindHost:            "0.0.0.0",
		FESLPort:            18680,
		HTTPPort:            80,
		LockerURL:           "http://easo.ea.com/easo/locker",
		MemcheckIntervalSec: 30,
		DBPath:              "me_server.db",
		GhostDir:            "ghosts",
		LogPath:             "session.log",
		GameVersion:         "1.0.0.0",
		MaxUploadBytes:      1048576,
		MaxIdentifierLength: 64,
	}
}

func readINI(path string) (map[string]string, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	values := map[string]string{}
	section := ""
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			section = strings.TrimSpace(line[1 : len(line)-1])
			continue
		}
		key, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		values[section+"."+strings.TrimSpace(key)] = strings.TrimSpace(value)
	}
	return values, scanner.Err()
}

func lookup(values map[string]string, key string, fallback string) string {
	if value, ok := values[key]; ok && value != "" {
		return value
	}
	return fallback
}

func lookupInt(values map[string]string, key string, fallback int) int {
	value, ok := values[key]
	if !ok {
		return fallback
	}
	parsed, err := strconv.Atoi(value)
	if err != nil {
		return fallback
	}
	return parsed
}

func getenv(name string, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func getenvInt(name string, fallback int) int {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	parsed, err := strconv.Atoi(value)
	if err != nil {
		return fallback
	}
	return parsed
}

func cleanPath(path string) string {
	if filepath.IsAbs(path) {
		return path
	}
	return filepath.Clean(path)
}

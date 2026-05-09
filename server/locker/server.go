package locker

import (
	"context"
	"encoding/json"
	"fmt"
	"hash/crc32"
	"io"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"medgenet-prototype/config"
	"medgenet-prototype/mlog"
	"medgenet-prototype/storage"
)

var (
	ghostNamePattern = regexp.MustCompile(`^tdttghost_(\d+)$`)
	safeIDPattern    = regexp.MustCompile(`^[A-Za-z0-9_. -]+$`)
)

type Server struct {
	cfg   config.Config
	log   *mlog.Logger
	store *storage.Store
}

func NewServer(cfg config.Config, log *mlog.Logger, store *storage.Store) *Server {
	return &Server{cfg: cfg, log: log, store: store}
}

func (s *Server) Listen(ctx context.Context) error {
	addr := fmt.Sprintf("%s:%d", s.cfg.BindHost, s.cfg.HTTPPort)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	defer ln.Close()
	s.log.Write("HTTP", "Listening on "+addr)

	go func() {
		<-ctx.Done()
		_ = ln.Close()
	}()

	for {
		conn, err := ln.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			s.log.Write("HTTP", "Accept error: "+err.Error())
			continue
		}
		go s.handleConnection(conn)
	}
}

func (s *Server) handleConnection(conn net.Conn) {
	defer conn.Close()
	buf := []byte{}
	for {
		_ = conn.SetReadDeadline(time.Now().Add(30 * time.Second))
		tmp := make([]byte, 65536)
		n, err := conn.Read(tmp)
		if err != nil {
			if err != io.EOF {
				return
			}
			return
		}
		buf = append(buf, tmp[:n]...)
		for {
			req, rest, ok := parseHTTPRequest(buf)
			if !ok {
				break
			}
			buf = rest
			s.log.Write("HTTP", req.method+" "+strings.Split(req.path, "?")[0])
			code, body, contentType := s.handleRequest(req.method, req.path, req.headers, req.body)
			_, _ = conn.Write(buildHTTPResponse(code, body, contentType))
		}
	}
}

func (s *Server) handleRequest(method string, path string, headers map[string]string, body []byte) (int, []byte, string) {
	_ = method
	_ = headers
	parsed, err := url.Parse(path)
	if err != nil {
		return 404, nil, "text/xml"
	}
	params := parsed.Query()

	if parsed.Path == "/healthz" {
		return 200, []byte("ok\n"), "text/plain"
	}
	if strings.Contains(parsed.Path, "/easo/editorial") || strings.Contains(parsed.Path, "/version") {
		return 200, []byte(s.cfg.GameVersion), "text/xml"
	}

	cmd := params.Get("cmd")
	pers := params.Get("pers")
	switch cmd {
	case "dir":
		if !s.safeIdentifier(pers) {
			s.log.Write("HTTP", fmt.Sprintf("FileLocker dir rejected unsafe pers=%q", pers))
			return 200, []byte(`<LOCKER error="1"/>`), "text/xml"
		}
		entries, err := s.ghostEntries(pers)
		if err != nil {
			s.log.Write("HTTP", "FileLocker dir error: "+err.Error())
			return 200, []byte(`<LOCKER error="1"/>`), "text/xml"
		}
		s.log.Write("HTTP", fmt.Sprintf("FileLocker dir: returning %d file(s) for %s", len(entries), pers))
		return 200, []byte(BuildXML(entries, params.Get("game"), pers)), "text/xml"

	case "nfo":
		name := params.Get("name")
		if !s.safeIdentifier(pers) || (name != "" && !s.safeIdentifier(name)) {
			return 200, []byte(`<LOCKER error="1"/>`), "text/xml"
		}
		entries, err := s.ghostEntries(pers)
		if err != nil {
			return 200, []byte(`<LOCKER error="1"/>`), "text/xml"
		}
		if name != "" {
			filtered := entries[:0]
			for _, entry := range entries {
				if entry.Name == name {
					filtered = append(filtered, entry)
				}
			}
			entries = filtered
		}
		return 200, []byte(BuildXML(entries, params.Get("game"), pers)), "text/xml"

	case "get":
		name := params.Get("name")
		if !s.safeIdentifier(pers) || !s.safeIdentifier(name) {
			return 404, nil, "application/octet-stream"
		}
		entries, err := s.ghostEntries(pers)
		if err != nil {
			return 404, nil, "application/octet-stream"
		}
		for _, entry := range entries {
			if entry.Name == name {
				data, err := os.ReadFile(entry.path)
				if err != nil {
					return 404, nil, "application/octet-stream"
				}
				return 200, data, "application/octet-stream"
			}
		}
		return 404, nil, "application/octet-stream"

	case "put":
		name := params.Get("name")
		if int64(len(body)) > s.cfg.MaxUploadBytes {
			s.log.Write("HTTP", fmt.Sprintf("FileLocker put rejected oversize upload: %d bytes", len(body)))
			return 200, []byte(`<LOCKER error="1"/>`), "text/xml"
		}
		if !s.safeIdentifier(pers) || !s.safeIdentifier(name) {
			return 200, []byte(`<LOCKER error="1"/>`), "text/xml"
		}
		if err := os.MkdirAll(s.cfg.GhostDir, 0755); err != nil {
			return 200, []byte(`<LOCKER error="1"/>`), "text/xml"
		}
		filePath := filepath.Join(s.cfg.GhostDir, pers+"_"+name)
		if err := os.WriteFile(filePath, body, 0644); err != nil {
			return 200, []byte(`<LOCKER error="1"/>`), "text/xml"
		}
		now := time.Now().Unix()
		entry := Entry{
			Name:   name,
			Desc:   params.Get("desc"),
			Game:   params.Get("game"),
			Type:   params.Get("type"),
			IDNT:   params.Get("idnt"),
			Date:   now,
			FileID: int64(crc32.ChecksumIEEE([]byte(pers+"_"+name)) & 0x7fffffff),
			Attr:   atoi(params.Get("attr")),
			Size:   int64(len(body)),
			Locs:   atoi(params.Get("locs")),
			Perm:   atoi(params.Get("perm")),
			Vers:   atoi(params.Get("vers")),
		}
		if err := saveMeta(filePath, entry); err != nil {
			return 200, []byte(`<LOCKER error="1"/>`), "text/xml"
		}
		s.log.Write("HTTP", fmt.Sprintf("Saved ghost: %s", filePath))
		return 200, []byte(BuildXML([]Entry{entry}, "", "")), "text/xml"
	}

	return 200, []byte(`<LOCKER error="0"/>`), "text/xml"
}

func (s *Server) ghostEntries(pers string) ([]Entry, error) {
	if pers == "" {
		return nil, nil
	}
	entries, err := s.collectGhostEntries(pers + "_")
	if err != nil || len(entries) > 0 {
		return withStretchAliases(s.store, entries)
	}
	all, err := s.collectGhostEntries("")
	if err != nil {
		return nil, err
	}
	return withStretchAliases(s.store, all)
}

func (s *Server) collectGhostEntries(prefix string) ([]Entry, error) {
	files, err := os.ReadDir(s.cfg.GhostDir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	sort.Slice(files, func(i, j int) bool { return files[i].Name() < files[j].Name() })
	entries := []Entry{}
	for _, file := range files {
		name := file.Name()
		if file.IsDir() || strings.HasSuffix(name, ".json") {
			continue
		}
		if prefix != "" && !strings.HasPrefix(name, prefix) {
			continue
		}
		path := filepath.Join(s.cfg.GhostDir, name)
		info, err := file.Info()
		if err != nil {
			return nil, err
		}
		ghostName := name
		if prefix != "" {
			ghostName = strings.TrimPrefix(name, prefix)
		} else if _, rest, ok := strings.Cut(name, "_"); ok {
			ghostName = rest
		}
		entry := Entry{
			Name:   ghostName,
			Game:   "MEPC",
			Date:   info.ModTime().Unix(),
			FileID: int64(crc32.ChecksumIEEE([]byte(name)) & 0x7fffffff),
			Size:   info.Size(),
			path:   path,
		}
		meta, err := loadMeta(path)
		if err == nil {
			if meta.Name != "" {
				entry.Name = meta.Name
			}
			entry.Desc = meta.Desc
			if meta.Game != "" {
				entry.Game = meta.Game
			}
			entry.Type = meta.Type
			entry.IDNT = meta.IDNT
			if meta.Date != 0 {
				entry.Date = meta.Date
			}
			if meta.FileID != 0 {
				entry.FileID = meta.FileID
			}
			entry.Attr = meta.Attr
			entry.Locs = meta.Locs
			entry.Perm = meta.Perm
			entry.Vers = meta.Vers
		}
		entries = append(entries, entry)
	}
	return entries, nil
}

func withStretchAliases(store *storage.Store, entries []Entry) ([]Entry, error) {
	tagToStretches, err := store.GhostTagToStretches()
	if err != nil || len(tagToStretches) == 0 {
		return entries, err
	}
	out := append([]Entry{}, entries...)
	names := map[string]bool{}
	for _, entry := range out {
		names[entry.Name] = true
	}
	for _, entry := range entries {
		match := ghostNamePattern.FindStringSubmatch(entry.Name)
		if len(match) != 2 {
			continue
		}
		tag, _ := strconv.Atoi(match[1])
		for _, stretchID := range tagToStretches[tag] {
			aliasName := fmt.Sprintf("tdttghost_%d", stretchID)
			if names[aliasName] {
				continue
			}
			alias := entry
			alias.Name = aliasName
			out = append(out, alias)
			names[aliasName] = true
		}
	}
	return out, nil
}

func (s *Server) safeIdentifier(value string) bool {
	if value == "" || len(value) > s.cfg.MaxIdentifierLength {
		return false
	}
	if strings.Contains(value, "/") || strings.Contains(value, "\\") || strings.Contains(value, "\x00") || strings.Contains(value, "..") {
		return false
	}
	return safeIDPattern.MatchString(value)
}

type httpRequest struct {
	method  string
	path    string
	headers map[string]string
	body    []byte
}

func parseHTTPRequest(data []byte) (httpRequest, []byte, bool) {
	idx := strings.Index(string(data), "\r\n\r\n")
	if idx < 0 {
		return httpRequest{}, data, false
	}
	headerEnd := idx + 4
	headerBlock := string(data[:idx])
	lines := strings.Split(headerBlock, "\r\n")
	parts := strings.SplitN(lines[0], " ", 3)
	if len(parts) < 2 {
		return httpRequest{}, data, false
	}
	headers := map[string]string{}
	for _, line := range lines[1:] {
		k, v, ok := strings.Cut(line, ":")
		if ok {
			headers[strings.ToLower(strings.TrimSpace(k))] = strings.TrimSpace(v)
		}
	}
	contentLen := int(atoi(headers["content-length"]))
	total := headerEnd + contentLen
	if len(data) < total {
		return httpRequest{}, data, false
	}
	return httpRequest{method: parts[0], path: parts[1], headers: headers, body: data[headerEnd:total]}, data[total:], true
}

func buildHTTPResponse(code int, body []byte, contentType string) []byte {
	reason := map[int]string{200: "OK", 404: "Not Found"}[code]
	if reason == "" {
		reason = "OK"
	}
	header := fmt.Sprintf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: keep-alive\r\nContent-Length: %d\r\n\r\n", code, reason, contentType, len(body))
	return append([]byte(header), body...)
}

func loadMeta(path string) (Entry, error) {
	data, err := os.ReadFile(path + ".json")
	if err != nil {
		return Entry{}, err
	}
	var entry Entry
	err = json.Unmarshal(data, &entry)
	return entry, err
}

func saveMeta(path string, entry Entry) error {
	data, err := json.Marshal(entry)
	if err != nil {
		return err
	}
	return os.WriteFile(path+".json", data, 0644)
}

func atoi(value string) int64 {
	i, _ := strconv.ParseInt(value, 10, 64)
	return i
}

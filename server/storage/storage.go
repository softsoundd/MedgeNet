package storage

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"
	"time"

	_ "modernc.org/sqlite"
)

const (
	passwordScheme              = "pbkdf2_sha256"
	passwordIterations          = 260000
	HistoricalOwnerOffset int64 = 1000000000
	MaxHistoricalNameLen        = 31
	loginTokenBytes             = 112
	maxLoginTokenLen            = 256
)

var (
	ErrAccountExists    = errors.New("account already exists")
	ErrPersonaNameTaken = errors.New("persona name is already owned by another account")
)

type Account struct {
	ID       int64
	NUID     string
	Password string
}

type Persona struct {
	ID        int64
	AccountID int64
	Name      string
	CreatedAt string
}

type Association struct {
	ID   int64
	Name string
}

type RankedStat struct {
	Rank  int
	Value float64
}

type TopEntry struct {
	PersonaID   int64
	PersonaName string
	Rank        int
	Values      map[string]float64
	Imported    bool
}

type Store struct {
	db *sql.DB
}

func Open(path string) (*Store, error) {
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, err
	}
	s := &Store{db: db}
	if err := s.Init(); err != nil {
		_ = db.Close()
		return nil, err
	}
	return s, nil
}

func (s *Store) Close() error {
	return s.db.Close()
}

func (s *Store) Init() error {
	_, err := s.db.Exec(`
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS accounts (
    id INTEGER PRIMARY KEY,
    nuid TEXT UNIQUE NOT NULL,
    password TEXT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS personas (
    id INTEGER PRIMARY KEY,
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    name TEXT UNIQUE NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS stats (
    persona_id INTEGER NOT NULL REFERENCES personas(id),
    key TEXT NOT NULL,
    value REAL NOT NULL DEFAULT 0,
    period_id INTEGER NOT NULL DEFAULT 0,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (persona_id, key, period_id)
);
CREATE TABLE IF NOT EXISTS friends (
    persona_id INTEGER NOT NULL REFERENCES personas(id),
    friend_persona_id INTEGER NOT NULL REFERENCES personas(id),
    type TEXT NOT NULL DEFAULT 'PlasmaFriends',
    PRIMARY KEY (persona_id, friend_persona_id, type)
);
CREATE TABLE IF NOT EXISTS login_tokens (
    token_hash TEXT PRIMARY KEY,
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_used_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS historical_players (
    id INTEGER PRIMARY KEY,
    source_key TEXT UNIQUE NOT NULL,
    name TEXT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS historical_runs (
    player_id INTEGER NOT NULL REFERENCES historical_players(id),
    stretch_id INTEGER NOT NULL,
    key TEXT NOT NULL,
    value REAL NOT NULL,
    period_id INTEGER NOT NULL DEFAULT 0,
    run_id TEXT NOT NULL,
    weblink TEXT NOT NULL DEFAULT '',
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (player_id, key, period_id)
);
`)
	return err
}

func (s *Store) CreateAccount(nuid string, password string) (*Account, error) {
	res, err := s.db.Exec("INSERT OR IGNORE INTO accounts (nuid, password) VALUES (?, ?)", nuid, hashPassword(password))
	if err != nil {
		return nil, err
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return nil, err
	}
	if affected == 0 {
		return nil, ErrAccountExists
	}
	return s.GetAccountByNUID(nuid)
}

func (s *Store) GetAccountByNUID(nuid string) (*Account, error) {
	row := s.db.QueryRow("SELECT id, nuid, password FROM accounts WHERE nuid = ?", nuid)
	var account Account
	if err := row.Scan(&account.ID, &account.NUID, &account.Password); err != nil {
		if err == sql.ErrNoRows {
			return nil, nil
		}
		return nil, err
	}
	return &account, nil
}

func (s *Store) ValidateAccount(nuid string, password string) (*Account, error) {
	account, err := s.GetAccountByNUID(nuid)
	if err != nil || account == nil {
		return nil, err
	}
	if !verifyPassword(password, account.Password) {
		return nil, nil
	}
	if !strings.HasPrefix(account.Password, passwordScheme+"$") {
		if _, err := s.db.Exec("UPDATE accounts SET password = ? WHERE id = ?", hashPassword(password), account.ID); err != nil {
			return nil, err
		}
		return s.GetAccountByNUID(nuid)
	}
	return account, nil
}

func (s *Store) IssueLoginToken(accountID int64) (string, error) {
	token, err := generateLoginToken()
	if err != nil {
		return "", err
	}
	if err := s.RevokeLoginTokens(accountID); err != nil {
		return "", err
	}
	_, err = s.db.Exec(`
INSERT INTO login_tokens (token_hash, account_id, created_at, last_used_at)
VALUES (?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)
`, hashLoginToken(token), accountID)
	if err != nil {
		return "", err
	}
	return token, nil
}

func (s *Store) AccountByLoginToken(token string) (*Account, error) {
	if !ValidLoginToken(token) {
		return nil, nil
	}
	row := s.db.QueryRow(`
SELECT a.id, a.nuid, a.password
FROM login_tokens t
JOIN accounts a ON a.id = t.account_id
WHERE t.token_hash = ?
`, hashLoginToken(token))
	var account Account
	if err := row.Scan(&account.ID, &account.NUID, &account.Password); err != nil {
		if err == sql.ErrNoRows {
			return nil, nil
		}
		return nil, err
	}
	if _, err := s.db.Exec("UPDATE login_tokens SET last_used_at = CURRENT_TIMESTAMP WHERE token_hash = ?", hashLoginToken(token)); err != nil {
		return nil, err
	}
	return &account, nil
}

func (s *Store) RevokeLoginTokens(accountID int64) error {
	_, err := s.db.Exec("DELETE FROM login_tokens WHERE account_id = ?", accountID)
	return err
}

func ValidLoginToken(token string) bool {
	if token == "" || len(token) > maxLoginTokenLen {
		return false
	}
	for _, r := range token {
		if r < 0x21 || r > 0x7e {
			return false
		}
	}
	return true
}

func LoginTokenHashPrefix(token string) string {
	hash := hashLoginToken(token)
	if len(hash) < 12 {
		return hash
	}
	return hash[:12]
}

func (s *Store) Personas(accountID int64) ([]Persona, error) {
	rows, err := s.db.Query("SELECT id, account_id, name, created_at FROM personas WHERE account_id = ? ORDER BY id", accountID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var personas []Persona
	for rows.Next() {
		var persona Persona
		if err := rows.Scan(&persona.ID, &persona.AccountID, &persona.Name, &persona.CreatedAt); err != nil {
			return nil, err
		}
		personas = append(personas, persona)
	}
	return personas, rows.Err()
}

func (s *Store) GetOrCreatePersona(accountID int64, name string) (*Persona, error) {
	if existing, err := s.LookupPersonaByName(name); err != nil {
		return nil, err
	} else if existing != nil {
		if existing.AccountID != accountID {
			return nil, ErrPersonaNameTaken
		}
		return existing, nil
	}
	if _, err := s.db.Exec("INSERT OR IGNORE INTO personas (account_id, name) VALUES (?, ?)", accountID, name); err != nil {
		return nil, err
	}
	persona, err := s.LookupPersonaByName(name)
	if err != nil {
		return nil, err
	}
	if persona == nil {
		return nil, sql.ErrNoRows
	}
	if persona.AccountID != accountID {
		return nil, ErrPersonaNameTaken
	}
	return persona, nil
}

func (s *Store) PersonaByID(personaID int64) (*Persona, error) {
	row := s.db.QueryRow("SELECT id, account_id, name, created_at FROM personas WHERE id = ?", personaID)
	var persona Persona
	if err := row.Scan(&persona.ID, &persona.AccountID, &persona.Name, &persona.CreatedAt); err != nil {
		if err == sql.ErrNoRows {
			return nil, nil
		}
		return nil, err
	}
	return &persona, nil
}

func (s *Store) PersonaExists(personaID int64) (bool, error) {
	var one int
	err := s.db.QueryRow("SELECT 1 FROM personas WHERE id = ?", personaID).Scan(&one)
	if err != nil {
		if err == sql.ErrNoRows {
			return false, nil
		}
		return false, err
	}
	return true, nil
}

func (s *Store) LookupPersonaByName(name string) (*Persona, error) {
	row := s.db.QueryRow("SELECT id, account_id, name, created_at FROM personas WHERE name = ?", name)
	var persona Persona
	if err := row.Scan(&persona.ID, &persona.AccountID, &persona.Name, &persona.CreatedAt); err != nil {
		if err == sql.ErrNoRows {
			return nil, nil
		}
		return nil, err
	}
	return &persona, nil
}

func (s *Store) GetStats(personaID int64, keys []string, periodID int) (map[string]float64, error) {
	out := map[string]float64{}
	if len(keys) == 0 {
		return out, nil
	}
	placeholders := strings.TrimRight(strings.Repeat("?,", len(keys)), ",")
	args := []any{personaID, periodID}
	for _, key := range keys {
		args = append(args, key)
	}
	dateClause, cutoff := periodDateClause(periodID)
	if cutoff != "" {
		args = append(args, cutoff)
	}
	rows, err := s.db.Query(fmt.Sprintf("SELECT key, value FROM stats WHERE persona_id = ? AND period_id = ? AND key IN (%s)%s", placeholders, dateClause), args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var key string
		var value float64
		if err := rows.Scan(&key, &value); err != nil {
			return nil, err
		}
		out[key] = value
	}
	return out, rows.Err()
}

func (s *Store) UpdateStats(personaID int64, values map[string]float64, periodID int) error {
	for key, value := range values {
		if _, err := s.db.Exec(`
INSERT INTO stats (persona_id, key, value, period_id, updated_at)
VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)
ON CONFLICT(persona_id, key, period_id)
DO UPDATE SET value = excluded.value, updated_at = CURRENT_TIMESTAMP
`, personaID, key, value, periodID); err != nil {
			return err
		}
	}
	return nil
}

func (s *Store) RankedStats(personaID int64, key string, periodID int) (RankedStat, error) {
	dateClause, cutoff := periodDateClause(periodID)
	args := []any{personaID, key, periodID}
	if cutoff != "" {
		args = append(args, cutoff)
	}
	var value float64
	if err := s.db.QueryRow("SELECT value FROM stats WHERE persona_id = ? AND key = ? AND period_id = ?"+dateClause, args...).Scan(&value); err != nil {
		if err == sql.ErrNoRows {
			return RankedStat{}, nil
		}
		return RankedStat{}, err
	}
	var rank int
	rankArgs := []any{key, periodID, value}
	if cutoff != "" {
		rankArgs = append(rankArgs, cutoff)
	}
	if err := s.db.QueryRow("SELECT COUNT(*) FROM stats WHERE key = ? AND period_id = ? AND value > 0 AND value <= ?"+dateClause, rankArgs...).Scan(&rank); err != nil {
		return RankedStat{}, err
	}
	return RankedStat{Rank: int(math.Max(float64(rank), 1)), Value: value}, nil
}

func (s *Store) TopN(key string, minRank int, maxRank int, extraKeys []string, periodID int) ([]TopEntry, error) {
	offset := max(minRank-1, 0)
	limit := max(maxRank-offset, 1)
	liveDateClause, cutoff := periodDateClauseFor(periodID, "s.updated_at")
	historicalDateClause, _ := periodDateClauseFor(periodID, "hr.updated_at")
	args := []any{key, periodID}
	if cutoff != "" {
		args = append(args, cutoff)
	}
	args = append(args, key, periodID)
	if cutoff != "" {
		args = append(args, cutoff)
	}
	args = append(args, limit, offset)
	rows, err := s.db.Query(`
SELECT owner_id, name, value, imported FROM (
SELECT s.persona_id AS owner_id, p.name AS name, s.value AS value, 0 AS imported
FROM stats s
JOIN personas p ON p.id = s.persona_id
WHERE s.key = ? AND s.period_id = ? AND s.value > 0`+liveDateClause+`
UNION ALL
SELECT hp.id + `+strconv.FormatInt(HistoricalOwnerOffset, 10)+` AS owner_id, hp.name AS name, hr.value AS value, 1 AS imported
FROM historical_runs hr
JOIN historical_players hp ON hp.id = hr.player_id
WHERE hr.key = ? AND hr.period_id = ? AND hr.value > 0`+historicalDateClause+`
)
ORDER BY value ASC
LIMIT ? OFFSET ?
`, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var entries []TopEntry
	for i := 0; rows.Next(); i++ {
		entry := TopEntry{Rank: offset + i + 1, Values: map[string]float64{}}
		var value float64
		if err := rows.Scan(&entry.PersonaID, &entry.PersonaName, &value, &entry.Imported); err != nil {
			return nil, err
		}
		if entry.Imported {
			entry.PersonaName = SanitizeHistoricalName(entry.PersonaName)
		}
		entry.Values[key] = value
		extra, err := s.GetStats(entry.PersonaID, extraKeys, periodID)
		if err != nil {
			return nil, err
		}
		for k, v := range extra {
			entry.Values[k] = v
		}
		entries = append(entries, entry)
	}
	return entries, rows.Err()
}

func SanitizeHistoricalName(name string) string {
	var b strings.Builder
	for _, r := range strings.TrimSpace(name) {
		if r < 0x20 || r > 0x7e {
			continue
		}
		if strings.ContainsRune("=\r\n\t", r) {
			continue
		}
		b.WriteRune(r)
		if b.Len() >= MaxHistoricalNameLen {
			break
		}
	}
	out := strings.TrimSpace(b.String())
	if out == "" {
		return "SRC Runner"
	}
	return out
}

func IsHistoricalOwner(ownerID int64) bool {
	return ownerID >= HistoricalOwnerOffset
}

func (s *Store) HistoricalStats(ownerID int64, keys []string, periodID int) (map[string]float64, error) {
	out := map[string]float64{}
	if len(keys) == 0 || !IsHistoricalOwner(ownerID) {
		return out, nil
	}
	placeholders := strings.TrimRight(strings.Repeat("?,", len(keys)), ",")
	playerID := ownerID - HistoricalOwnerOffset
	args := []any{playerID, periodID}
	for _, key := range keys {
		args = append(args, key)
	}
	dateClause, cutoff := periodDateClauseFor(periodID, "updated_at")
	if cutoff != "" {
		args = append(args, cutoff)
	}
	rows, err := s.db.Query(fmt.Sprintf("SELECT key, value FROM historical_runs WHERE player_id = ? AND period_id = ? AND key IN (%s)%s", placeholders, dateClause), args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var key string
		var value float64
		if err := rows.Scan(&key, &value); err != nil {
			return nil, err
		}
		out[key] = value
	}
	return out, rows.Err()
}

func (s *Store) HistoricalRankedStats(ownerID int64, key string, periodID int) (RankedStat, error) {
	if !IsHistoricalOwner(ownerID) {
		return RankedStat{}, nil
	}
	playerID := ownerID - HistoricalOwnerOffset
	dateClause, cutoff := periodDateClauseFor(periodID, "updated_at")
	args := []any{playerID, key, periodID}
	if cutoff != "" {
		args = append(args, cutoff)
	}
	var value float64
	if err := s.db.QueryRow("SELECT value FROM historical_runs WHERE player_id = ? AND key = ? AND period_id = ?"+dateClause, args...).Scan(&value); err != nil {
		if err == sql.ErrNoRows {
			return RankedStat{}, nil
		}
		return RankedStat{}, err
	}

	liveDateClause, _ := periodDateClauseFor(periodID, "updated_at")
	historicalDateClause, _ := periodDateClauseFor(periodID, "updated_at")
	rankArgs := []any{key, periodID, value}
	if cutoff != "" {
		rankArgs = append(rankArgs, cutoff)
	}
	rankArgs = append(rankArgs, key, periodID, value)
	if cutoff != "" {
		rankArgs = append(rankArgs, cutoff)
	}
	var rank int
	if err := s.db.QueryRow(`
SELECT COUNT(*) FROM (
SELECT value FROM stats WHERE key = ? AND period_id = ? AND value > 0 AND value <= ?`+liveDateClause+`
UNION ALL
SELECT value FROM historical_runs WHERE key = ? AND period_id = ? AND value > 0 AND value <= ?`+historicalDateClause+`
)`, rankArgs...).Scan(&rank); err != nil {
		return RankedStat{}, err
	}
	return RankedStat{Rank: int(math.Max(float64(rank), 1)), Value: value}, nil
}

func (s *Store) AddHistoricalRun(sourceKey string, name string, stretchID int, key string, value float64, periodID int, runID string, weblink string, updatedAt string) error {
	if _, err := s.db.Exec("INSERT OR IGNORE INTO historical_players (source_key, name) VALUES (?, ?)", sourceKey, name); err != nil {
		return err
	}
	_, err := s.db.Exec(`
INSERT OR REPLACE INTO historical_runs (player_id, stretch_id, key, value, period_id, run_id, weblink, updated_at)
VALUES ((SELECT id FROM historical_players WHERE source_key = ?), ?, ?, ?, ?, ?, ?, ?)
`, sourceKey, stretchID, key, value, periodID, runID, weblink, updatedAt)
	return err
}

func (s *Store) GetAssociations(personaID int64, assocType string) ([]Association, error) {
	rows, err := s.db.Query(`
SELECT f.friend_persona_id, p.name
FROM friends f
JOIN personas p ON p.id = f.friend_persona_id
WHERE f.persona_id = ? AND f.type = ?
ORDER BY p.name
`, personaID, assocType)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Association
	for rows.Next() {
		var assoc Association
		if err := rows.Scan(&assoc.ID, &assoc.Name); err != nil {
			return nil, err
		}
		out = append(out, assoc)
	}
	return out, rows.Err()
}

func (s *Store) AddAssociation(personaID int64, friendPersonaID int64, assocType string) (bool, error) {
	res, err := s.db.Exec("INSERT OR IGNORE INTO friends (persona_id, friend_persona_id, type) VALUES (?, ?, ?)", personaID, friendPersonaID, assocType)
	if err != nil {
		return false, err
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return false, err
	}
	return affected > 0, nil
}

func (s *Store) DeleteAssociation(personaID int64, friendPersonaID int64, assocType string) error {
	_, err := s.db.Exec("DELETE FROM friends WHERE persona_id = ? AND friend_persona_id = ? AND type = ?", personaID, friendPersonaID, assocType)
	return err
}

func (s *Store) GhostTagToStretches() (map[int][]int, error) {
	rows, err := s.db.Query("SELECT key, value FROM stats WHERE key LIKE 'TD_%_41'")
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := map[int][]int{}
	for rows.Next() {
		var key string
		var value float64
		if err := rows.Scan(&key, &value); err != nil {
			return nil, err
		}
		parts := strings.Split(key, "_")
		if len(parts) != 3 || parts[0] != "TD" || parts[2] != "41" {
			continue
		}
		stretchID, err := strconv.Atoi(parts[1])
		if err != nil {
			continue
		}
		tag := int(value)
		if tag <= 0 {
			continue
		}
		out[tag] = append(out[tag], stretchID)
	}
	return out, rows.Err()
}

func periodDateClause(periodID int) (string, string) {
	return periodDateClauseFor(periodID, "updated_at")
}

func periodDateClauseFor(periodID int, column string) (string, string) {
	cutoff, ok := periodCutoff(periodID, time.Now().UTC())
	if !ok {
		return "", ""
	}
	return " AND " + column + " >= ?", cutoff.Format("2006-01-02 15:04:05")
}

func periodCutoff(periodID int, now time.Time) (time.Time, bool) {
	now = now.UTC()
	switch periodID {
	case 2:
		return time.Date(now.Year(), now.Month(), 1, 0, 0, 0, 0, time.UTC), true
	case 4:
		weekday := int(now.Weekday())
		if weekday == 0 {
			weekday = 7
		}
		start := time.Date(now.Year(), now.Month(), now.Day(), 0, 0, 0, 0, time.UTC)
		return start.AddDate(0, 0, -(weekday - 1)), true
	default:
		return time.Time{}, false
	}
}

func hashPassword(password string) string {
	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		panic(err)
	}
	digest := pbkdf2SHA256([]byte(password), salt, passwordIterations, 32)
	return fmt.Sprintf("%s$%d$%s$%s", passwordScheme, passwordIterations, hex.EncodeToString(salt), hex.EncodeToString(digest))
}

func verifyPassword(password string, stored string) bool {
	parts := strings.Split(stored, "$")
	if len(parts) != 4 || parts[0] != passwordScheme {
		return hmac.Equal([]byte(stored), []byte(password))
	}
	iterations, err := strconv.Atoi(parts[1])
	if err != nil {
		return false
	}
	salt, err := hex.DecodeString(parts[2])
	if err != nil {
		return false
	}
	expected, err := hex.DecodeString(parts[3])
	if err != nil {
		return false
	}
	digest := pbkdf2SHA256([]byte(password), salt, iterations, len(expected))
	return hmac.Equal(digest, expected)
}

func generateLoginToken() (string, error) {
	buf := make([]byte, loginTokenBytes)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	return strings.ReplaceAll(base64.URLEncoding.EncodeToString(buf), "=", "."), nil
}

func hashLoginToken(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

func pbkdf2SHA256(password []byte, salt []byte, iterations int, keyLen int) []byte {
	hLen := sha256.Size
	numBlocks := (keyLen + hLen - 1) / hLen
	var out []byte
	for block := 1; block <= numBlocks; block++ {
		u := prf(password, append(salt, byte(block>>24), byte(block>>16), byte(block>>8), byte(block)))
		t := make([]byte, len(u))
		copy(t, u)
		for i := 1; i < iterations; i++ {
			u = prf(password, u)
			for j := range t {
				t[j] ^= u[j]
			}
		}
		out = append(out, t...)
	}
	return out[:keyLen]
}

func prf(key []byte, data []byte) []byte {
	mac := hmac.New(sha256.New, key)
	mac.Write(data)
	return mac.Sum(nil)
}

package fesl

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net"
	"strconv"
	"strings"
	"time"

	"medgenet-prototype/config"
	"medgenet-prototype/mlog"
	"medgenet-prototype/storage"
)

const noTime = 3599.99
const maxPasswordBytes = 256

type Server struct {
	cfg   config.Config
	log   *mlog.Logger
	store *storage.Store
}

type Session struct {
	conn        net.Conn
	addr        string
	server      *Server
	accountID   int64
	personaID   int64
	personaName string
	nuid        string
	lkey        string
}

type handler func(*Session, Packet) ([]byte, error)

func NewServer(cfg config.Config, log *mlog.Logger, store *storage.Store) *Server {
	return &Server{cfg: cfg, log: log, store: store}
}

func (s *Server) Listen(ctx context.Context) error {
	addr := fmt.Sprintf("%s:%d", s.cfg.BindHost, s.cfg.FESLPort)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	defer ln.Close()
	s.log.Write("FESL", "Listening on "+addr)

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
			s.log.Write("FESL", "Accept error: "+err.Error())
			continue
		}
		session := &Session{conn: conn, addr: conn.RemoteAddr().String(), server: s}
		go session.run(ctx)
	}
}

func (s *Session) run(ctx context.Context) {
	s.log("Connected")
	defer func() {
		_ = s.conn.Close()
		s.log("Disconnected")
	}()

	challenge := Encode("fsys", 0x80000000, kvs(
		"TXN", "MemCheck",
		"memcheck.[]", "0",
		"type", "0",
		"salt", "12345678",
	))
	_, _ = s.conn.Write(challenge)

	buf := []byte{}
	lastMemcheck := time.Now()
	for {
		_ = s.conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		tmp := make([]byte, 8192)
		n, err := s.conn.Read(tmp)
		if err != nil {
			if ctx.Err() != nil || err == io.EOF {
				return
			}
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				if time.Since(lastMemcheck) > time.Duration(s.server.cfg.MemcheckIntervalSec)*time.Second {
					_, _ = s.conn.Write(challenge)
					lastMemcheck = time.Now()
				}
				continue
			}
			s.log("Connection error: " + err.Error())
			return
		}

		buf = append(buf, tmp[:n]...)
		for {
			pkt, rest, complete, err := Decode(buf)
			if err != nil {
				s.log("Decode error: " + err.Error())
				return
			}
			if !complete {
				break
			}
			buf = rest
			params := ParamsMap(pkt.Params)
			txn := params["TXN"]
			s.log(fmt.Sprintf("<< %s TXN=%s id=0x%08x", pkt.Type, txn, pkt.ID))
			if txn == "MemCheck" && pkt.ID == 0x80000000 {
				lastMemcheck = time.Now()
				continue
			}
			resp, err := s.dispatch(pkt)
			if err != nil {
				s.log("Handler error: " + err.Error())
				resp = handlerErrorResponse(pkt, txn)
			}
			if len(resp) > 0 {
				if _, err := s.conn.Write(resp); err == nil {
					s.log(fmt.Sprintf(">> %s (%d bytes)", txn, len(resp)))
				}
			}
		}
	}
}

func (s *Session) dispatch(pkt Packet) ([]byte, error) {
	txn := ParamsMap(pkt.Params)["TXN"]
	handlers := map[string]handler{
		"Hello":                   handleHello,
		"NuLogin":                 handleNuLogin,
		"Login":                   handleNuLogin,
		"NuAddAccount":            handleNuAddAccount,
		"NuGetPersonas":           handleNuGetPersonas,
		"NuLoginPersona":          handleNuLoginPersona,
		"NuAddPersona":            handleNuAddPersona,
		"NuSuggestPersonas":       handleNuSuggestPersonas,
		"NuGetEntitlements":       handleNuGetEntitlements,
		"NuGetEntitlementCount":   handleNuGetEntitlementCount,
		"GameSpyPreAuth":          handleGameSpyPreAuth,
		"GetCountryList":          handleGetCountryList,
		"NuGetTos":                handleNuGetTos,
		"GetLockerURL":            handleGetLockerURL,
		"NuLookupUserInfo":        handleNuLookupUserInfo,
		"GetAssociations":         handleGetAssociations,
		"AddAssociations":         handleAddAssociations,
		"DeleteAssociations":      handleDeleteAssociations,
		"SetPresenceStatus":       handleSetPresenceStatus,
		"PresenceSubscribe":       handlePresenceSubscribe,
		"ModifySettings":          handleModifySettings,
		"GetMessages":             handleGetMessages,
		"GetStats":                handleGetStats,
		"GetRankedStats":          handleGetRankedStats,
		"GetStatsForOwners":       handleGetStatsForOwners,
		"GetRankedStatsForOwners": handleGetRankedStatsForOwners,
		"GetTopNAndStats":         handleGetTopNAndStats,
		"UpdateStats":             handleUpdateStats,
		"SubmitStats":             handleUpdateStats,
	}
	h, ok := handlers[txn]
	if !ok {
		s.log("UNHANDLED TXN=" + txn)
		return Encode(pkt.Type, pkt.ID, kvs("TXN", txn)), nil
	}
	return h(s, pkt)
}

func handleHello(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	s.log(fmt.Sprintf("Hello: clientString=%s sku=%s SDK=%s locale=%s", p["clientString"], p["sku"], p["SDKVersion"], p["locale"]))
	return Encode("fsys", pkt.ID, kvs(
		"TXN", "Hello",
		"domainPartition.domain", "eagames",
		"domainPartition.subDomain", "takedown-pc",
		"curTime", time.Now().UTC().Format("Jan-02-2006 15:04:05 UTC"),
		"activityTimeoutSecs", "3600",
		"messengerIp", "127.0.0.1",
		"messengerPort", "0",
		"theaterIp", "127.0.0.1",
		"theaterPort", "0",
	)), nil
}

func handleNuLogin(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	if token := p["encryptedInfo"]; token != "" {
		account, err := s.server.store.AccountByLoginToken(token)
		if err != nil {
			return nil, err
		}
		if account == nil {
			s.log(fmt.Sprintf("NuLogin failed: invalid encryptedInfo len=%d hash=%s", len(token), storage.LoginTokenHashPrefix(token)))
			return Encode("acct", pkt.ID, kvs("TXN", "NuLogin", "errorCode", "122")), nil
		}
		s.log(fmt.Sprintf("NuLogin encryptedInfo accepted: accountId=%d nuid=%q tokenHash=%s", account.ID, account.NUID, storage.LoginTokenHashPrefix(token)))
		return s.loginResponse(pkt, account, "")
	}

	account, err := s.server.store.ValidateAccount(p["nuid"], p["password"])
	if err != nil {
		return nil, err
	}
	if account == nil {
		s.log("NuLogin failed: invalid credentials for " + p["nuid"])
		return Encode("acct", pkt.ID, kvs("TXN", "NuLogin", "errorCode", "122")), nil
	}
	encryptedLoginInfo := ""
	if p["returnEncryptedInfo"] == "1" {
		token, err := s.server.store.IssueLoginToken(account.ID)
		if err != nil {
			return nil, err
		}
		encryptedLoginInfo = token
		s.log(fmt.Sprintf("NuLogin issued encryptedLoginInfo: accountId=%d len=%d hash=%s", account.ID, len(token), storage.LoginTokenHashPrefix(token)))
	}
	return s.loginResponse(pkt, account, encryptedLoginInfo)
}

func (s *Session) loginResponse(pkt Packet, account *storage.Account, encryptedLoginInfo string) ([]byte, error) {
	s.accountID = account.ID
	s.nuid = account.NUID
	s.lkey = randomHex(20)
	resp := kvs(
		"TXN", "NuLogin",
		"lkey", s.lkey,
		"nuid", account.NUID,
		"profileId", itoa64(account.ID),
		"userId", itoa64(account.ID),
	)
	if encryptedLoginInfo != "" {
		resp = append(resp, KV{Key: "encryptedLoginInfo", Value: encryptedLoginInfo})
	}
	return Encode("acct", pkt.ID, resp), nil
}

func handleNuAddAccount(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	nuid := p["nuid"]
	s.log(fmt.Sprintf("NuAddAccount: nuid=%q len=%d hex=%s", nuid, len(nuid), hex.EncodeToString([]byte(nuid))))
	if reason := validateAccountNUID(nuid, s.server.cfg.MaxIdentifierLength); reason != "" {
		s.log(fmt.Sprintf("NuAddAccount rejected: field=nuid reason=%s len=%d hex=%s", reason, len(nuid), hex.EncodeToString([]byte(nuid))))
		return Encode("acct", pkt.ID, kvs("TXN", "NuAddAccount", "errorCode", "122")), nil
	}
	if reason := validateAccountPassword(p["password"]); reason != "" {
		s.log(fmt.Sprintf("NuAddAccount rejected: field=password reason=%s len=%d", reason, len(p["password"])))
		return Encode("acct", pkt.ID, kvs("TXN", "NuAddAccount", "errorCode", "122")), nil
	}

	account, err := s.server.store.CreateAccount(nuid, p["password"])
	if err != nil {
		if errors.Is(err, storage.ErrAccountExists) {
			s.log(fmt.Sprintf("NuAddAccount rejected: account already exists for nuid=%q len=%d hex=%s", nuid, len(nuid), hex.EncodeToString([]byte(nuid))))
			return Encode("acct", pkt.ID, kvs("TXN", "NuAddAccount", "errorCode", "122")), nil
		}
		return nil, err
	}
	if account == nil {
		s.log(fmt.Sprintf("NuAddAccount rejected: account already exists for nuid=%q len=%d hex=%s", nuid, len(nuid), hex.EncodeToString([]byte(nuid))))
		return Encode("acct", pkt.ID, kvs("TXN", "NuAddAccount", "errorCode", "122")), nil
	}
	s.log(fmt.Sprintf("NuAddAccount created: accountId=%d nuid=%q", account.ID, account.NUID))
	return Encode("acct", pkt.ID, kvs("TXN", "NuAddAccount")), nil
}

func handleNuGetPersonas(s *Session, pkt Packet) ([]byte, error) {
	if s.accountID == 0 {
		return Encode("acct", pkt.ID, kvs("TXN", "NuGetPersonas", "personas.[]", "0")), nil
	}
	personas, err := s.server.store.Personas(s.accountID)
	if err != nil {
		return nil, err
	}
	resp := kvs("TXN", "NuGetPersonas", "personas.[]", strconv.Itoa(len(personas)))
	for i, persona := range personas {
		resp = append(resp, KV{Key: fmt.Sprintf("personas.%d", i), Value: persona.Name})
	}
	return Encode("acct", pkt.ID, resp), nil
}

func handleNuLoginPersona(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	if s.accountID == 0 {
		return Encode("acct", pkt.ID, kvs("TXN", "NuLoginPersona")), nil
	}
	name := p["name"]
	if reason := validatePersonaName(name, s.server.cfg.MaxIdentifierLength); reason != "" {
		s.log(fmt.Sprintf("NuLoginPersona rejected: field=name reason=%s len=%d hex=%s", reason, len(name), hex.EncodeToString([]byte(name))))
		return Encode("acct", pkt.ID, kvs("TXN", "NuLoginPersona", "errorCode", "122")), nil
	}
	persona, err := s.server.store.GetOrCreatePersona(s.accountID, name)
	if err != nil {
		if errors.Is(err, storage.ErrPersonaNameTaken) {
			s.log(fmt.Sprintf("NuLoginPersona rejected: persona %q belongs to another account", name))
			return Encode("acct", pkt.ID, kvs("TXN", "NuLoginPersona", "errorCode", "122")), nil
		}
		return nil, err
	}
	s.personaID = persona.ID
	s.personaName = persona.Name
	s.lkey = randomHex(20)
	return Encode("acct", pkt.ID, kvs(
		"TXN", "NuLoginPersona",
		"lkey", s.lkey,
		"profileId", itoa64(persona.ID),
		"userId", itoa64(s.accountID),
	)), nil
}

func handleNuAddPersona(s *Session, pkt Packet) ([]byte, error) {
	if s.accountID != 0 {
		name := ParamsMap(pkt.Params)["name"]
		if reason := validatePersonaName(name, s.server.cfg.MaxIdentifierLength); reason != "" {
			s.log(fmt.Sprintf("NuAddPersona rejected: field=name reason=%s len=%d hex=%s", reason, len(name), hex.EncodeToString([]byte(name))))
			return Encode("acct", pkt.ID, kvs("TXN", "NuAddPersona", "errorCode", "122")), nil
		}
		_, err := s.server.store.GetOrCreatePersona(s.accountID, name)
		if err != nil {
			if errors.Is(err, storage.ErrPersonaNameTaken) {
				s.log(fmt.Sprintf("NuAddPersona rejected: persona %q belongs to another account", name))
				return Encode("acct", pkt.ID, kvs("TXN", "NuAddPersona", "errorCode", "122")), nil
			}
			return nil, err
		}
	}
	return Encode("acct", pkt.ID, kvs("TXN", "NuAddPersona")), nil
}

func handleGetLockerURL(s *Session, pkt Packet) ([]byte, error) {
	return Encode("acct", pkt.ID, kvs("TXN", "GetLockerURL", "url", s.server.cfg.LockerURL)), nil
}

func handleNuLookupUserInfo(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	count := atoi(p["userInfo.[]"])
	resp := kvs("TXN", "NuLookupUserInfo", "userInfo.[]", strconv.Itoa(count))
	for i := 0; i < count; i++ {
		name := p[fmt.Sprintf("userInfo.%d.userName", i)]
		persona, err := s.server.store.LookupPersonaByName(name)
		if err != nil {
			return nil, err
		}
		userID := "0"
		if persona != nil {
			name = persona.Name
			userID = itoa64(persona.ID)
			s.log(fmt.Sprintf("NuLookupUserInfo: request=%q resolved personaId=%d", p[fmt.Sprintf("userInfo.%d.userName", i)], persona.ID))
		} else {
			s.log(fmt.Sprintf("NuLookupUserInfo: request=%q missed", name))
		}
		resp = append(resp,
			KV{Key: fmt.Sprintf("userInfo.%d.userName", i), Value: name},
			KV{Key: fmt.Sprintf("userInfo.%d.namespace", i), Value: ""},
			KV{Key: fmt.Sprintf("userInfo.%d.userId", i), Value: userID},
		)
	}
	return Encode("acct", pkt.ID, resp), nil
}

func handleNuSuggestPersonas(s *Session, pkt Packet) ([]byte, error) {
	return Encode("acct", pkt.ID, kvs("TXN", "NuSuggestPersonas", "suggestedNames.[]", "0")), nil
}

func handleNuGetEntitlements(s *Session, pkt Packet) ([]byte, error) {
	return Encode("acct", pkt.ID, kvs("TXN", "NuGetEntitlements", "entitlements.[]", "0")), nil
}

func handleNuGetEntitlementCount(s *Session, pkt Packet) ([]byte, error) {
	return Encode("acct", pkt.ID, kvs("TXN", "NuGetEntitlementCount", "entitlementCount", "0")), nil
}

func handleGameSpyPreAuth(s *Session, pkt Packet) ([]byte, error) {
	return Encode("acct", pkt.ID, kvs("TXN", "GameSpyPreAuth")), nil
}

func handleGetCountryList(s *Session, pkt Packet) ([]byte, error) {
	countries := []struct{ iso, desc, age string }{
		{"US", "United%20States", "13"}, {"GB", "United%20Kingdom", "13"},
		{"CA", "Canada", "13"}, {"AU", "Australia", "15"}, {"DE", "Germany", "16"},
		{"FR", "France", "15"}, {"ES", "Spain", "14"}, {"IT", "Italy", "14"},
		{"NL", "Netherlands", "16"}, {"SE", "Sweden", "13"}, {"NO", "Norway", "15"},
		{"DK", "Denmark", "13"}, {"FI", "Finland", "13"}, {"JP", "Japan", "13"},
		{"BR", "Brazil", "13"}, {"MX", "Mexico", "13"}, {"PL", "Poland", "13"},
		{"RU", "Russia", "14"}, {"KR", "South%20Korea", "14"}, {"NZ", "New%20Zealand", "13"},
		{"AT", "Austria", "14"}, {"BE", "Belgium", "13"}, {"CH", "Switzerland", "13"},
		{"IE", "Ireland", "13"}, {"PT", "Portugal", "13"}, {"CZ", "Czech%20Republic", "15"},
	}
	resp := kvs("TXN", "GetCountryList", "countryList.[]", strconv.Itoa(len(countries)))
	for i, c := range countries {
		resp = append(resp,
			KV{Key: fmt.Sprintf("countryList.%d.ISOCode", i), Value: c.iso},
			KV{Key: fmt.Sprintf("countryList.%d.description", i), Value: c.desc},
			KV{Key: fmt.Sprintf("countryList.%d.registrationAgeLimit", i), Value: c.age},
		)
	}
	return Encode("acct", pkt.ID, resp), nil
}

func handleNuGetTos(s *Session, pkt Packet) ([]byte, error) {
	tos := "MedgeNet%20-%20Terms%20of%20Service.%20This%20is%20an%20unofficial%20server%20for%20Mirror's%20Edge.%20It%20is%20not%20affiliated%20with%20or%20endorsed%20by%20Electronic%20Arts.%20Use%20the%20service%20respectfully."
	return Encode("acct", pkt.ID, kvs("TXN", "NuGetTos", "tos", tos)), nil
}

func handleSetPresenceStatus(s *Session, pkt Packet) ([]byte, error) {
	return Encode("pres", pkt.ID, kvs("TXN", "SetPresenceStatus")), nil
}

func handlePresenceSubscribe(s *Session, pkt Packet) ([]byte, error) {
	return Encode("pres", pkt.ID, kvs("TXN", "PresenceSubscribe", "responses.[]", "0")), nil
}

func handleModifySettings(s *Session, pkt Packet) ([]byte, error) {
	return Encode("xmsg", pkt.ID, kvs("TXN", "ModifySettings")), nil
}

func handleGetMessages(s *Session, pkt Packet) ([]byte, error) {
	return Encode("xmsg", pkt.ID, kvs("TXN", "GetMessages", "messages.[]", "0")), nil
}

func (s *Session) log(msg string) {
	s.server.log.Write("FESL", "["+s.addr+"] "+msg)
}

func kvs(values ...string) []KV {
	out := make([]KV, 0, len(values)/2)
	for i := 0; i+1 < len(values); i += 2 {
		out = append(out, KV{Key: values[i], Value: values[i+1]})
	}
	return out
}

func handlerErrorResponse(pkt Packet, txn string) []byte {
	if txn == "" {
		txn = ParamsMap(pkt.Params)["TXN"]
	}
	return Encode(pkt.Type, pkt.ID, kvs("TXN", txn, "errorCode", "1"))
}

func validateAccountNUID(value string, maxLen int) string {
	return validateLineIdentifier(value, maxLen, false)
}

func validatePersonaName(value string, maxLen int) string {
	if reason := validateLineIdentifier(value, maxLen, true); reason != "" {
		return reason
	}
	for _, r := range value {
		if !(r >= 'A' && r <= 'Z') &&
			!(r >= 'a' && r <= 'z') &&
			!(r >= '0' && r <= '9') &&
			r != '_' && r != '.' && r != '-' && r != ' ' {
			return "unsafe character"
		}
	}
	return ""
}

func validateLineIdentifier(value string, maxLen int, rejectSeparators bool) string {
	if value == "" || strings.TrimSpace(value) == "" {
		return "empty"
	}
	if maxLen > 0 && len(value) > maxLen {
		return "too long"
	}
	for _, r := range value {
		if r < 0x20 || r == 0x7f || r == '=' {
			return "invalid character"
		}
		if rejectSeparators && (r == '/' || r == '\\') {
			return "path separator"
		}
	}
	return ""
}

func validateAccountPassword(value string) string {
	if value == "" {
		return "empty"
	}
	if len(value) > maxPasswordBytes {
		return "too long"
	}
	for _, r := range value {
		if r == 0 || r == '\r' || r == '\n' {
			return "invalid character"
		}
	}
	return ""
}

func randomHex(n int) string {
	buf := make([]byte, n)
	if _, err := rand.Read(buf); err != nil {
		panic(err)
	}
	return hex.EncodeToString(buf)
}

func itoa64(value int64) string {
	return strconv.FormatInt(value, 10)
}

func atoi(value string) int {
	i, _ := strconv.Atoi(value)
	return i
}

func atof(value string) float64 {
	f, _ := strconv.ParseFloat(value, 64)
	return f
}

func parseKeyList(params map[string]string, prefix string) []string {
	count := atoi(params[prefix+".[]"])
	keys := make([]string, 0, count)
	for i := 0; i < count; i++ {
		key := params[fmt.Sprintf("%s.%d", prefix, i)]
		if key != "" {
			keys = append(keys, key)
		}
	}
	return keys
}

func parseOwnerList(params map[string]string) []struct {
	id  int64
	typ string
} {
	count := atoi(params["owners.[]"])
	owners := make([]struct {
		id  int64
		typ string
	}, 0, count)
	for i := 0; i < count; i++ {
		id := int64(atoi(params[fmt.Sprintf("owners.%d.ownerId", i)]))
		if id == 0 {
			continue
		}
		typ := params[fmt.Sprintf("owners.%d.ownerType", i)]
		if typ == "" {
			typ = "1"
		}
		owners = append(owners, struct {
			id  int64
			typ string
		}{id: id, typ: typ})
	}
	return owners
}

func defaultForKey(key string) float64 {
	if strings.HasSuffix(key, "_20") {
		return noTime
	}
	return 0
}

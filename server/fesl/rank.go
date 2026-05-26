package fesl

import (
	"fmt"
	"strconv"
	"strings"

	"medgenet-prototype/storage"
)

const maxRankPacketBytes = 60000
const maxHistoricalTopRows = 100

func handleGetStats(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	owner := int64(atoi(p["owner"]))
	periodID := atoi(p["periodId"])
	keys := parseKeyList(p, "keys")
	personaID := s.rankLookupPersonaID(owner)
	responseOwnerID := owner
	if responseOwnerID == 0 {
		responseOwnerID = personaID
	}

	stats := map[string]float64{}
	if personaID != 0 {
		var err error
		if storage.IsHistoricalOwner(personaID) {
			stats, err = s.server.store.HistoricalStats(personaID, keys, periodID)
		} else {
			stats, err = s.server.store.GetStats(personaID, keys, periodID)
		}
		if err != nil {
			return nil, err
		}
	}

	rankValue, err := statGroupRankValue(s.server.store, personaID, keys, periodID)
	if err != nil {
		return nil, err
	}
	resp := kvs(
		"TXN", "GetStats",
		"ownerId", itoa64(responseOwnerID),
		"ownerType", withDefault(p["ownerType"], "1"),
		"owner", itoa64(responseOwnerID),
		"stats.[]", strconv.Itoa(len(keys)),
	)
	emitStatGroupFlat(&resp, "stats.", stats, keys, rankValue)
	return Encode("rank", pkt.ID, resp), nil
}

func handleGetRankedStats(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	owner := int64(atoi(p["owner"]))
	periodID := atoi(p["periodId"])
	keys := parseKeyList(p, "keys")
	personaID := s.rankLookupPersonaID(owner)
	responseOwnerID := owner
	if responseOwnerID == 0 {
		responseOwnerID = personaID
	}

	resp := kvs(
		"TXN", "GetRankedStats",
		"ownerId", itoa64(responseOwnerID),
		"ownerType", withDefault(p["ownerType"], "1"),
		"owner", itoa64(responseOwnerID),
		"rankedStats.[]", strconv.Itoa(len(keys)),
		"stats.[]", strconv.Itoa(len(keys)),
	)
	for i, key := range keys {
		ranked := zeroRanked(key)
		if personaID != 0 {
			var got storage.RankedStat
			var err error
			if storage.IsHistoricalOwner(personaID) {
				got, err = s.server.store.HistoricalRankedStats(personaID, key, periodID)
			} else {
				got, err = s.server.store.RankedStats(personaID, key, periodID)
			}
			if err != nil {
				return nil, err
			}
			if got.Value != 0 {
				ranked = got
			}
		}
		prefix := fmt.Sprintf("rankedStats.%d", i)
		resp = append(resp,
			KV{Key: prefix + ".key", Value: key},
			KV{Key: prefix + ".value", Value: fmtFloat(ranked.Value)},
			KV{Key: prefix + ".text", Value: fmtFloat(ranked.Value)},
			KV{Key: prefix + ".rank", Value: strconv.Itoa(ranked.Rank)},
		)
		statsPrefix := fmt.Sprintf("stats.%d", i)
		resp = append(resp,
			KV{Key: statsPrefix + ".key", Value: key},
			KV{Key: statsPrefix + ".value", Value: fmtFloat(ranked.Value)},
			KV{Key: statsPrefix + ".text", Value: fmtFloat(ranked.Value)},
			KV{Key: statsPrefix + ".rank", Value: strconv.Itoa(ranked.Rank)},
		)
	}
	return Encode("rank", pkt.ID, resp), nil
}

func handleGetTopNAndStats(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	key := p["key"]
	minRank := atoi(p["minRank"])
	if minRank == 0 {
		minRank = 1
	}
	maxRank := atoi(p["maxRank"])
	if maxRank == 0 {
		maxRank = 1
	}
	periodID := atoi(p["periodId"])
	extraKeys := parseKeyList(p, "keys")
	entries, err := s.server.store.TopN(key, minRank, maxRank, extraKeys, periodID)
	if err != nil {
		return nil, err
	}
	allKeys := leaderboardKeys(key, extraKeys)

	if len(entries) == 0 {
		if minRank >= 500 {
			resp := buildDummyTopNAndStatsResponse(allKeys, key, minRank, "cutoff", true)
			encoded := Encode("rank", pkt.ID, resp)
			s.logTopNAndStats(key, minRank, maxRank, periodID, len(allKeys), 1, len(encoded), "dummy:cutoff")
			return encoded, nil
		}
		if minRank == 1 && maxRank == 1 {
			resp := buildDummyTopNAndStatsResponse(allKeys, key, minRank, "rank1", false)
			encoded := Encode("rank", pkt.ID, resp)
			s.logTopNAndStats(key, minRank, maxRank, periodID, len(allKeys), 1, len(encoded), "dummy:rank1")
			return encoded, nil
		}
	}

	if hasHistoricalRows(entries) && len(entries) > maxHistoricalTopRows {
		entries = entries[:maxHistoricalTopRows]
	}
	resp := buildTopNAndStatsResponse(entries, allKeys, key)
	encoded := Encode("rank", pkt.ID, resp)
	for len(encoded) > maxRankPacketBytes && len(entries) > 1 {
		entries = entries[:len(entries)-1]
		resp = buildTopNAndStatsResponse(entries, allKeys, key)
		encoded = Encode("rank", pkt.ID, resp)
	}
	s.logTopNAndStats(key, minRank, maxRank, periodID, len(allKeys), len(entries), len(encoded), topNRowSummary(entries))
	if len(encoded) > maxRankPacketBytes {
		s.log(fmt.Sprintf("GetTopNAndStats response remains large after cap: %d bytes", len(encoded)))
	}
	return encoded, nil
}

func (s *Session) logTopNAndStats(key string, minRank int, maxRank int, periodID int, requestedKeys int, returnedRows int, bytes int, rows string) {
	s.log(fmt.Sprintf(
		"GetTopNAndStats detail: key=%s range=%d-%d period=%d requestedKeys=%d returnedRows=%d bytes=%d rows=%s",
		key,
		minRank,
		maxRank,
		periodID,
		requestedKeys,
		returnedRows,
		bytes,
		rows,
	))
}

func topNRowSummary(entries []storage.TopEntry) string {
	if len(entries) == 0 {
		return "none"
	}
	parts := make([]string, 0, len(entries))
	for _, entry := range entries {
		source := "live"
		if entry.Imported {
			source = "hist"
		} else if entry.TimeOnly {
			source = "linked-hist"
		}
		parts = append(parts, fmt.Sprintf("%d:%s:%s", entry.Rank, source, entry.PersonaName))
	}
	return strings.Join(parts, ", ")
}

func hasHistoricalRows(entries []storage.TopEntry) bool {
	for _, entry := range entries {
		if entry.Imported {
			return true
		}
	}
	return false
}

func buildTopNAndStatsResponse(entries []storage.TopEntry, allKeys []string, sortKey string) []KV {
	resp := kvs("TXN", "GetTopNAndStats", "stats.[]", strconv.Itoa(len(entries)))
	for i, entry := range entries {
		prefix := fmt.Sprintf("stats.%d", i)
		sortValue := entry.Values[sortKey]
		ownerID := itoa64(entry.PersonaID)
		if entry.Imported {
			resp = append(resp,
				KV{Key: prefix + ".owner", Value: ownerID},
				KV{Key: prefix + ".name", Value: entry.PersonaName},
				KV{Key: prefix + ".rank", Value: strconv.Itoa(entry.Rank)},
				KV{Key: prefix + ".value", Value: fmtFloat(sortValue)},
			)
			emitSparseTimeAddStatsForEntry(&resp, prefix+".addStats.", entry, allKeys, sortKey)
			continue
		}
		resp = append(resp,
			KV{Key: prefix + ".ownerId", Value: ownerID},
			KV{Key: prefix + ".ownerType", Value: "1"},
			KV{Key: prefix + ".owner", Value: ownerID},
			KV{Key: prefix + ".name", Value: entry.PersonaName},
			KV{Key: prefix + ".rank", Value: strconv.Itoa(entry.Rank)},
			KV{Key: prefix + ".value", Value: fmtFloat(sortValue)},
			KV{Key: prefix + ".text", Value: fmtFloat(sortValue)},
		)
		if !entry.Imported {
			emitStatGroupFlatForEntry(&resp, prefix+".stats.", entry, allKeys, strconv.Itoa(entry.Rank))
		}
		emitStatGroupFlatForEntry(&resp, prefix+".addStats.", entry, allKeys, strconv.Itoa(entry.Rank))
	}
	return resp
}

func buildDummyTopNAndStatsResponse(allKeys []string, sortKey string, rank int, reason string, zeroTime bool) []KV {
	value := defaultForKey(sortKey)
	if zeroTime {
		value = 0
	}
	name := "No Name"
	if reason == "cutoff" {
		name = ""
	}
	resp := kvs(
		"TXN", "GetTopNAndStats",
		"stats.[]", "1",
		"stats.0.ownerId", "0",
		"stats.0.ownerType", "1",
		"stats.0.owner", "0",
		"stats.0.name", name,
		"stats.0.rank", strconv.Itoa(rank),
		"stats.0.value", fmtFloat(value),
		"stats.0.text", fmtFloat(value),
	)
	dummy := map[string]float64{}
	for _, key := range allKeys {
		dummy[key] = defaultForKey(key)
		if zeroTime {
			dummy[key] = 0
		}
	}
	emitStatGroupFlat(&resp, "stats.0.stats.", dummy, allKeys, strconv.Itoa(rank))
	emitStatGroupFlat(&resp, "stats.0.addStats.", dummy, allKeys, strconv.Itoa(rank))
	return resp
}

func emitMinimalStatGroupForEntry(resp *[]KV, prefix string, entry storage.TopEntry, keys []string) {
	*resp = append(*resp, KV{Key: prefix + "[]", Value: strconv.Itoa(len(keys))})
	for i, key := range keys {
		value, ok := entry.Values[key]
		if !ok {
			value = defaultForKey(key)
		}
		statPrefix := fmt.Sprintf("%s%d", prefix, i)
		*resp = append(*resp,
			KV{Key: statPrefix + ".key", Value: key},
			KV{Key: statPrefix + ".value", Value: fmtFloat(value)},
		)
	}
}

func emitSparseTimeAddStatsForEntry(resp *[]KV, prefix string, entry storage.TopEntry, keys []string, sortKey string) {
	*resp = append(*resp, KV{Key: prefix + "[]", Value: strconv.Itoa(len(keys))})
	for i, key := range keys {
		if key != sortKey {
			continue
		}
		value, ok := entry.Values[key]
		if !ok {
			value = defaultForKey(key)
		}
		statPrefix := fmt.Sprintf("%s%d", prefix, i)
		*resp = append(*resp,
			KV{Key: statPrefix + ".key", Value: key},
			KV{Key: statPrefix + ".value", Value: fmtFloat(value)},
		)
	}
}

func emitStatGroupFlatForEntry(resp *[]KV, prefix string, entry storage.TopEntry, keys []string, rankValue string) {
	*resp = append(*resp, KV{Key: prefix + "[]", Value: strconv.Itoa(len(keys))})
	for i, key := range keys {
		value, ok := entry.Values[key]
		if !ok {
			value = defaultForKey(key)
		}
		text := fmtFloat(value)
		if (entry.Imported || entry.TimeOnly) && !ok && !strings.HasSuffix(key, "_20") {
			text = ""
		}
		statPrefix := fmt.Sprintf("%s%d", prefix, i)
		*resp = append(*resp,
			KV{Key: statPrefix + ".key", Value: key},
			KV{Key: statPrefix + ".value", Value: fmtFloat(value)},
			KV{Key: statPrefix + ".text", Value: text},
			KV{Key: statPrefix + ".rank", Value: rankValue},
		)
	}
}

func handleGetStatsForOwners(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	periodID := atoi(p["periodId"])
	owners := parseOwnerList(p)
	keys := parseKeyList(p, "keys")
	resp := kvs("TXN", "GetStatsForOwners", "stats.[]", strconv.Itoa(len(owners)))
	for i, owner := range owners {
		lookupID := s.rankLookupPersonaID(owner.id)
		var stats map[string]float64
		var err error
		if storage.IsHistoricalOwner(lookupID) {
			stats, err = s.server.store.HistoricalStats(lookupID, keys, periodID)
		} else {
			stats, err = s.server.store.GetStats(lookupID, keys, periodID)
		}
		if err != nil {
			return nil, err
		}
		rankValue, err := statGroupRankValue(s.server.store, lookupID, keys, periodID)
		if err != nil {
			return nil, err
		}
		prefix := fmt.Sprintf("stats.%d", i)
		resp = append(resp,
			KV{Key: prefix + ".ownerId", Value: itoa64(owner.id)},
			KV{Key: prefix + ".ownerType", Value: owner.typ},
			KV{Key: prefix + ".owner", Value: itoa64(owner.id)},
			KV{Key: prefix + ".rank", Value: rankValue},
		)
		emitStatGroupFlat(&resp, prefix+".stats.", stats, keys, rankValue)
	}
	encoded := Encode("rank", pkt.ID, resp)
	s.log(fmt.Sprintf("GetStatsForOwners detail: owners=%d period=%d requestedKeys=%d returnedRows=%d bytes=%d", len(owners), periodID, len(keys), len(owners), len(encoded)))
	return encoded, nil
}

func handleGetRankedStatsForOwners(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	periodID := atoi(p["periodId"])
	owners := parseOwnerList(p)
	keys := parseKeyList(p, "keys")
	resp := kvs("TXN", "GetRankedStatsForOwners", "rankedStats.[]", strconv.Itoa(len(owners)))
	for i, owner := range owners {
		lookupID := s.rankLookupPersonaID(owner.id)
		prefix := fmt.Sprintf("rankedStats.%d", i)
		resp = append(resp,
			KV{Key: prefix + ".ownerId", Value: itoa64(owner.id)},
			KV{Key: prefix + ".ownerType", Value: owner.typ},
			KV{Key: prefix + ".owner", Value: itoa64(owner.id)},
			KV{Key: prefix + ".rankedStats.[]", Value: strconv.Itoa(len(keys))},
		)
		for j, key := range keys {
			var ranked storage.RankedStat
			var err error
			if storage.IsHistoricalOwner(lookupID) {
				ranked, err = s.server.store.HistoricalRankedStats(lookupID, key, periodID)
			} else {
				ranked, err = s.server.store.RankedStats(lookupID, key, periodID)
			}
			if err != nil {
				return nil, err
			}
			if ranked.Value == 0 {
				ranked = zeroRanked(key)
			}
			statPrefix := fmt.Sprintf("%s.rankedStats.%d", prefix, j)
			resp = append(resp,
				KV{Key: statPrefix + ".key", Value: key},
				KV{Key: statPrefix + ".value", Value: fmtFloat(ranked.Value)},
				KV{Key: statPrefix + ".text", Value: fmtFloat(ranked.Value)},
				KV{Key: statPrefix + ".rank", Value: strconv.Itoa(ranked.Rank)},
			)
		}
	}
	encoded := Encode("rank", pkt.ID, resp)
	s.log(fmt.Sprintf("GetRankedStatsForOwners detail: owners=%d period=%d requestedKeys=%d returnedRows=%d bytes=%d", len(owners), periodID, len(keys), len(owners), len(encoded)))
	return encoded, nil
}

func handleUpdateStats(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	ownerCount := atoi(p["u.[]"])
	total := 0
	for oi := 0; oi < ownerCount; oi++ {
		statCount := atoi(p[fmt.Sprintf("u.%d.s.[]", oi)])
		for si := 0; si < statCount; si++ {
			key := p[fmt.Sprintf("u.%d.s.%d.k", oi, si)]
			if key == "" {
				continue
			}
			value := atof(p[fmt.Sprintf("u.%d.s.%d.v", oi, si)])
			periods := parsePeriods(p[fmt.Sprintf("u.%d.s.%d.pt", oi, si)])
			if len(periods) == 0 {
				periods = []int{0}
			}
			if s.personaID != 0 {
				for _, periodID := range periods {
					if err := s.server.store.UpdateStats(s.personaID, map[string]float64{key: value}, periodID); err != nil {
						return nil, err
					}
				}
				total++
			}
		}
	}
	s.log(fmt.Sprintf("Persisted %d stat values for %s", total, s.personaName))
	txn := withDefault(p["TXN"], "UpdateStats")
	return Encode("rank", pkt.ID, kvs("TXN", txn)), nil
}

func emitStatGroupFlat(resp *[]KV, prefix string, values map[string]float64, keys []string, rankValue string) {
	*resp = append(*resp, KV{Key: prefix + "[]", Value: strconv.Itoa(len(keys))})
	for i, key := range keys {
		value, ok := values[key]
		if !ok {
			value = defaultForKey(key)
		}
		statPrefix := fmt.Sprintf("%s%d", prefix, i)
		*resp = append(*resp,
			KV{Key: statPrefix + ".key", Value: key},
			KV{Key: statPrefix + ".value", Value: fmtFloat(value)},
			KV{Key: statPrefix + ".text", Value: fmtFloat(value)},
			KV{Key: statPrefix + ".rank", Value: rankValue},
		)
	}
}

func leaderboardKeys(sortKey string, requested []string) []string {
	var keys []string
	hasSortKey := sortKey == ""
	for _, key := range requested {
		if key != "" {
			keys = append(keys, key)
			if key == sortKey {
				hasSortKey = true
			}
		}
	}
	if sortKey != "" && !hasSortKey {
		keys = append([]string{sortKey}, keys...)
	}
	return keys
}

func zeroRanked(key string) storage.RankedStat {
	return storage.RankedStat{Rank: 0, Value: defaultForKey(key)}
}

func (s *Session) rankLookupPersonaID(ownerID int64) int64 {
	if ownerID == 0 {
		return s.personaID
	}
	if s.personaID != 0 && s.accountID != 0 && ownerID == s.accountID {
		return s.personaID
	}
	return ownerID
}

func statGroupRankValue(store *storage.Store, ownerID int64, keys []string, periodID int) (string, error) {
	key := primaryTimeKey(keys)
	if ownerID == 0 || key == "" {
		return "0", nil
	}
	var ranked storage.RankedStat
	var err error
	if storage.IsHistoricalOwner(ownerID) {
		ranked, err = store.HistoricalRankedStats(ownerID, key, periodID)
	} else {
		ranked, err = store.RankedStats(ownerID, key, periodID)
	}
	if err != nil {
		return "", err
	}
	if ranked.Value == 0 {
		return "0", nil
	}
	return strconv.Itoa(ranked.Rank), nil
}

func primaryTimeKey(keys []string) string {
	for _, key := range keys {
		if strings.HasSuffix(key, "_20") {
			return key
		}
	}
	return ""
}

func fmtFloat(value float64) string {
	return fmt.Sprintf("%.6f", value)
}

func parsePeriods(value string) []int {
	value = strings.Trim(value, "\"")
	if value == "" {
		return nil
	}
	parts := strings.Split(value, ",")
	out := make([]int, 0, len(parts))
	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		period, err := strconv.Atoi(part)
		if err == nil {
			out = append(out, period)
		}
	}
	return out
}

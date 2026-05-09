package fesl

import (
	"fmt"
	"strconv"

	"medgenet-prototype/storage"
)

func handleGetAssociations(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	assocType := associationType(p)
	ownerID := p["owner.id"]
	if ownerID == "" {
		ownerID = "0"
	}
	ownerType := p["owner.type"]
	if ownerType == "" {
		ownerType = "1"
	}

	members := []memberRow{}
	if s.personaID != 0 {
		associations, err := s.server.store.GetAssociations(s.personaID, assocType)
		if err != nil {
			return nil, err
		}
		for _, assoc := range associations {
			members = append(members, memberRow{id: assoc.ID, name: assoc.Name, typ: "1"})
		}
	}

	resp := baseAssociationResponse("GetAssociations", p, assocType, ownerID, ownerType)
	emitMembers(&resp, members)
	return Encode("asso", pkt.ID, resp), nil
}

func handleAddAssociations(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	assocType := associationType(p)
	count := atoi(p["addRequests.[]"])
	ownerID := int64(0)
	ownerType := "1"
	results := []associationResult{}

	for i := 0; i < count; i++ {
		req := s.parseAssociationRequest(p, "addRequests", i)
		ownerID = req.ownerID
		ownerType = req.ownerType
		outcome, reason, err := s.validateAssociationRequest(req, true)
		if err != nil {
			return nil, err
		}
		if outcome == associationOutcomeOK {
			if _, err := s.server.store.AddAssociation(req.ownerID, req.memberID, assocType); err != nil {
				return nil, err
			}
		} else {
			s.logAssociationRefusal("AddAssociations", i, assocType, req, reason)
		}
		results = append(results, associationResult{
			requestIndex: i,
			ownerID:      req.ownerID,
			ownerType:    req.ownerType,
			memberID:     req.memberID,
			memberType:   req.memberType,
			outcome:      outcome,
		})
	}

	return s.associationMutationResponse(pkt, "AddAssociations", "addRequests", p, assocType, ownerID, ownerType, results)
}

func handleDeleteAssociations(s *Session, pkt Packet) ([]byte, error) {
	p := ParamsMap(pkt.Params)
	assocType := associationType(p)
	count := atoi(p["deleteRequests.[]"])
	ownerID := int64(0)
	ownerType := "1"
	results := []associationResult{}

	for i := 0; i < count; i++ {
		req := s.parseAssociationRequest(p, "deleteRequests", i)
		ownerID = req.ownerID
		ownerType = req.ownerType
		outcome, reason, err := s.validateAssociationRequest(req, false)
		if err != nil {
			return nil, err
		}
		if outcome == associationOutcomeOK {
			if err := s.server.store.DeleteAssociation(req.ownerID, req.memberID, assocType); err != nil {
				return nil, err
			}
		} else {
			s.logAssociationRefusal("DeleteAssociations", i, assocType, req, reason)
		}
		results = append(results, associationResult{
			requestIndex: i,
			ownerID:      req.ownerID,
			ownerType:    req.ownerType,
			memberID:     req.memberID,
			memberType:   req.memberType,
			outcome:      outcome,
		})
	}

	return s.associationMutationResponse(pkt, "DeleteAssociations", "deleteRequests", p, assocType, ownerID, ownerType, results)
}

func (s *Session) associationMutationResponse(pkt Packet, txn string, requestPrefix string, p map[string]string, assocType string, ownerID int64, ownerType string, results []associationResult) ([]byte, error) {
	if ownerID == 0 {
		ownerID = s.personaID
	}
	resp := baseAssociationResponse(txn, p, assocType, itoa64(ownerID), ownerType)
	resp = append(resp, KV{Key: "result.[]", Value: strconv.Itoa(len(results))})
	for i, result := range results {
		if err := s.emitAssociationResult(&resp, p, requestPrefix, i, result, assocType); err != nil {
			return nil, err
		}
	}
	members := []memberRow{}
	if ownerID != 0 {
		associations, err := s.server.store.GetAssociations(ownerID, assocType)
		if err != nil {
			return nil, err
		}
		for _, assoc := range associations {
			members = append(members, memberRow{id: assoc.ID, name: assoc.Name, typ: "1"})
		}
	}
	emitMembers(&resp, members)
	return Encode("asso", pkt.ID, resp), nil
}

func (s *Session) emitAssociationResult(resp *[]KV, p map[string]string, requestPrefix string, resultIndex int, result associationResult, assocType string) error {
	list, err := s.server.store.GetAssociations(result.ownerID, assocType)
	if err != nil {
		return err
	}
	owner, err := s.server.store.PersonaByID(result.ownerID)
	if err != nil {
		return err
	}
	member, err := s.server.store.PersonaByID(result.memberID)
	if err != nil {
		return err
	}

	ownerName := ""
	if owner != nil {
		ownerName = owner.Name
	}
	memberName := ""
	memberCreated := ""
	if member != nil {
		memberName = member.Name
		memberCreated = member.CreatedAt
	}

	prefix := fmt.Sprintf("result.%d", resultIndex)
	*resp = append(*resp,
		KV{Key: prefix + ".owner.id", Value: itoa64(result.ownerID)},
		KV{Key: prefix + ".owner.type", Value: result.ownerType},
		KV{Key: prefix + ".owner.name", Value: ownerName},
		KV{Key: prefix + ".owner.xuid", Value: itoa64(result.ownerID)},
		KV{Key: prefix + ".member.id", Value: itoa64(result.memberID)},
		KV{Key: prefix + ".member.type", Value: result.memberType},
		KV{Key: prefix + ".member.name", Value: memberName},
		KV{Key: prefix + ".member.xuid", Value: itoa64(result.memberID)},
		KV{Key: prefix + ".member.created", Value: memberCreated},
		KV{Key: prefix + ".member.modified", Value: ""},
		KV{Key: prefix + ".mutual", Value: withDefault(p[fmt.Sprintf("%s.%d.mutual", requestPrefix, result.requestIndex)], "0")},
		KV{Key: prefix + ".outcome", Value: result.outcome},
		KV{Key: prefix + ".listSize", Value: strconv.Itoa(len(list))},
	)
	return nil
}

const (
	associationOutcomeOK      = "0"
	associationOutcomeRefused = "1"
)

type memberRow struct {
	id   int64
	name string
	typ  string
}

type associationRequest struct {
	requestIndex int
	ownerID      int64
	ownerType    string
	memberID     int64
	memberType   string
}

type associationResult struct {
	requestIndex int
	ownerID      int64
	ownerType    string
	memberID     int64
	memberType   string
	outcome      string
}

func baseAssociationResponse(txn string, p map[string]string, assocType string, ownerID string, ownerType string) []KV {
	return kvs(
		"TXN", txn,
		"domainPartition.domain", withDefault(p["domainPartition.domain"], "eagames"),
		"domainPartition.subDomain", withDefault(p["domainPartition.subDomain"], "takedown-pc"),
		"owner.id", ownerID,
		"owner.type", ownerType,
		"type", assocType,
	)
}

func emitMembers(resp *[]KV, members []memberRow) {
	*resp = append(*resp, KV{Key: "members.[]", Value: strconv.Itoa(len(members))})
	for i, member := range members {
		prefix := fmt.Sprintf("members.%d", i)
		*resp = append(*resp,
			KV{Key: prefix + ".id", Value: itoa64(member.id)},
			KV{Key: prefix + ".name", Value: member.name},
			KV{Key: prefix + ".type", Value: withDefault(member.typ, "1")},
		)
	}
}

func associationType(p map[string]string) string {
	return withDefault(p["type"], "PlasmaFriends")
}

func (s *Session) parseAssociationRequest(p map[string]string, requestPrefix string, index int) associationRequest {
	ownerID := int64(atoi(p[fmt.Sprintf("%s.%d.owner.id", requestPrefix, index)]))
	if ownerID == 0 {
		ownerID = s.personaID
	}
	return associationRequest{
		requestIndex: index,
		ownerID:      ownerID,
		ownerType:    withDefault(p[fmt.Sprintf("%s.%d.owner.type", requestPrefix, index)], "1"),
		memberID:     int64(atoi(p[fmt.Sprintf("%s.%d.member.id", requestPrefix, index)])),
		memberType:   withDefault(p[fmt.Sprintf("%s.%d.member.type", requestPrefix, index)], "1"),
	}
}

func (s *Session) validateAssociationRequest(req associationRequest, rejectSelf bool) (string, string, error) {
	if req.ownerID == 0 {
		return associationOutcomeRefused, "missing owner persona", nil
	}
	if req.memberID == 0 {
		return associationOutcomeRefused, "missing member persona", nil
	}
	if rejectSelf && req.ownerID == req.memberID {
		return associationOutcomeRefused, "self association", nil
	}
	if storage.IsHistoricalOwner(req.ownerID) {
		return associationOutcomeRefused, "historical owner persona", nil
	}
	if storage.IsHistoricalOwner(req.memberID) {
		return associationOutcomeRefused, "historical member persona", nil
	}
	if s.personaID != 0 && req.ownerID != s.personaID {
		return associationOutcomeRefused, "owner does not match logged-in persona", nil
	}
	ownerExists, err := s.server.store.PersonaExists(req.ownerID)
	if err != nil {
		return associationOutcomeRefused, "", err
	}
	if !ownerExists {
		return associationOutcomeRefused, "owner persona does not exist", nil
	}
	memberExists, err := s.server.store.PersonaExists(req.memberID)
	if err != nil {
		return associationOutcomeRefused, "", err
	}
	if !memberExists {
		return associationOutcomeRefused, "member persona does not exist", nil
	}
	return associationOutcomeOK, "", nil
}

func (s *Session) logAssociationRefusal(txn string, index int, assocType string, req associationRequest, reason string) {
	s.log(fmt.Sprintf(
		"%s refused: index=%d type=%s owner=%d member=%d reason=%s",
		txn,
		index,
		assocType,
		req.ownerID,
		req.memberID,
		reason,
	))
}

func withDefault(value string, fallback string) string {
	if value == "" {
		return fallback
	}
	return value
}

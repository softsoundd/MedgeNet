package fesl

import (
	"bytes"
	"encoding/binary"
	"errors"
	"strings"
)

const HeaderLen = 12

type KV struct {
	Key   string
	Value string
}

type Packet struct {
	Type   string
	ID     uint32
	Params []KV
}

func Encode(packetType string, id uint32, params []KV) []byte {
	var payload bytes.Buffer
	for _, param := range params {
		payload.WriteString(param.Key)
		payload.WriteByte('=')
		payload.WriteString(param.Value)
		payload.WriteByte('\n')
	}
	payload.WriteByte(0)

	payloadBytes := payload.Bytes()
	totalLen := HeaderLen + len(payloadBytes)
	out := make([]byte, totalLen)

	copy(out[0:4], []byte(packetType))
	binary.BigEndian.PutUint32(out[4:8], id)
	binary.BigEndian.PutUint32(out[8:12], uint32(totalLen))
	copy(out[12:], payloadBytes)

	return out
}

func Decode(data []byte) (Packet, []byte, bool, error) {
	if len(data) < HeaderLen {
		return Packet{}, data, false, nil
	}

	totalLen := int(binary.BigEndian.Uint32(data[8:12]))
	if totalLen < HeaderLen {
		return Packet{}, data, false, errors.New("invalid FESL packet length")
	}
	if len(data) < totalLen {
		return Packet{}, data, false, nil
	}

	pkt := Packet{
		Type: strings.TrimRight(string(data[0:4]), "\x00"),
		ID:   binary.BigEndian.Uint32(data[4:8]),
	}

	payload := string(data[12:totalLen])
	for _, line := range strings.Split(payload, "\n") {
		line = strings.TrimRight(line, "\x00")
		if line == "" {
			continue
		}
		key, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		pkt.Params = append(pkt.Params, KV{Key: key, Value: value})
	}

	return pkt, data[totalLen:], true, nil
}

func ParamsMap(params []KV) map[string]string {
	out := make(map[string]string, len(params))
	for _, param := range params {
		out[param.Key] = param.Value
	}
	return out
}

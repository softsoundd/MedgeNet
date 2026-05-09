package locker

import (
	"fmt"
	"strings"
)

type Entry struct {
	Name   string `json:"name"`
	Desc   string `json:"desc"`
	Game   string `json:"game"`
	Type   string `json:"type"`
	IDNT   string `json:"idnt"`
	Date   int64  `json:"date"`
	FileID int64  `json:"FileId"`
	Attr   int64  `json:"attr"`
	Size   int64  `json:"size"`
	Locs   int64  `json:"locs"`
	Perm   int64  `json:"perm"`
	Vers   int64  `json:"vers"`
	path   string
}

func BuildXML(entries []Entry, game string, pers string) string {
	var totalBytes int64
	for _, entry := range entries {
		totalBytes += entry.Size
	}

	var b strings.Builder
	fmt.Fprintf(&b, `<LOCKER error="0" numFiles="%d" maxFiles="150" numBytes="%d" maxBytes="1048576"`, len(entries), totalBytes)
	if game != "" {
		fmt.Fprintf(&b, ` game="%s"`, attr(game))
	}
	if pers != "" {
		fmt.Fprintf(&b, ` pers="%s"`, attr(pers))
	}
	if len(entries) == 0 {
		b.WriteString("/>")
		return b.String()
	}

	b.WriteString(">\n")
	for _, entry := range entries {
		fmt.Fprintf(
			&b,
			`<FILE name="%s" desc="%s" game="%s" type="%s" idnt="%s" date="%d" FileId="%d" attr="%d" size="%d" locs="%d" perm="%d" vers="%d"/>`+"\n",
			attr(entry.Name),
			attr(entry.Desc),
			attr(entry.Game),
			attr(entry.Type),
			attr(entry.IDNT),
			entry.Date,
			entry.FileID,
			entry.Attr,
			entry.Size,
			entry.Locs,
			entry.Perm,
			entry.Vers,
		)
	}
	b.WriteString("</LOCKER>")
	return b.String()
}

func attr(value string) string {
	value = strings.ReplaceAll(value, "&", "&amp;")
	value = strings.ReplaceAll(value, `"`, "&quot;")
	value = strings.ReplaceAll(value, "<", "&lt;")
	value = strings.ReplaceAll(value, ">", "&gt;")
	return value
}

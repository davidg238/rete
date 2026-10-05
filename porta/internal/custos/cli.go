// Copyright (c) 2026 Ekorau LLC

package custos

// cli.go — design §9's command fronts over the query core:
//
//	custos sql      -descriptors F (-store DB | -snapshot SNAP) "<query>"
//	custos tables   -descriptors F (-store DB | -snapshot SNAP)
//	custos describe -descriptors F <key|rule>
//
// Free SQL lives here, on the operator's own shell, and not on the network
// surface (addendum D3). A query must name its source: `-store` is the live
// binding, `-snapshot` the history one, and the two answer different questions,
// so neither is a default.

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"strings"
	"time"

	"github.com/davidg238/porta/internal/gate"
)

// RunCLI runs one subcommand. args excludes the program name.
func RunCLI(args []string, out io.Writer) error {
	if len(args) == 0 {
		return fmt.Errorf("custos: want sql, tables or describe")
	}
	fs := flag.NewFlagSet("custos "+args[0], flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	descriptors := fs.String("descriptors", "", "protocol/descriptors/records.yaml")
	store := fs.String("store", "", "porta's live store (the live binding)")
	snapshot := fs.String("snapshot", "", "a porta snapshot (the snap binding)")
	deadline := fs.Duration("deadline", 30*time.Second, "wall-clock limit for the query")
	if err := fs.Parse(args[1:]); err != nil {
		return err
	}
	if *descriptors == "" {
		return fmt.Errorf("custos %s: -descriptors is required", args[0])
	}
	d, err := gate.LoadDescriptors(*descriptors)
	if err != nil {
		return err
	}

	switch args[0] {
	case "describe":
		if fs.NArg() != 1 {
			return fmt.Errorf("custos describe: want one field key or rule name")
		}
		return describe(out, d, fs.Arg(0))
	case "sql", "tables":
	default:
		return fmt.Errorf("custos: unknown command %q", args[0])
	}

	var g *gate.Gate
	switch {
	case *store != "" && *snapshot == "":
		g, err = gate.OpenLive(*store, d)
	case *snapshot != "" && *store == "":
		g, err = gate.Open(*snapshot, d)
	default:
		return fmt.Errorf("custos %s: name exactly one of -store (live) or -snapshot (history)", args[0])
	}
	if err != nil {
		return err
	}
	defer g.Close()

	if args[0] == "tables" {
		names, err := g.Tables()
		if err != nil {
			return err
		}
		_, err = fmt.Fprintln(out, strings.Join(names, "\n"))
		return err
	}
	if fs.NArg() != 1 {
		return fmt.Errorf("custos sql: want one query argument")
	}
	res, err := g.Query(context.Background(), gate.Query{SQL: fs.Arg(0), Deadline: *deadline})
	if err != nil {
		return err
	}
	enc := json.NewEncoder(out)
	enc.SetIndent("", "  ")
	return enc.Encode(res)
}

// describe renders the reading rules for a field key (every shape carrying it)
// or for a rule name. It reads the descriptor table directly, so it needs no
// store — the same table `select * from descriptors` answers from.
func describe(out io.Writer, d *gate.Descriptors, what string) error {
	found := false
	for _, rule := range d.RecordRules() {
		r := d.Records[rule]
		for _, f := range r.Fields {
			if f.Key != what && rule != what {
				continue
			}
			found = true
			fmt.Fprintf(out, "%s.%s  (%s, %s)  %s\n", rule, f.Key, r.Name, strings.Join(r.Paths, " "), f.Name)
			fmt.Fprintf(out, "  unit: %s   kind: %s   status: %s\n", f.Unit, f.Kind, r.Status)
			fmt.Fprintf(out, "  meaning: %s\n", strings.TrimSpace(f.Meaning))
			if f.Validity != "" {
				fmt.Fprintf(out, "  validity: %s\n", strings.TrimSpace(f.Validity))
			}
			if len(f.Segment) > 0 {
				fmt.Fprintf(out, "  segment: %s\n", strings.Join(f.Segment, ", "))
			}
			if len(f.Refs) > 0 {
				fmt.Fprintf(out, "  refs: %s\n", strings.Join(f.Refs, ", "))
			}
		}
	}
	for _, rule := range d.ScalarRules() {
		if rule != what {
			continue
		}
		found = true
		s := d.Scalars[rule]
		fmt.Fprintf(out, "%s  (scalar, %s)\n  unit: %s\n  meaning: %s\n", rule, strings.Join(s.Paths, " "), s.Unit, strings.TrimSpace(s.Meaning))
		if s.Validity != "" {
			fmt.Fprintf(out, "  validity: %s\n", strings.TrimSpace(s.Validity))
		}
	}
	if !found {
		return fmt.Errorf("custos describe: no field or rule %q", what)
	}
	return nil
}

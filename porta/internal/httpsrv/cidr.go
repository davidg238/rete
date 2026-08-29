// Copyright (c) 2026 Ekorau LLC

// Package httpsrv is the porta gateway's operator HTTP listener — the
// foundation future operator routes register on (the packages that used to be
// named here, v1's MCP and htmx surfaces, went with the rewrite; the extension
// point outlived them because httpsrv depends on a one-method NodeCounter, not
// a store). Provides IP allowlist middleware, /health, and graceful shutdown.
// No app routes live here; consumers do s.Mux.Handle("/<path>", handler).
package httpsrv

import (
	"fmt"
	"net"
	"net/http"
	"strings"
)

// DefaultAllowCIDR returns the RFC1918 + loopback + Tailscale set, as a fresh
// slice per invocation (cobra StringSliceVar shares the backing slice across
// resets — see TestDefaultAllowCIDRReturnsFreshSlice).
//
// 100.64.0.0/10 is the CGNAT range Tailscale assigns to tailnet peers, so the
// operator surface is reachable from the operator's phone/laptop over Tailscale
// out of the box (e.g. while away from the office LAN). Shared by porta's
// operator listener and custos so the two surfaces cannot drift.
func DefaultAllowCIDR() []string {
	return []string{
		"10.0.0.0/8",
		"172.16.0.0/12",
		"192.168.0.0/16",
		"100.64.0.0/10",
		"127.0.0.0/8",
		"::1/128",
	}
}

// AllowCIDR parses raw CIDR strings into *net.IPNet, filtering
// empty/whitespace-only entries (so `--http-allow-cidr=""` from cobra,
// which arrives as []string{""}, cleanly disables the allowlist). All-empty
// input returns nil so the middleware short-circuits to "serve any peer".
// Returns the first parse error encountered, wrapped with the offending
// slice index and raw string (so a multi-value --http-allow-cidr mistake is
// diagnosable); the underlying net.ParseCIDR error stays unwrappable.
func AllowCIDR(raw []string) ([]*net.IPNet, error) {
	var out []*net.IPNet
	for i, s := range raw {
		s = strings.TrimSpace(s)
		if s == "" {
			continue
		}
		_, n, err := net.ParseCIDR(s)
		if err != nil {
			return nil, fmt.Errorf("allow-cidr[%d]=%q: %w", i, s, err)
		}
		out = append(out, n)
	}
	return out, nil
}

// AllowlistMiddleware returns http middleware that rejects (403) any
// request whose peer IP is outside allowed. nil or empty allowed = serve
// any peer (the middleware short-circuits without parsing RemoteAddr).
//
// Peer IP is extracted from r.RemoteAddr via net.SplitHostPort so it
// handles both IPv4 ("1.2.3.4:5") and bracketed IPv6 ("[::1]:5") forms.
// IPv4-mapped IPv6 addresses ("::ffff:192.168.1.5") are normalized to
// their v4 form via ip.To4() so they match IPv4 CIDRs as expected.
//
// A malformed RemoteAddr is rejected defensively — porta is bound to
// LAN and this should never happen in practice, but a parse failure
// must not silently pass.
func AllowlistMiddleware(allowed []*net.IPNet) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		if len(allowed) == 0 {
			return next
		}
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			host, _, err := net.SplitHostPort(r.RemoteAddr)
			if err != nil {
				http.Error(w, "forbidden", http.StatusForbidden)
				return
			}
			ip := net.ParseIP(host)
			if ip == nil {
				http.Error(w, "forbidden", http.StatusForbidden)
				return
			}
			if v4 := ip.To4(); v4 != nil {
				ip = v4
			}
			for _, n := range allowed {
				if n.Contains(ip) {
					next.ServeHTTP(w, r)
					return
				}
			}
			http.Error(w, "forbidden", http.StatusForbidden)
		})
	}
}

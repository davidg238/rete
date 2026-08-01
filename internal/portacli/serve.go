// Copyright (c) 2026 Ekorau LLC

// internal/portacli/serve.go
package portacli

import (
	"context"
	"log"

	"github.com/davidg238/porta/internal/httpsrv"
	"github.com/davidg238/porta/internal/serverstat"
	"github.com/spf13/cobra"
)

// defaultAllowCIDR returns the RFC1918 + loopback + Tailscale set, as a fresh
// slice per invocation (cobra StringSliceVar shares the backing slice across
// resets — see TestDefaultAllowCIDRReturnsFreshSlice).
//
// 100.64.0.0/10 is the CGNAT range Tailscale assigns to tailnet peers, so the
// operator surface is reachable from the operator's phone/laptop over Tailscale
// out of the box (e.g. while away from the office LAN).
func defaultAllowCIDR() []string {
	return []string{
		"10.0.0.0/8",
		"172.16.0.0/12",
		"192.168.0.0/16",
		"100.64.0.0/10",
		"127.0.0.0/8",
		"::1/128",
	}
}

// newServeCmd runs the gateway. During the space-sync rewrite it serves only
// /health and opens no store — the UDP space listener arrives in Task 4 and
// the operator surface in the operator-surface plan. The TFTP listener and its
// --port flag went with v1; bulk transfer returns on the CoAP blockwise plane
// (design §7), so the flag is deliberately absent rather than accepted and
// ignored.
func newServeCmd() *cobra.Command {
	var httpPort int
	var httpBind string
	var httpAllowCIDR []string
	cmd := &cobra.Command{
		Use:   "serve",
		Short: "Run the porta gateway",
		RunE: func(cmd *cobra.Command, _ []string) error {
			ctx := cmd.Context()

			// Process-lifetime stats. The report-outcome and per-transport
			// packet counters have no feeder until the wire task lands; the
			// version/uptime pair that /health reports works today.
			stats := serverstat.New(version, commit, nowSec)

			srv, err := httpsrv.New(httpsrv.Config{
				Bind:      httpBind,
				Port:      httpPort,
				AllowCIDR: httpAllowCIDR,
			}, nil)
			if err != nil {
				return err
			}
			srv.SetStats(stats)
			httpErr := make(chan error, 1)
			go func() { httpErr <- srv.Run(ctx) }()
			log.Printf("porta: serving HTTP on %s:%d (health only; space listener pending)", httpBind, httpPort)

			return awaitServeExit(ctx, nil, httpErr)
		},
	}
	cmd.Flags().IntVar(&httpPort, "http-port", 6970, "operator HTTP port")
	cmd.Flags().StringVar(&httpBind, "http-bind", "0.0.0.0", "operator HTTP bind address")
	cmd.Flags().StringSliceVar(&httpAllowCIDR, "http-allow-cidr",
		defaultAllowCIDR(),
		"CIDR ranges allowed to reach the operator HTTP listener (repeatable; empty = serve any peer)")
	return cmd
}

// awaitServeExit blocks until a listener goroutine exits or ctx is cancelled.
// It is transport-agnostic — two error channels, either of which may be nil —
// so the space listener plugs into the udpErr arm when Task 4 lands.
//
//   - A listener error (non-nil udpErr or httpErr) propagates verbatim.
//   - A clean HTTP exit (httpErr delivers nil) while UDP still runs falls
//     through to wait on UDP.
//   - On ctx cancel (SIGINT/SIGTERM) it DRAINS both channels first, so the
//     listeners and the HTTP server's 5s graceful Shutdown complete before
//     RunE returns and its deferred Close calls fire (#7). A nil channel's
//     select arm blocks forever and the nil guard skips draining it.
func awaitServeExit(ctx context.Context, udpErr, httpErr <-chan error) error {
	select {
	case err := <-udpErr:
		return err
	case err := <-httpErr:
		if err == nil {
			if udpErr != nil {
				return <-udpErr
			}
			return nil
		}
		return err
	case <-ctx.Done():
		if udpErr != nil {
			<-udpErr
		}
		if httpErr != nil {
			<-httpErr
		}
		return nil
	}
}

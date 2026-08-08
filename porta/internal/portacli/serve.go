// Copyright (c) 2026 Ekorau LLC

// internal/portacli/serve.go
package portacli

import (
	"context"
	"log"
	"net"
	"strconv"

	"github.com/davidg238/porta/internal/blob"
	"github.com/davidg238/porta/internal/blockserv"
	"github.com/davidg238/porta/internal/httpsrv"
	"github.com/davidg238/porta/internal/serverstat"
	"github.com/davidg238/porta/internal/space"
	"github.com/davidg238/porta/internal/wire"
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

// newServeCmd runs the gateway: the node-facing UDP space listener plus the
// operator HTTP surface. The two are independent — a node keeps checking in
// with --http-port 0, and the health endpoint stays up if the mesh is quiet.
func newServeCmd() *cobra.Command {
	var udpAddr, httpBind string
	var httpPort, testDropAck int
	var httpAllowCIDR []string
	cmd := &cobra.Command{
		Use:   "serve",
		Short: "Run the porta gateway: UDP space listener + operator HTTP surface",
		RunE: func(cmd *cobra.Command, _ []string) error {
			ctx := cmd.Context()
			st, err := space.Open(dbPath, nowMs)
			if err != nil {
				return err
			}
			defer st.Close()

			// Process-lifetime stats shared by the HTTP surface.
			stats := serverstat.New(version, commit, nowSec)

			srv := wire.NewServer(st, log.Printf)
			if testDropAck > 0 {
				srv.SetDropAckEvery(testDropAck)
				log.Printf("porta: TEST-DROP ACTIVE — every %d-th #ack will be swallowed AFTER commit", testDropAck)
			}
			udpErr := make(chan error, 1)
			go func() { udpErr <- srv.ListenAndServe(ctx, udpAddr) }()
			log.Printf("porta: serving the space on udp %s (db=%s)", udpAddr, dbPath)

			// The block server shares the exchange host but sits one port up
			// (design D1 note): the bulk plane is a separate stateless listener,
			// never multiplexed onto the space socket.
			blocksHost, exPort, err := net.SplitHostPort(udpAddr)
			if err != nil {
				return err
			}
			exPortNum, err := strconv.Atoi(exPort)
			if err != nil {
				return err
			}
			blocksAddr := net.JoinHostPort(blocksHost, strconv.Itoa(exPortNum+1))
			bst, err := blob.Open(blobDir(dbPath))
			if err != nil {
				return err
			}
			blocksSrv := blockserv.New(bst)
			blocksErr := make(chan error, 1)
			go func() { blocksErr <- blocksSrv.ListenAndServe(ctx, blocksAddr) }()
			log.Printf("porta: serving blobs on udp %s", blocksAddr)

			// When --http-port 0, httpErr stays nil so the select arm blocks
			// forever and UDP keeps serving until ctx cancel. (A closed
			// channel would fire the select immediately and exit the serve —
			// the bug found in T5 review.)
			var httpErr chan error
			if httpPort > 0 {
				srv, err := httpsrv.New(httpsrv.Config{
					Bind:      httpBind,
					Port:      httpPort,
					AllowCIDR: httpAllowCIDR,
				}, st)
				if err != nil {
					return err
				}
				srv.SetStats(stats)
				httpErr = make(chan error, 1)
				go func() { httpErr <- srv.Run(ctx) }()
				log.Printf("porta: serving HTTP on %s:%d", httpBind, httpPort)
			}

			return awaitServeExit(ctx, udpErr, blocksErr, httpErr)
		},
	}
	cmd.Flags().StringVar(&udpAddr, "udp", ":6969", "node-facing UDP listen address")
	cmd.Flags().IntVar(&httpPort, "http-port", 6970, "operator HTTP port (0 = disabled)")
	cmd.Flags().StringVar(&httpBind, "http-bind", "0.0.0.0", "operator HTTP bind address")
	cmd.Flags().StringSliceVar(&httpAllowCIDR, "http-allow-cidr",
		defaultAllowCIDR(),
		"CIDR ranges allowed to reach the operator HTTP listener (repeatable; empty = serve any peer)")
	cmd.Flags().IntVar(&testDropAck, "test-drop-ack", 0,
		"TEST ONLY: swallow every Nth outbound #ack AFTER the push is committed (0 = off)")
	return cmd
}

// awaitServeExit blocks until a listener goroutine exits or ctx is cancelled.
// It is transport-agnostic — three error channels; only httpErr may be nil
// (udp and blocks are unconditional listeners, so those two are always live).
//
//   - A listener error (non-nil udpErr, blocksErr or httpErr) propagates
//     verbatim.
//   - A clean HTTP exit (httpErr delivers nil) while UDP/blocks still run
//     falls through to wait on whichever of them reports next.
//   - On ctx cancel (SIGINT/SIGTERM) it DRAINS every channel first, so the
//     listeners and the HTTP server's 5s graceful Shutdown complete before
//     RunE returns and its deferred Close calls fire (#7). A nil channel's
//     select arm blocks forever and the nil guard skips draining it.
func awaitServeExit(ctx context.Context, udpErr, blocksErr, httpErr <-chan error) error {
	select {
	case err := <-udpErr:
		return err
	case err := <-blocksErr:
		return err
	case err := <-httpErr:
		if err == nil {
			select {
			case err := <-udpErr:
				return err
			case err := <-blocksErr:
				return err
			}
		}
		return err
	case <-ctx.Done():
		if udpErr != nil {
			<-udpErr
		}
		if blocksErr != nil {
			<-blocksErr
		}
		if httpErr != nil {
			<-httpErr
		}
		return nil
	}
}

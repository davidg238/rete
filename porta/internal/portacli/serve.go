// Copyright (c) 2026 Ekorau LLC

// internal/portacli/serve.go
package portacli

import (
	"context"
	"log"
	"net"
	"net/http"
	"strconv"
	"time"

	"github.com/davidg238/porta/internal/blob"
	"github.com/davidg238/porta/internal/blockserv"
	"github.com/davidg238/porta/internal/httpsrv"
	"github.com/davidg238/porta/internal/manifest"
	"github.com/davidg238/porta/internal/serverstat"
	"github.com/davidg238/porta/internal/space"
	"github.com/davidg238/porta/internal/wire"
	"github.com/spf13/cobra"
)

// manifestHandler serves the manifest porta enforces — the dev tools'
// fetch-and-pin source (rete#243 D5/D11). The hash rides a header so a
// fetcher can refuse an endpoint that lies about its own bytes.
func manifestHandler(tbl *manifest.Table) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if tbl == nil {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		w.Header().Set("X-Manifest-SHA256", tbl.SHA256)
		w.Write(tbl.Raw)
	}
}

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

// sweepLiveness runs the silence sweep until ctx is cancelled — the fourth
// listener goroutine, alongside UDP, blocks and HTTP.
//
// A sweep failure LOGS AND CONTINUES rather than taking the gateway down: the
// space listener is the node-facing contract and this is an observer of it. An
// observer that can stop the thing it observes is worse than no observer.
func sweepLiveness(ctx context.Context, st *space.Store, tick time.Duration, logf func(string, ...any)) error {
	t := time.NewTicker(tick)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-t.C:
			changes, err := st.Sweep()
			if err != nil {
				// Log it, but do NOT discard `changes`. Sweep isolates per
				// node, so an error for one node arrives ALONGSIDE verdicts
				// that were really committed for others; dropping them here
				// would lose transitions the store already holds.
				logf("porta: liveness sweep: %v", err)
			}
			for _, c := range changes {
				logf("porta: %s is #%s — %d ms since its last exchange, declared budget %d ms",
					c.NodeID, c.Verdict, c.SinceMs, c.BudgetMs)
			}
		}
	}
}

// newServeCmd runs the gateway: the node-facing UDP space listener plus the
// operator HTTP surface. The two are independent — a node keeps checking in
// with --http-port 0, and the health endpoint stays up if the mesh is quiet.
func newServeCmd() *cobra.Command {
	var udpAddr, httpBind, manifestPath, devDocs string
	var httpPort, testDropAck int
	var httpAllowCIDR []string
	// TEST ONLY, on --test-drop-ack's precedent: the sweep's own period. The
	// host gate asserts a verdict and the default 30 s tick outlasts the whole
	// gate, so the alternative is a 31 s sleep in a script that otherwise runs
	// in seconds — a mystery interval that would also be racing the tick.
	var sweepTick time.Duration
	cmd := &cobra.Command{
		Use:   "serve",
		Short: "Run the porta gateway: UDP space listener + operator HTTP surface",
		RunE: func(cmd *cobra.Command, _ []string) error {
			ctx := cmd.Context()
			st, err := space.Open(dbPath, nowMs)
			if err != nil {
				return err
			}
			// rete#231 D6: this process's own presence, recorded so that a
			// verdict formed across an outage can read #unknown instead of
			// asserting the last thing porta heard. A failure here is fatal
			// and not logged-and-continued: a gateway that cannot record
			// whether it was listening will produce confident verdicts it
			// cannot support, which is worse than not starting. The per-tick
			// stamp is inside Store.Sweep, not in the loop below — see the
			// comment there for why the tick owns it.
			if err := st.StartRun(); err != nil {
				return err
			}
			defer st.Close()

			// rete#243: the kind+schema gate. Loading is fatal on error — a
			// half-read contract must not gate — but an ABSENT flag only
			// warns: the gate lands on running gateways one deploy at a time,
			// and the warning is what keeps the off state from being quiet.
			var tbl *manifest.Table
			if manifestPath != "" {
				tbl, err = manifest.Load(manifestPath)
				if err != nil {
					return err
				}
				// Persisted, not merely armed: the CLI doors are their own
				// processes over the same db, and #180/#206 both came through
				// them. From here on every Open of this store gates.
				if err := st.StoreManifest(tbl.Raw); err != nil {
					return err
				}
				log.Printf("porta: manifest gate ON (%s, sha256 %s…) — persisted to the store", manifestPath, tbl.SHA256[:8])
			} else {
				log.Printf("porta: MANIFEST GATE OFF for this flagless serve — no --manifest given; a manifest already stored in the db still gates (rete#243)")
			}

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
			blocksErr := startBlocksPlane(ctx, blockserv.New(bst), blocksAddr, stats, log.Printf)

			// The fourth goroutine: porta's own verdict on which nodes are still
			// talking. It is a ticker and not a read-time view because the
			// failure it must catch is porta hearing nothing, and a view that
			// evaluates only when read is silent exactly when everything is
			// (design 2026-08-10 §6.3).
			sweepErr := make(chan error, 1)
			tick := space.SweepTick
			if sweepTick > 0 {
				tick = sweepTick
			}
			go func() { sweepErr <- sweepLiveness(ctx, st, tick, log.Printf) }()
			log.Printf("porta: sweeping for silent nodes every %s", tick)

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
				srv.Mux.HandleFunc("/manifest", manifestHandler(tbl))
				if devDocs != "" {
					registerDevDocs(srv.Mux, devDocs)
					log.Printf("porta: dev status surface ON — /dev serves %s", devDocs)
				}
				httpErr = make(chan error, 1)
				go func() { httpErr <- srv.Run(ctx) }()
				log.Printf("porta: serving HTTP on %s:%d", httpBind, httpPort)
			}

			return awaitServeExit(ctx, udpErr, blocksErr, sweepErr, httpErr)
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
	cmd.Flags().DurationVar(&sweepTick, "sweep-tick", 0,
		"TEST ONLY: how often to re-form liveness verdicts (0 = space.SweepTick, 30s)")
	cmd.Flags().StringVar(&manifestPath, "manifest", "",
		"path to MANIFEST.cddl — arms the kind+schema gate and serves GET /manifest (empty = gate OFF, loudly)")
	cmd.Flags().StringVar(&devDocs, "dev-docs", "",
		"path to the repo's docs/ tree — serves the dev status surface at /dev (empty = off)")
	return cmd
}

// startBlocksPlane binds and runs the bulk-plane listener — or degrades
// (rete#176). The exchange is the fleet's lifeline and the bulk plane is
// secondary, so a BIND failure (a squatter on exchange+1 at restart) must not
// take the daemon down: it answers a nil channel, an unmissable log line, and
// the reason in stats so /health carries it past the log scrollback. A bind
// SUCCESS keeps today's semantics — a runtime listener death still propagates
// through the returned channel and fells the daemon, because a plane that
// bound and then died is a fault, not a collision.
func startBlocksPlane(ctx context.Context, srv *blockserv.Server, addr string,
	stats *serverstat.Stats, logf func(string, ...any)) <-chan error {
	pc, err := net.ListenPacket("udp", addr)
	if err != nil {
		logf("porta: BLOCKS PLANE DOWN — cannot bind udp %s: %v — serving the "+
			"exchange WITHOUT the bulk plane; blob fetches and deploys will fail "+
			"until porta restarts with the port free", addr, err)
		if stats != nil {
			stats.BlocksPlaneDown(err.Error())
		}
		return nil
	}
	ch := make(chan error, 1)
	go func() { ch <- srv.Serve(ctx, pc) }()
	logf("porta: serving blobs on udp %s", addr)
	return ch
}

// awaitServeExit blocks until a listener goroutine exits or ctx is cancelled.
// It is transport-agnostic — four error channels; httpErr may be nil (no HTTP
// listener) and blocksErr may be nil (rete#176: the bulk plane degraded at
// bind); udp and the liveness sweep are unconditional.
//
//   - A listener error (non-nil udpErr, blocksErr, sweepErr or httpErr)
//     propagates verbatim.
//   - A clean HTTP exit (httpErr delivers nil) while the others still run falls
//     through to wait on whichever of them reports next.
//   - On ctx cancel (SIGINT/SIGTERM) it DRAINS every channel first, so the
//     listeners and the HTTP server's 5s graceful Shutdown complete before RunE
//     returns and its deferred Close calls fire (#7). Draining the sweep is not
//     cosmetic: it holds the store, and Close under an in-flight Sweep is a race.
//     A nil channel's select arm blocks forever and the nil guard skips draining it.
//
// The ctx-Done check runs in its OWN non-blocking select before the main one.
// Putting all five cases in a single select would race: once ctx is cancelled,
// a listener that has already buffered a value makes its case just as "ready"
// as ctx.Done(), and Go picks uniformly among ready cases — so a listener's
// case can win and return early, skipping the drain of the others entirely.
// That is not just a test artifact: it is the same store-Close race this
// drain exists to prevent, so ctx cancellation must take priority outright.
func awaitServeExit(ctx context.Context, udpErr, blocksErr, sweepErr, httpErr <-chan error) error {
	drain := func() error {
		if udpErr != nil {
			<-udpErr
		}
		if blocksErr != nil {
			<-blocksErr
		}
		if sweepErr != nil {
			<-sweepErr
		}
		if httpErr != nil {
			<-httpErr
		}
		return nil
	}
	select {
	case <-ctx.Done():
		return drain()
	default:
	}
	select {
	case err := <-udpErr:
		return err
	case err := <-blocksErr:
		return err
	case err := <-sweepErr:
		return err
	case err := <-httpErr:
		if err == nil {
			// The same priority check as above, and for the same reason: without
			// it, a ctx cancel that races a listener's already-buffered value can
			// let the listener's case win this select too, skipping the drain of
			// the others just as it could in the outer one.
			//
			// NOT drain() here, unlike the OUTER pre-check: by construction we
			// only reach this branch after the main select's `case err :=
			// <-httpErr` has already received httpErr's one and only value (the
			// sender writes once and never closes the channel), so drain()'s
			// `<-httpErr` would block forever. udpErr, blocksErr and sweepErr are
			// still outstanding, so those are what must be drained. The outer
			// pre-check has consumed nothing yet, so it correctly keeps using
			// drain() — do not "unify" the two.
			select {
			case <-ctx.Done():
				if udpErr != nil {
					<-udpErr
				}
				if blocksErr != nil {
					<-blocksErr
				}
				if sweepErr != nil {
					<-sweepErr
				}
				return nil
			default:
			}
			select {
			case err := <-udpErr:
				return err
			case err := <-blocksErr:
				return err
			case err := <-sweepErr:
				return err
			}
		}
		return err
	case <-ctx.Done():
		return drain()
	}
}

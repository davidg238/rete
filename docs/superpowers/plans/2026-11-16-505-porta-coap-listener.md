# porta's CoAP listener — Implementation Plan (rete#505)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `porta serve` answers `POST /space` (a check-in) and `GET /blob/<hash>` (an image, by Block2) over CoAP on UDP `:5683`, IPv6 and IPv4, beside the untouched UDP exchange and block plane.

**Architecture:** A new package `porta/internal/coapsrv` is the whole adapter: it owns `github.com/plgd-dev/go-coap/v3`, forwards `/space` bodies to `wire.Server.HandleFrom` unchanged, and slices `/blob` itself from the request's Block2 option with the library's blockwise layer switched off. `wire.Server.HandleFrom` gains one mutex because it now has two callers. `serve` gains `--coap`.

**Tech Stack:** Go 1.26 (porta's `go 1.26.1` directive; the library declares `go 1.26.0`, so it does not move), `github.com/plgd-dev/go-coap/v3@v3.8.1`, libcoap's `coap-client` (package `libcoap3-bin`) for the interop gate only.

**Spec:** `docs/design/2026-11-16-505-porta-coap-listener-design.md` (approved by David 2026-11-16). Its decisions are not reopened here.

**Library citations.** `$LIB` below is `$(go env GOMODCACHE)/github.com/plgd-dev/go-coap/v3@v3.8.1`. Every library identifier used in this plan was found there and is cited `file:line`. The five facts the plan depends on were also run in a scratch module (2026-11-16, go1.27.4, `go run -race`, no race reported):

| # | Fact | Source |
|---|---|---|
| 1 | Blockwise off: `options.WithBlockwise(false, szx, timeout)`; szx and timeout are ignored when off. Run: a 512-byte `/space` reply arrived whole, with no Block2 option | `$LIB/options/commonOptions.go:538` (applies at `:507`) |
| 2 | Read Block2: `r.GetOptionUint32(message.Block2)` then `blockwise.DecodeBlockOption(v)`; absent → `message.ErrOptionNotFound`. Set Block2: `blockwise.EncodeBlockOption(szx, num, more)` → `message.EncodeUint32(buf, v)` → pass `message.Option{ID: message.Block2, Value: buf[:n]}` as the variadic last argument of `w.SetResponse` | `$LIB/message/pool/message.go:335`, `$LIB/message/option.go:77`, `$LIB/net/blockwise/blockwise.go:80,101`, `$LIB/message/encodeDecodeUint32.go:9`, `$LIB/message/option.go:362`, `$LIB/mux/muxResponseWriter.go:30` |
| 3 | Remote address: `w.Conn().RemoteAddr().String()`. Run: `127.0.0.1:58162` and `[::1]:33248`, the same form `net.Addr.String()` gives the UDP loop's log lines | `$LIB/mux/client.go:30` |
| 4 | Both families: one socket from `net.ListenUDP("udp", ":<port>")` binds `[::]` dual-stack on Linux; the library wraps it with `coapnet.NewUDPConn("udp", c)`. Run: GET and POST answered from `127.0.0.1` and `::1` on the one socket | `$LIB/net/connUDP.go:268` (and `:233–243`, which is the same two calls) |
| 5 | Stop: cancelling a context passed with `options.WithContext` does **not** return `Serve` (run: still blocked after 3 s). `s.Stop()` cancels, closes the listener, and `Serve` returns `nil`. So: `go func() { <-ctx.Done(); s.Stop() }()` | `$LIB/udp/server/server.go:245` (`Serve`), `:333` (`Stop`) |

Other identifiers used: `udp.NewServer` `$LIB/udp/server.go:5`; `options.WithMux` `$LIB/options/commonOptions.go:120`; `options.WithErrors` `:212`; `mux.NewRouter` `$LIB/mux/router.go:67` (unmatched path → 4.04 at `:78`); `(*mux.Router).Handle` `:140` (pattern `/blob/{hash}` → `r.RouteParams.Vars["hash"]`, `$LIB/mux/message.go:8`, default var pattern `[^/]+`, `$LIB/mux/regexp.go`); `r.ReadBody` `$LIB/message/pool/message.go:519`; `codes.GET/POST/Changed/Content/BadRequest/BadOption/NotFound/MethodNotAllowed` `$LIB/message/codes/codes.go:16,17,31,32,34,36,38,39`; `message.AppCBOR` / `message.AppOctets` `$LIB/message/option.go:193,188`; `blockwise.SZX256` `$LIB/net/blockwise/blockwise.go:70`; `SZX.Size()` `$LIB/net/blockwise/internal/blocks/types.go:21`; client `udp.Dial` `$LIB/udp/client.go:25`, `Get` / `Post` `$LIB/net/client/client.go:72,143`. The design's claim that an unanswered confirmable request gets an empty ACK holds: `$LIB/udp/client/conn.go:1205` (`sendJustAcknowledgeMessage`).

**Library defaults kept (design §9):** `MaxMessageSize` 64 KiB (`$LIB/options/config/common.go:42`) and the 16 s per-peer inactivity close (`$LIB/udp/server/config.go`, `DefaultConfig`). No option sets them, so no constant is invented.

## Global Constraints

- CoAP beside UDP; nsl nodes, `internal/blockserv` and `internal/space` do not change (design §2, §5.2).
- `POST /space`: body is exactly a UDP check-in datagram; reply 2.04 Changed, `application/cbor`, body exactly `HandleFrom`'s bytes. A refusal stays an `#err` in a 2.04 (§5.1).
- `GET /blob/<hash>`: `<hash>` is 64 lowercase hex; 2.05 Content, `application/octet-stream`, by Block2. Unknown → 4.04, malformed → 4.00. Unknown path 4.04, wrong method 4.05 (§5.1).
- Block size `blockserv.BlockSize` or a smaller one the client asks for; no Block2 → block 0 at `blockserv.BlockSize`. Nothing kept per transfer (§5.1).
- Library blockwise is OFF; only `/blob` is blockwise and `coapsrv` does it (§5.1).
- `HandleFrom` takes one mutex for its whole body; `/blob` takes none (§5.2).
- `--coap`, default `:5683`, empty disables. No other new flag, constant or knob (§5.2). `--test-refuse-all` covers CoAP for free; `--test-drop-all`/`--test-drop-ack` do not, and `serve` says so (§5.2).
- No `MANIFEST.cddl` change (§2, §6). No DTLS (§2).
- Every new Go file starts `// Copyright (c) 2026 Ekorau LLC`; `gofmt -l .` in `porta/` prints nothing before every commit.
- Commits: `git add` named paths only (never `-a`), subject ends `refs #505`, body ≤ 5 lines, last line `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never a closing keyword. Do not push (that is `/conclude`'s).
- Gate results are read with `-count=1` (Go's cache reads as a pass).

## Review Focus

1. **A client that asks for a block larger than 256 B** (libcoap and Zephyr may open with SZX 1024): the reply must be the block at the *same byte offset* at SZX 256, renumbered (RFC 7959 §2.4) — not block N×4 bytes off. Test: Task 2 `TestWindow` rows and `TestBlobWhole` with a SZX1024 client.
2. **A blob whose length is an exact multiple of 256, or empty:** the last block carries `more=false` and no phantom empty block exists; asking for block `len/256` is 4.02. Test: Task 2 `TestWindow` and `TestBlobWhole` sizes 0, 256, 512.
3. **Port 5683 already held** (a second `porta serve` on one host, or a harness beside a dev serve): `serve` must fail at start-up with an error that names `--coap ""`, before any listener runs — not die later, not half-start. Test: Task 4 `TestStartCoAPFailsWhenPortTaken`. *(Plan's call — the design does not say; fail-fast is the simpler of the two house precedents, see Task 4.)*
4. **The same push arriving twice over CoAP** (a lost ACK, a retransmit): the second reply is the same `#ack` and nothing is ingested twice. Test: Task 1 `TestPushTwiceAcksOnce`.
5. **A body that is not a record, and hashes that are almost right** (uppercase hex, 63 characters, an extra path segment): an empty or garbage POST body is a 2.04 `#err`, never a 4.xx; the hash variants are 4.00, 4.00 and 4.04. Test: Task 1 `TestGarbageBodyIsErrRecord`, Task 2 `TestBlobRefusals`.

---

## File map

| File | Change | Task |
|---|---|---|
| `porta/go.mod`, `porta/go.sum` | add `github.com/plgd-dev/go-coap/v3 v3.8.1` | 1 |
| `porta/internal/coapsrv/coapsrv.go` | new: `Server`, `New`, `Serve`, the router, the `/space` handler | 1 |
| `porta/internal/coapsrv/blob.go` | new: `maxSZX`, `window`, the `/blob` handler | 2 |
| `porta/internal/coapsrv/coapsrv_test.go` | new: harness + `/space` tests | 1 |
| `porta/internal/coapsrv/blob_test.go` | new: `/blob` tests | 2 |
| `porta/internal/coapsrv/overlap_test.go` | new: UDP+CoAP overlap under `-race` | 3 |
| `porta/internal/wire/server.go` | `mu sync.Mutex` held for `HandleFrom`'s body | 3 |
| `porta/internal/portacli/serve.go`, `serve_test.go` | `--coap`, `startCoAP`, `awaitServeExit` gains `coapErr` | 4 |
| `porta/docs/CLI.md`, `docs/tutorials/03-porta.md` | the flag and the start-up line | 4 |
| `porta/tests/coap_interop.sh`, `porta/tests/fixtures/pull.hex` | new: the libcoap gate and its record | 5 |
| `protocol/PROTOCOL-NSL.md`, `porta/docs/ARCHITECTURE.md`, `porta/CLAUDE.md` | §5.3 table of the design, plus the contradictions below | 7 |

---

### Task 1: the dependency and `POST /space`

**Files:**
- Modify: `porta/go.mod`, `porta/go.sum`
- Create: `porta/internal/coapsrv/coapsrv.go`, `porta/internal/coapsrv/coapsrv_test.go`

**Interfaces:**
- Consumes: `wire.NewServer(st *space.Store, logf func(string, ...any)) *wire.Server`; `(*wire.Server).HandleFrom(req []byte, peer string) []byte`; `wire.EncodePull`, `wire.EncodePush`, `wire.DecodeBatch`, `wire.DecodeAck`; `blob.Open(dir string) (*blob.Store, error)`.
- Produces (later tasks rely on these exact names):
  - `func New(ex *wire.Server, blobs *blob.Store, logf func(string, ...any)) *Server`
  - `func (s *Server) Serve(ctx context.Context, pc *net.UDPConn) error` — takes ownership of `pc`; returns `nil` after `ctx` is cancelled, a non-nil error only if the listener dies otherwise. Mirrors `blockserv.Server.Serve`.
  - test helpers in `coapsrv_test.go`: `harness(t) *rig` where `rig` has fields `port int`, `st *space.Store`, `blobs *blob.Store`, `ex *wire.Server`; `dial(t, host string, port int, bw bool, szx blockwise.SZX) *client.Conn` (`client` = `github.com/plgd-dev/go-coap/v3/udp/client`). The store's clock in `harness` is a `sync/atomic.Int64` (`clock.Add(10)`) — `wire`'s own `testServer` clock is a bare `int64` and would itself be the race under Task 3's `-race` run, a check that shares a cause with the failure.

- [ ] **Step 1: Add the dependency (no code imports it yet)**

Run: `cd porta && go get github.com/plgd-dev/go-coap/v3@v3.8.1`
Expected: `go.mod` gains the require line; the `go 1.26.1` directive is unchanged (`grep '^go ' go.mod`).

- [ ] **Step 2: Write the failing tests** in `coapsrv_test.go`. `harness` binds `net.ListenUDP("udp", &net.UDPAddr{})` (dual-stack, port 0), starts `New(ex, blobs, nil).Serve(ctx, pc)` in a goroutine (a nil, i.e. no-op, logf: `t.Logf` from a library goroutine after the test ends panics), and on cleanup cancels and requires `Serve` to return `nil` within 5 s.

```go
func TestPullMatchesHandleFrom(t *testing.T)      // write goal/name "alpha" for n1; req := wire.EncodePull("n1", 0, 7, 0, 0);
                                                 // POST /space via CoAP (blockwise-off client) → code codes.Changed, ContentFormat message.AppCBOR;
                                                 // then got2 := rig.ex.HandleFrom(req, "") → bytes.Equal(coapBody, got2)
                                                 // (a cursor-0 pull of a cell replays identically)
func TestPushAckedOverCoAP(t *testing.T)        // wire.EncodePush("n1", 1, one tel/log entry seq 1) → DecodeAck == 1
func TestPushTwiceAcksOnce(t *testing.T)        // the same push POSTed twice → both DecodeAck == 1; st.Ingest's skip path, not a second row
                                                 // rig.st.Events("n1", 100, 0) holds exactly one row with Path "tel/log" (store.go:781; check boot arg semantics there)
func TestRefusalIsErrIn204(t *testing.T)        // rig.ex.SetRefuseAll(true) before any request → code Changed, body decodes to cval.Record Tag "err"
func TestGarbageBodyIsErrRecord(t *testing.T)   // POST empty body and POST []byte{0xff} → code Changed, Tag "err"
func TestSpaceMethodAndPath(t *testing.T)       // GET /space → codes.MethodNotAllowed; GET /nope → codes.NotFound
func TestSpaceReplyIsNeverSplit(t *testing.T)   // write 3 goal cells of 120-byte strings; pull → len(body) > blockserv.BlockSize,
                                                 // and resp.GetOptionUint32(message.Block2) returns message.ErrOptionNotFound
func TestBothFamilies(t *testing.T)             // TestPullMatchesHandleFrom's POST from "127.0.0.1" and from "::1" → both codes.Changed, Tag "batch"
```

- [ ] **Step 3: Run to see them fail**

Run: `cd porta && go test -count=1 ./internal/coapsrv/`
Expected: FAIL — `undefined: New`.

- [ ] **Step 4: Implement `coapsrv.go`**

- `Server` holds `ex`, `blobs`, `logf`. `New` defaults a nil `logf` to a no-op, as `wire.NewServer` does.
- `Serve`: build a `mux.NewRouter()`; `Handle("/space", …)` (Task 2 adds `/blob/{hash}`); `srv := udp.NewServer(options.WithMux(r), options.WithBlockwise(false, maxSZX, 0), options.WithErrors(func(err error) { s.logf("porta: coap: %v", err) }))` — `maxSZX` is Task 2's constant; until then declare it in `blob.go` as `var maxSZX = blockwise.SZX256` so Task 1 compiles. Then `go func() { <-ctx.Done(); srv.Stop() }()` and `return srv.Serve(coapnet.NewUDPConn("udp", pc))` (fact 5, fact 4).
- `/space` handler: `r.Code() != codes.POST` → `w.SetResponse(codes.MethodNotAllowed, message.TextPlain, nil)` (`message.TextPlain` `$LIB/message/option.go:182`). Otherwise `body, err := r.ReadBody()`; on error → 4.00. `resp := s.ex.HandleFrom(body, w.Conn().RemoteAddr().String())`; `w.SetResponse(codes.Changed, message.AppCBOR, bytes.NewReader(resp))`. A `SetResponse` error is logged with `s.logf`, never panicked on. The package comment says: the adapter only; refusals are records; no per-request log line (HandleFrom already logs its rejections, and the peer string reads as UDP's does).

- [ ] **Step 5: Run to see them pass, then tidy**

Run: `cd porta && go mod tidy && go test -count=1 ./internal/coapsrv/ && go vet ./internal/coapsrv/ && gofmt -l .`
Expected: `ok`; vet silent; gofmt prints nothing; `grep '^go ' go.mod` still `go 1.26.1`.

- [ ] **Step 6: Commit**

```bash
git add porta/go.mod porta/go.sum porta/internal/coapsrv/coapsrv.go porta/internal/coapsrv/blob.go porta/internal/coapsrv/coapsrv_test.go
git commit -m "coapsrv: POST /space over CoAP, library blockwise off; refs #505" -m "go-coap v3.8.1 added; a check-in is one datagram each way.
A refusal stays an #err in a 2.04.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `GET /blob/<hash>` by Block2

**Files:**
- Modify: `porta/internal/coapsrv/blob.go`, `porta/internal/coapsrv/coapsrv.go` (register the route)
- Create: `porta/internal/coapsrv/blob_test.go`

**Interfaces:**
- Consumes: Task 1's `harness`, `dial`; `blob.Store.Put`, `blob.Store.Get(hash []byte) ([]byte, error)`; `blockserv.BlockSize`.
- Produces:
  - `var maxSZX = blockwise.SZX256` — the block size `blockserv.BlockSize` names, as an SZX; pinned to it by a test, never by a second literal 256.
  - `func window(n int64, block2 uint32, has bool) (off, end int64, reply uint32, err error)` — which bytes of an `n`-byte blob a request names, and the Block2 value the reply carries. Task 6 breaks this function.

- [ ] **Step 1: Write the failing tests**

```go
func TestMaxSZXIsBlockSize(t *testing.T)   // maxSZX.Size() == blockserv.BlockSize
func TestWindow(t *testing.T)              // table, each row (n, req szx/num or absent) → (off, end, reply szx/num/more, err?):
	// n=600 absent            → 0,256   SZX256 num0 more
	// n=600 SZX256 num2       → 512,600 SZX256 num2 !more
	// n=600 SZX64  num3       → 192,256 SZX64  num3 more        (smaller size honoured)
	// n=2000 SZX1024 num1     → 1024,1280 SZX256 num4 more      (larger size: same offset, renumbered)
	// n=512 SZX256 num1       → 256,512 SZX256 num1 !more       (exact multiple)
	// n=512 SZX256 num2       → err                             (past end)
	// n=0 absent              → 0,0 SZX256 num0 !more           (empty blob)
	// n=0 SZX256 num1         → err
	// any n, szx 7 (BERT)     → err
func TestBlobWhole(t *testing.T)           // sizes 0,1,256,512,600; for each, blockwise-ON clients at SZX1024 and SZX16:
                                            // GET /blob/<hex> → codes.Content, message.AppOctets, sha256(body) == the stored hash
func TestColdBlock(t *testing.T)           // design §8.2: FRESH conn, blockwise OFF, first and only request asks SZX256 num 2 of a 600-byte blob
                                            // → body == blob[512:600]; reply Block2 decodes to num 2, more false
func TestBlobRefusals(t *testing.T)        // 64 lowercase hex not stored → NotFound; 64 UPPERCASE hex → BadRequest; 63 hex → BadRequest;
                                            // "/blob/<64hex>/x" → NotFound; POST /blob/<hash> → MethodNotAllowed; num past end → BadOption
```

- [ ] **Step 2: Run to see them fail**

Run: `cd porta && go test -count=1 ./internal/coapsrv/`
Expected: FAIL — `undefined: window`; `TestBlobWhole` 4.04 (route not registered).

- [ ] **Step 3: Implement `window` and the handler in `blob.go`**

`window`: when `!has`, the request is SZX `maxSZX` number 0. Decode with `blockwise.DecodeBlockOption`; a decode error or szx > `blockwise.SZX1024` (BERT is not UDP) is `err`. `off = num × reqSZX.Size()`. Reply SZX = the smaller of the request's and `maxSZX`; reply num = `off / replySZX.Size()` (exact, because sizes are powers of two and the reply size never exceeds the request's). `off > n`, or `off == n && n > 0`, is `err`. `end = min(off+size, n)`; `more = end < n`; `reply` from `blockwise.EncodeBlockOption(replySZX, replyNum, more)`.

Handler (registered as `"/blob/{hash}"`): non-GET → 4.05. `hash := r.RouteParams.Vars["hash"]`; not exactly 64 chars of `[0-9a-f]` → 4.00. `hex.DecodeString` → `s.blobs.Get(h)`; `errors.Is(err, fs.ErrNotExist)` → 4.04, other error → 5.00 (`codes.InternalServerError`, `$LIB/message/codes/codes.go:48`, logged). Block2: `v, err := r.GetOptionUint32(message.Block2)`; `errors.Is(err, message.ErrOptionNotFound)` → `has=false`; other error → 4.02. `window` error → 4.02 (`codes.BadOption`). Else `SetResponse(codes.Content, message.AppOctets, bytes.NewReader(body[off:end]), message.Option{ID: message.Block2, Value: enc})` with `enc` from `message.EncodeUint32`. Block2 is set on every 2.05, including a one-block blob. The whole blob is read per request (`Store.Get`, design §9's builder's call: no new store method; earned by a measured image size where it costs).

- [ ] **Step 4: Run to see them pass**

Run: `cd porta && go test -count=1 ./internal/coapsrv/ && gofmt -l .`
Expected: `ok`; gofmt silent.

- [ ] **Step 5: Commit**

```bash
git add porta/internal/coapsrv/blob.go porta/internal/coapsrv/blob_test.go porta/internal/coapsrv/coapsrv.go
git commit -m "coapsrv: GET /blob/<hash>, Block2 sliced by the handler, stateless; refs #505" -m "A larger asked size is answered at 256 B, same offset, renumbered.
A cold request for block N returns block N.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: one mutex in `HandleFrom`

**Files:**
- Modify: `porta/internal/wire/server.go`
- Create: `porta/internal/coapsrv/overlap_test.go`

**Interfaces:**
- Consumes: Task 1's `harness`, `dial`; `(*wire.Server).Serve(ctx, pc net.PacketConn) error`.
- Produces: `wire.Server` safe for concurrent `HandleFrom` callers. No signature changes.

- [ ] **Step 1: Write the test** `TestOverlappingCheckInsDoNotCollide`: in the same rig, also bind a UDP socket and run `rig.ex.Serve(ctx, udpPC)`. Write one goal cell for node `n1`. Start 2 × 40 goroutines released together by a closed channel: half send `wire.EncodePull("n1", 0, 1, 0, 0)` as a plain UDP datagram (own socket, 2 s read deadline), half POST the same pull over CoAP (own conn each). Assert: every reply arrives, and none decodes to an `#err` record (on failure print its `msg`).

- [ ] **Step 2: Run it with the mutex absent**

Run: `cd porta && go test -race -count=20 -run TestOverlappingCheckInsDoNotCollide ./internal/coapsrv/`
Expected: FAIL is expected (an `#err` from the store's read-then-write in `NoteCheckIn`, e.g. `database is locked`, or a race report). **Record what happened, either way, for the commit body.** If it passes all 20 runs, say so: the mutex is the design's decision (§5.2) and this test is its regression guard, not its evidence.

- [ ] **Step 3: Add the mutex**

`Server` gains `mu sync.Mutex`. `HandleFrom` takes `s.mu.Lock(); defer s.mu.Unlock()` as its first statement, before the `refuseAll` check. Its doc comment gains one paragraph: two listeners (the UDP loop and `coapsrv`) call it, so check-ins are handled one at a time whichever listener they came in on (rete#505); `/blob` does not pass through here. The setters' "not concurrency-safe … set before the loop starts" comments stay true and are not touched.

- [ ] **Step 4: Run under the race detector**

Run: `cd porta && go test -race -count=5 ./internal/coapsrv/ ./internal/wire/`
Expected: `ok` for both; no `DATA RACE`.

- [ ] **Step 5: Commit**

```bash
git add porta/internal/wire/server.go porta/internal/coapsrv/overlap_test.go
git commit -m "wire: HandleFrom holds one mutex; it has two listeners now; refs #505" -m "<one line: what Step 2 showed without the mutex, in 20 runs>
Overlapping UDP+CoAP pulls for one node, none refused, under -race.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: `serve --coap`

**Files:**
- Modify: `porta/internal/portacli/serve.go`, `porta/internal/portacli/serve_test.go`, `porta/docs/CLI.md`, `docs/tutorials/03-porta.md`

**Interfaces:**
- Consumes: `coapsrv.New`, `(*coapsrv.Server).Serve`.
- Produces:
  - `func startCoAP(ctx context.Context, srv *coapsrv.Server, addr string, logf func(string, ...any)) (<-chan error, error)` — `addr == ""` → `(nil, nil)`, the listener is off. Otherwise `net.ResolveUDPAddr("udp", addr)` + `net.ListenUDP`; a bind error is returned as `fmt.Errorf("coap: cannot bind udp %s: %w — pass --coap \"\" to run without the CoAP listener", addr, err)`; on success logs `porta: serving CoAP on udp %s (POST /space, GET /blob/<hash>)` and returns the `Serve` goroutine's channel.
  - `func awaitServeExit(ctx context.Context, udpErr, blocksErr, coapErr, sweepErr, httpErr <-chan error) error` — `coapErr` may be nil, handled exactly like `blocksErr` everywhere (drain, both selects, the inner `httpErr==nil` branch).

**Why fail-fast and not degrade (plan's call, design silent):** `startBlocksPlane` degrades because the exchange outranks the bulk plane (rete#176). A ztoit node has no other plane, so a CoAP listener that silently is not there would look like a fleet of dead nodes. Failing at start-up, with the error naming the off switch, keeps one rule: a plane a node depends on either starts or `serve` does not.

- [ ] **Step 1: Write the failing tests** in `serve_test.go`:

```go
func TestNewServeCmdRegistersFlags  // extend the name list with "coap"; and newServeCmd().Flags().Lookup("coap").DefValue == ":5683"
func TestStartCoAPOffWhenEmpty(t *testing.T)         // startCoAP(ctx, coapsrv.New(nil, nil, nil), "", logf) → nil channel, nil error
func TestStartCoAPFailsWhenPortTaken(t *testing.T)   // squatter net.ListenPacket("udp","127.0.0.1:0"); startCoAP on its address
                                                     // → nil channel, error containing `--coap ""`
func TestStartCoAPServesAndExitsOnCancel(t *testing.T) // "127.0.0.1:0" → live channel; cancel → channel delivers nil within 5 s
func TestAwaitServeExitDrainsCoAPOnCtxCancel(t *testing.T) // as TestAwaitServeExitDrainsOnCtxCancel, with coapErr: must not return until coapErr is sent
```

Every existing `awaitServeExit(ctx, udpErr, blocksErr, X, Y)` call in the file gains a `nil` third argument (the `coapErr` slot).

- [ ] **Step 2: Run to see them fail**

Run: `cd porta && go test -count=1 ./internal/portacli/`
Expected: FAIL — `undefined: startCoAP`; too many arguments to `awaitServeExit`.

- [ ] **Step 3: Implement**

- Flag: `cmd.Flags().StringVar(&coapAddr, "coap", ":5683", "node-facing CoAP listen address: POST /space, GET /blob/<hash> (empty = disabled)")`.
- In `RunE`, open the blob store and call `startCoAP` **before** `go srv.ListenAndServe(...)` starts, so a bind failure returns before any listener goroutine runs (the existing code opens `bst` after the UDP goroutine; move `blob.Open` up). Pass the same `bst` to `startBlocksPlane`.
- If `coapAddr != ""` and (`testDropAck > 0` or `testDropAll`), log once: `porta: --test-drop-ack/--test-drop-all cover UDP only — the CoAP listener on %s still answers (rete#505)`.
- `awaitServeExit(ctx, udpErr, blocksErr, coapErr, sweepErr, httpErr)`; update its doc comment's channel count and list.

- [ ] **Step 4: Run to see them pass, and run serve by hand**

Run: `cd porta && go test -count=1 ./internal/portacli/ && gofmt -l .`
Expected: `ok`; gofmt silent.

Run: `cd porta && go build -o /tmp/porta-505 ./cmd/porta && d=$(mktemp -d) && /tmp/porta-505 serve --db $d/p.db --udp 127.0.0.1:37969 --http-port 0 --coap 127.0.0.1:37683 --test-drop-all > $d/log 2>&1 & sleep 1; cat $d/log; kill %1`
Expected: the log has `serving CoAP on udp 127.0.0.1:37683` and the `cover UDP only` line. Repeat with `--coap ""`: neither line.

- [ ] **Step 5: Update the pages the binary is checked against**

- `porta/docs/CLI.md` `## serve`: add `[--coap :5683]` to the synopsis; a `--coap` row in the flag table (`:5683`, CoAP's registered port; `POST /space` and `GET /blob/<hash>`; empty disables; a bind failure stops `serve`); the transcript gains the `serving CoAP` line; the listener inventory gains a **CoAP** bullet; the test-only table's `--test-drop-all` and `--test-drop-ack` rows say "UDP only".
- `docs/tutorials/03-porta.md`: the expected-output block after the first `bash gate` block gains `porta: serving CoAP on udp :5683 (POST /space, GET /blob/<hash>)` in its place in the order `serve` prints.

Run: `bash porta/docs/cli-gate.sh`
Expected: PASS (exit 0). Then delete the `--coap` row and rerun: FAIL naming `--coap`; restore.

- [ ] **Step 6: Commit**

```bash
git add porta/internal/portacli/serve.go porta/internal/portacli/serve_test.go porta/docs/CLI.md docs/tutorials/03-porta.md
git commit -m "serve: --coap (default :5683) starts coapsrv; a bind failure stops serve; refs #505" -m "The drop switches say they cover UDP only. CLI.md and tutorial 03 name the listener.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: the interop gate, against libcoap

**Files:**
- Create: `porta/tests/coap_interop.sh`, `porta/tests/fixtures/pull.hex`
- Modify: `porta/internal/coapsrv/coapsrv_test.go` (the fixture's pin)

**Interfaces:**
- Consumes: the `porta` binary (`serve --coap`, `key gen`, `blob put --node`).
- Produces: `porta/tests/coap_interop.sh` — exit 0 and `COAP INTEROP OK`, or exit 1 and `FAIL: …`. There is no skip exit.

- [ ] **Step 1: The fixture and its pin**

`pull.hex` is one line: the hex of `wire.EncodePull("0123456789ab", 0, 1, 0, 0)`. Test `TestInteropFixtureIsThePull` in `coapsrv_test.go` reads `../../tests/fixtures/pull.hex`, decodes it, and requires it equal that call's bytes — so the gate's record cannot drift from the encoder. Run it first with the file absent (FAIL: no such file), then write the file from the encoder's output, then PASS.

- [ ] **Step 2: Write the gate**

`coap_interop.sh`, `set -euo pipefail`:
1. `command -v coap-client` or `FAIL: no coap-client — sudo apt install libcoap3-bin` and exit 1. The comment above it says why it fails rather than skips: the Go tests share the library with the server; this is the check that does not share a cause (design §8.3).
2. Build porta into a `mktemp -d`; start `serve --db $W/p.db --udp 127.0.0.1:37969 --http-port 0 --coap :37683`; wait (≤ 5 s) for `serving CoAP`; trap kills it.
3. `porta --db $W/p.db key gen`; a 1000-byte file from `/dev/urandom`; `porta --db $W/p.db blob put --node 0123456789ab $W/img` → read `hash` from its output.
4. **Check-in over IPv6:** `xxd -r -p pull.hex > $W/pull.cbor`; `coap-client -m post -t 60 -f $W/pull.cbor -o $W/reply.cbor "coap://[::1]:37683/space"`. Require `reply.cbor` to contain the 32 hash bytes (a `python3 -c` byte search): the reply is the batch carrying the node's `goal/apps/main`, which only `HandleFrom` could have produced.
5. **Blob over IPv4:** `coap-client -m get -b 256 -o $W/got "coap://127.0.0.1:37683/blob/$hash"`; require `sha256sum $W/got` to equal `$hash`.
6. `echo "COAP INTEROP OK"`.

The flags (`-m`, `-t`, `-f`, `-o`, `-b`) are the ones go-coap's own libcoap tests pass (`$LIB/udp/qblock_libcoap_test.go:532–540`, `$LIB/udp/server_libcoap_test.go:440–446`); confirm each against `coap-client -h` once it is installed.

- [ ] **Step 3: Run it before libcoap is installed**

Run: `bash porta/tests/coap_interop.sh; echo "exit $?"`
Expected: `FAIL: no coap-client — sudo apt install libcoap3-bin`, `exit 1`. This is the fail-not-skip proof; keep the output for the #505 comment.

- [ ] **Step 4: Commit the gate**

```bash
git add porta/tests/coap_interop.sh porta/tests/fixtures/pull.hex porta/internal/coapsrv/coapsrv_test.go
git commit -m "tests: coap_interop.sh, libcoap's coap-client against porta serve; refs #505" -m "Check-in over IPv6 returns the node's goal/apps/main; the blob over IPv4 hashes right.
No coap-client is a FAIL, not a skip.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 5: WAIT FOR DAVID.** Ask, as a closing item: "May I have `sudo apt install libcoap3-bin` on msi? (yes/no)" — he runs it; the session does not. Do not continue to Step 6 until `command -v coap-client` answers.

- [ ] **Step 6: Run it green**

Run: `bash porta/tests/coap_interop.sh`
Expected: `COAP INTEROP OK`. If a flag differs from `coap-client -h`, fix the script, rerun, and amend nothing: commit the fix as its own `refs #505` commit.

---

### Task 6: the guard is shown to fail

No commit; the result goes in the #505 comment at close-out.

- [ ] **Step 1: Break the offset by one** — in `window`, `off = num × size + 1`.
- [ ] **Step 2: Run the three proofs**

Run: `cd porta && go test -count=1 ./internal/coapsrv/ 2>&1 | grep -E '^(--- FAIL|ok|FAIL)'; bash tests/coap_interop.sh; echo "interop exit $?"`
Expected: `TestWindow`, `TestColdBlock` and `TestBlobWhole` FAIL; the interop gate FAILS on the sha256. Record exactly which went red. A proof that stays green under this break is a blind guard: report it and fix the test before going on.

- [ ] **Step 3: Restore and confirm**

Run: `git diff --stat porta/internal/coapsrv/` (shows only `blob.go`), `git checkout porta/internal/coapsrv/blob.go`, then `go test -count=1 ./internal/coapsrv/` → `ok`.

---

### Task 7: the documents (design §5.3)

**Files:**
- Modify: `protocol/PROTOCOL-NSL.md`, `porta/docs/ARCHITECTURE.md`, `porta/CLAUDE.md`

The design's §5.3 table, plus three places in the same documents that say the opposite of what §0 will (design posture: two documents that disagree is a finding; resolve it).

- [ ] **Step 1: `protocol/PROTOCOL-NSL.md`**
  - **Status paragraph and §0:** rewritten. One protocol. porta has no node-kind column and does not distinguish node kinds; the `app` cell already carries any image. A Toit-on-Zephyr node (ztoit) is a second implementation of the node end. The v1 Toit nodes and the `st` kind are history (archive link stays). The north-star box goes: it has happened.
  - **§5.3 Transport bullet:** CoAP is built: `POST /space` carrying the same record, `GET /blob/<hash>` by Block2 at ≤ 256 B, port 5683, beside plain UDP until rete#507. Refusals stay `#err` records in a 2.04. Library blockwise off; one datagram each way for a check-in. The DTLS measurement no longer gates CoAP; it gates DTLS. **The bulk-plane paragraph's** "still gated on the §7 q7 DTLS footprint" clause is cut to match.
  - **§6 first bullet** ("Porta serves v1 and this protocol side by side, selected by node `kind`") and the north-star bullet: rewritten to the two transports of one protocol, nsl on UDP, ztoit on CoAP, and rete#507 as the step that deletes the UDP planes.
  - **§7 q7:** the DTLS RAM measurement gates DTLS only; CoAP without DTLS is built.
  - **§8:** a dated entry `(2026-11-16) rete#505 BUILT: CoAP without DTLS (David)`: envelope `POST /space` over resources; `github.com/plgd-dev/go-coap/v3` over hand-written; beside UDP; no manifest change (rete#508). Record: the design doc.

- [ ] **Step 2: `porta/docs/ARCHITECTURE.md`**
  - §1 picture: "four loops" → five, with `coap  udp :5683  /space /blob`; the arrow label below the gateway names CoAP as well as plain UDP. The planes table's southbound transport cell names CoAP.
  - §3.3's last-but-one sentence (the CoAP move "costs a reframing") → it was done in rete#505, and the binary plane stays for nsl nodes until rete#507.
  - §4: "two transports" → three; a **CoAP** bullet (`:5683`, `internal/coapsrv`, the two resources, the one mutex in `HandleFrom` and why, no admission rule as UDP has none, no DTLS — design §7 in one sentence); the dependency named (go-coap v3.8.1, and its cost: ten modules, pion's DTLS among them).

- [ ] **Step 3: `porta/CLAUDE.md`**
  - Key-decisions line "Bulk transfer … deferred to a CoAP blockwise plane" → built for CoAP clients (rete#505), the binary plane serving nsl nodes until rete#507. Stack line "Go 1.26, sqlite …, cobra" gains go-coap.
  - Layout gains `porta/internal/coapsrv/`.

- [ ] **Step 4: Check and commit**

Run: `rg -n "kind: \"toit\"|two node kinds|selected by node .kind|four loops|deferred to a CoAP" protocol/PROTOCOL-NSL.md porta/docs/ARCHITECTURE.md porta/CLAUDE.md .`
Expected: no hit in the three edited files (hits under `review/` or `archive/` are listed in the close-out comment for David, never edited).

```bash
git add protocol/PROTOCOL-NSL.md porta/docs/ARCHITECTURE.md porta/CLAUDE.md
git commit -m "docs: CoAP is built beside UDP; one protocol, porta knows no node kinds; refs #505" -m "PROTOCOL-NSL §0, §5.3, §6, §7 q7, §8; ARCHITECTURE §1, §3.3, §4; porta/CLAUDE.md.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: close-out

- [ ] **Step 1: The gates** (design §8.6), each read fresh:

```
for m in protocol porta custos; do (cd $m && go test -count=1 ./...); done    # three ok lines, no FAIL
cd porta && go test -race -count=1 ./internal/coapsrv/ ./internal/wire/       # ok, no DATA RACE
bash porta/docs/cli-gate.sh                                                   # PASS
bash porta/tests/coap_interop.sh                                              # COAP INTEROP OK
cd nodus/oevm && bash tests/porta_sync_host.sh                                # PORTA SYNC OK
```

`porta_sync_host.sh` holds `/tmp/rete-nsl-suite.lock`; run it with no suite running. Its `serve` now also binds `:5683` (no `--coap` there); a bind failure there means another process holds the port — find it (`ss -ulpn | grep 5683`), do not add `--coap ""` to the harness.

- [ ] **Step 2: The record**
  - Comment on #505 (≤ 5 lines): the gates above with their result lines; Task 3's without-mutex result; Task 6's red set; the Task 5 FAIL-before-install output.
  - Comment on davidg238/ztoit#10: the check-in is `POST /space` with the record as the body (not `GET /space?id=&cursor=`), reply 2.04 `application/cbor`; refusals are `#err` records; blobs `GET /blob/<64 hex>` by Block2 ≤ 256 B on port 5683. The ztoit design is edited by a ztoit session.

- [ ] **Step 3: Promotion (CLAUDE.md promotion rule).** The durable knowledge is in `protocol/PROTOCOL-NSL.md` (§0, §5.3, §6, §7 q7, §8) and `porta/docs/ARCHITECTURE.md` (§1, §3.3, §4), done in Task 7, with `porta/docs/CLI.md` (Task 4). The design doc stays as the why. #505 closes when the §8 proofs above are shown — by David's word or the PR merge, never by a closing keyword in a commit. rete#506–#508 stay open.

**Note for `/conclude` (not a task):** FOCUS.md is rewritten there — #505's state (branch `505-porta-coap`, PR or not), that gw's production porta needs a rebuild David starts before any ztoit node can reach it (and that it will then bind `:5683`), and the ztoit#10 hand-off.

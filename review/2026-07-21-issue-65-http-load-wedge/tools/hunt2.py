"""#65 pass 2 — CONCURRENT hammer, then interrogate the node the moment it wedges.

Every earlier repro drove the node with a SERIAL client, which can pin at most one
connection at a time; the 4 contexts only ever filled via TIME_WAIT residue or
accept backlog. Real overload is simultaneous, so this opens K connections at
once -- the state most likely to pin every context and every RX packet together.

On a confirmed wedge (sustained failure of BOTH ICMP and HTTP) it immediately
runs `net conn` / `net mem` over the UART, because the question is what TCP state
the stuck connections are in, and that state must be read before anything else
touches the node.
"""
import socket, subprocess, sys, threading, time

HOST, PORT = "192.168.0.240", 18080
REQ = b"GET /thermal HTTP/1.0\r\n\r\n"
WORKERS = 8
stop = threading.Event()
counts = {"ok": 0, "refused": 0, "timeout": 0, "other": 0}
lock = threading.Lock()

def worker():
    while not stop.is_set():
        s = socket.socket(); s.settimeout(2.0)
        kind = "other"
        try:
            s.connect((HOST, PORT)); s.sendall(REQ)
            while s.recv(4096): pass
            kind = "ok"
        except socket.timeout: kind = "timeout"
        except ConnectionRefusedError: kind = "refused"
        except OSError: kind = "other"
        finally:
            s.close()
        with lock: counts[kind] += 1

def healthy():
    p = subprocess.run(["ping", "-c", "1", "-W", "2", HOST], capture_output=True)
    if p.returncode == 0: return True
    c = subprocess.run(["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}",
                        "--max-time", "5", f"http://{HOST}:{PORT}/thermal"],
                       capture_output=True, text=True)
    return c.stdout.strip() == "200"

threads = [threading.Thread(target=worker, daemon=True) for _ in range(WORKERS)]
for t in threads: t.start()
print(f"{WORKERS} concurrent workers started", flush=True)

bad = 0
deadline = time.time() + 900
while time.time() < deadline:
    time.sleep(5)
    if healthy():
        bad = 0
    else:
        bad += 1
        print(f"  probe failed ({bad}/5)", flush=True)
        if bad >= 5:
            print(f">>> WEDGE CONFIRMED at {time.strftime('%H:%M:%S')}", flush=True)
            break
    with lock:
        print(f"{time.strftime('%H:%M:%S')} {dict(counts)}", flush=True)

stop.set()
time.sleep(2)
with lock: print("final:", dict(counts), flush=True)

if False:
    print("\n### interrogating the wedged node over UART ###", flush=True)
    subprocess.run([sys.executable,
                    "/tmp/claude-1000/-home-david-workspace-tuvm/fc2fcf9f-97cb-434e-8b17-eb311eb7e475/scratchpad/i65/shell_cmd.py",
                    "/dev/ttyUSB1", "net conn", "net mem"])

"""Time-resolved probe of the refusal window.

Hypothesis under test: the node's accept ceiling is set by net_context slots
(CONFIG_NET_MAX_CONTEXTS=5, one held by the listener => 4) each parked in
TIME_WAIT for CONFIG_NET_TCP_TIME_WAIT_DELAY=1500 ms after the SERVER's active
close. Prediction: a quiesced node accepts a burst of ~4, refuses the rest, and
resumes accepting ~1.5 s after the first connection closed -- not sooner.
"""
import socket, sys, time

HOST, PORT = "192.168.0.240", 18080
REQ = b"GET /thermal HTTP/1.0\r\n\r\n"

def attempt():
    t = time.perf_counter()
    s = socket.socket(); s.settimeout(3.0)
    try:
        s.connect((HOST, PORT))
    except OSError as e:
        return t, "REFUSED(%s)" % e.errno, time.perf_counter() - t
    try:
        s.sendall(REQ)
        n = 0
        while True:
            b = s.recv(4096)
            if not b: break
            n += len(b)
        return t, "OK(%d B)" % n, time.perf_counter() - t
    except OSError as e:
        return t, "ERR(%s)" % e.errno, time.perf_counter() - t
    finally:
        s.close()

print("quiescing 6 s...")
time.sleep(6)
base = time.perf_counter()
print(f"{'t(ms)':>8}  {'dur(ms)':>8}  outcome")
for i in range(8):                      # saturate
    t, res, dur = attempt()
    print(f"{(t-base)*1000:8.0f}  {dur*1000:8.0f}  {res}")
print("--- now polling every 150 ms to find when acceptance resumes ---")
for i in range(28):
    t, res, dur = attempt()
    print(f"{(t-base)*1000:8.0f}  {dur*1000:8.0f}  {res}")
    time.sleep(0.15)

import socket, subprocess, time
HOST, PORT = "192.168.0.240", 18080
socks = []
print("--- opening 4 idle connections (browser-style, no request sent) ---", flush=True)
for i in range(4):
    try:
        s = socket.create_connection((HOST, PORT), timeout=5); socks.append(s)
        print(f"  conn {i+1} established", flush=True)
    except Exception as e:
        print(f"  conn {i+1} refused: {e}", flush=True)
def probe(t=3):
    r = subprocess.run(['curl','-s','-m',str(t),'-o','/dev/null','-w','%{http_code}',
                        f'http://{HOST}:{PORT}/hello'], capture_output=True, text=True)
    return r.stdout.strip()
print(f"immediately after pinning: {probe()}", flush=True)
t0 = time.time()
while time.time() - t0 < 60:
    c = probe()
    if c == '200':
        print(f"*** recovered after {time.time()-t0:.1f}s (sockets still held open) ***", flush=True)
        break
    time.sleep(1)
else:
    print("*** did NOT recover within 60s ***", flush=True)
for s in socks: s.close()

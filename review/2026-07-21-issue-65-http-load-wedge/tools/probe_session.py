"""#65 pass 3 — ONE serial handle for the whole experiment.

Two mistakes this design exists to prevent, both made earlier tonight:

 1. OPENING THE PORT RESETS THE BOARD. Verified: two successive attaches both
    reported uptime restarting at ~5 s. So a tool that attaches at wedge time to
    ask what happened REBOOTS THE NODE AND READS A FRESH BOOT -- which is exactly
    what produced an "empty pools, no TCP connections" reading that meant nothing.
    Here the port is opened ONCE, before any load, and the same handle is used to
    capture the console AND to type shell commands. The only reset is at t=0.

 2. A DETECTOR THAT PROBES WHILE STILL HAMMERING CANNOT TELL SATURATION FROM A
    WEDGE. Under sustained concurrent load the node genuinely cannot answer, so
    "5 failed probes" is guaranteed and means nothing. Load is QUIESCED here, and
    only then is health judged: a node that recovers within the quiet window was
    saturated; one that does not is wedged.
"""
import socket, subprocess, sys, threading, time, serial

HOST, PORT, DEV = "192.168.0.240", 18080, "/dev/ttyUSB1"
REQ = b"GET /thermal HTTP/1.0\r\n\r\n"
WORKERS = 8
log = open(sys.argv[1] if len(sys.argv) > 1 else "session.log", "w")

p = serial.Serial(); p.port = DEV; p.baudrate = 115200; p.timeout = 0.2
p.dtr = False; p.rts = True; p.open(); time.sleep(0.2); p.rts = False  # deliberate boot
reading = threading.Event(); reading.set()

def reader():
    while reading.is_set():
        try: d = p.read(4096)
        except Exception: break
        if d: log.write(d.decode("utf-8", "replace")); log.flush()
threading.Thread(target=reader, daemon=True).start()

def shell(cmd, wait=3.5):
    log.write(f"\n##### SHELL: {cmd} #####\n"); log.flush()
    p.write((cmd + "\r\n").encode()); p.flush(); time.sleep(wait)

def healthy():
    if subprocess.run(["ping","-c","1","-W","2",HOST],capture_output=True).returncode == 0:
        return True
    c = subprocess.run(["curl","-s","-o","/dev/null","-w","%{http_code}","--max-time","5",
                        f"http://{HOST}:{PORT}/thermal"],capture_output=True,text=True)
    return c.stdout.strip() == "200"

print("waiting for boot + wifi...", flush=True)
for _ in range(60):
    time.sleep(2)
    if healthy(): break
print("node up; starting load", flush=True)

hammer = threading.Event(); hammer.set()
stop = threading.Event()
counts = {"ok":0,"refused":0,"timeout":0,"other":0}; lock = threading.Lock()

def worker():
    while not stop.is_set():
        if not hammer.is_set(): time.sleep(0.2); continue
        s = socket.socket(); s.settimeout(2.0); kind="other"
        try:
            s.connect((HOST,PORT)); s.sendall(REQ)
            while s.recv(4096): pass
            kind="ok"
        except socket.timeout: kind="timeout"
        except ConnectionRefusedError: kind="refused"
        except OSError: kind="other"
        finally: s.close()
        with lock: counts[kind]+=1
for _ in range(WORKERS): threading.Thread(target=worker,daemon=True).start()

wedged = False
for cycle in range(40):
    time.sleep(15)                       # hammer
    hammer.clear()                       # QUIESCE before judging
    time.sleep(12)
    ok = [healthy() for _ in range(3)]
    with lock: c = dict(counts)
    print(f"{time.strftime('%H:%M:%S')} cycle {cycle}: {c} quiesced-health={ok}", flush=True)
    if not any(ok):
        time.sleep(20)                   # one more chance to self-recover
        if not any(healthy() for _ in range(3)):
            print(">>> TRUE WEDGE (failed after quiescing)", flush=True); wedged=True; break
    hammer.set()

stop.set(); time.sleep(1)
if wedged:
    print("interrogating on the SAME handle (no reset)", flush=True)
    for c in ["net conn", "net mem", "net stats"]: shell(c)
else:
    print("no wedge; capturing a healthy reference", flush=True)
    for c in ["net conn", "net mem"]: shell(c)
time.sleep(1); reading.clear(); time.sleep(0.5); p.close(); log.close()
print("done", flush=True)

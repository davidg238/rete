#!/usr/bin/env python3
"""rete#311 — the in-image debugger for a person: source lines in, source lines out.

The VM's debugger speaks `dbg:*` lines (nodus/nsl/lib/supervisor.nsl) and knows methods
only by a registry id and a bytecode offset: `dbg:break 2 9`, `dbg:paused break 2 9`.
This tool sits between a person and that line and does the two translations, using the
image map the compiler writes beside every image (`<name>.oimg.map.json`,
protocol/IMAGE-ARTIFACTS.md) and the registry the VM lists (`dbg:methods`):

    break 26            ->  dbg:break 2 2        (line 26 is in countTo:, offset 2)
    dbg:paused break 2 2  ->  stopped (break) at 46_debug_target.nsl:26  countTo:
                              26 |     1 to: n do: [ :i | i println. sum:: sum + i ].

The same tool drives a board over its console cable (--port, one handle, a collector
stopped first) or a host VM it starts itself (everything after `--`):

    dbg.py --map /tmp/46_debug_target.oimg.map.json -- /tmp/oevm_img /tmp/46_debug_target.oimg
    dbg.py --map DebugProbeC6.oimg.map.json --port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_...

Commands (type `help`): break/clear <line> or <file>:<line>, continue, step, next,
finish, pause, inspect, where, list, methods, quit; any `dbg:...` line is passed through
unchanged. Any line typed while the program runs stops it first (the pause rule), so
`break 26` while running stops the program, sets the breakpoint, and stays stopped.

What a line can be: the registry is the ENTRY class's methods (then the kernel's
Object), so a breakpoint can go on a line inside a method of the entry class -- not in
another class of the file, not in a block the compiler kept as a block, and not in the
kernel. A line with no code of its own (a comment, a blank) inside a method moves forward
to the next line that has some, and says so; outside every method (a header, `| locals |`)
it is refused with the nearest lines that have code. docs/tutorials/04-debugger.md is
the page for a person.
"""
import argparse, hashlib, json, os, re, subprocess, sys, threading, time

PAUSED_RE = re.compile(r"^dbg:paused (\w+) (-?\d+) (\d+)$")
REG_RE = re.compile(r"^(\d+) #?(\S+) (-?\d+)$")


class SourceMap:
    """The image map (offset -> name, file, lines per method) joined to the VM's
    registry (id -> selector). Pure: no I/O beyond reading the files it is given."""

    def __init__(self, map_path, source_path=None):
        with open(map_path, encoding="utf-8") as f:
            doc = json.load(f)
        missing = [k for k in ("image", "entry", "files", "methods") if k not in doc]
        if missing:
            raise ValueError("%s is not an image map (no %s); rebuild it (rete#337)"
                             % (map_path, ", ".join(missing)))
        self.image = doc["image"]
        self.source = doc["files"][0]
        self.entry = doc["entry"]
        # every method with lines in THIS file (files[0]): the entry's, its blocks',
        # its other classes'. The kernel and the Supervisor are other files.
        self.functions = {m["name"]: m["lines"] for m in doc["methods"].values()
                          if m["file"] == 0 and m["lines"]}
        prefix = self.entry + ">>"
        # selector -> [(offset, line), ...] for the entry class's own methods
        self.methods = {k[len(prefix):]: [tuple(p) for p in v]
                        for k, v in self.functions.items() if k.startswith(prefix)}
        path = source_path or os.path.join(os.path.dirname(os.path.abspath(map_path)),
                                           self.source)
        try:
            with open(path, encoding="utf-8") as f:
                self.text = f.read().splitlines()
        except OSError:
            self.text = None
        self.ids = {}       # selector -> id   (the entry class's group only)
        self.sels = {}      # id -> (selector, is_entry)

    def check_image(self, oimg_path):
        """Refuse a map that belongs to another build: its hash names one image."""
        with open(oimg_path, "rb") as f:
            got = hashlib.sha256(f.read()).hexdigest()
        if got != self.image["sha256"]:
            raise ValueError("%s is not the image this map describes (sha256 %s, map says "
                             "%s); rebuild both together" % (oimg_path, got[:12],
                                                             self.image["sha256"][:12]))

    def load_registry(self, lines):
        """`<id> #<selector> <tag>` lines from `dbg:methods`. The first tag is the
        entry class's vtable; later tags are its delegates (the kernel's Object)."""
        first = None
        for l in lines:
            m = REG_RE.match(l)
            if not m:
                continue
            i, sel, tag = int(m.group(1)), m.group(2), m.group(3)
            first = first or tag
            self.sels[i] = (sel, tag == first)
            if tag == first:
                self.ids[sel] = i

    def line_text(self, line):
        if self.text and 1 <= line <= len(self.text):
            return "%4d | %s" % (line, self.text[line - 1])
        return None

    def method_at_line(self, line):
        """The narrowest method of this file whose lines span `line`, as a map name,
        or None. Narrowest, because a block that is not inlined (a `whenever:do:`
        handler) is its own method, `<Entry>_blockN>>value:`, inside its home's span."""
        best = None
        for k, pairs in self.functions.items():
            lo, hi = min(l for _, l in pairs), max(l for _, l in pairs)
            if lo <= line <= hi and (best is None or hi - lo < best[1]):
                best = (k, hi - lo)
        return best and best[0]

    def resolve(self, spec):
        """`26` or `file.nsl:26` -> (id, offset, line, selector). Raises ValueError
        with a sentence a person can act on."""
        f, _, n = spec.rpartition(":")
        if f and os.path.basename(f) != self.source:
            raise ValueError("this image was built from %s, not %s" % (self.source, f))
        try:
            line = int(n)
        except ValueError:
            raise ValueError("expected <line> or %s:<line>, got %r" % (self.source, spec))
        key = self.method_at_line(line)
        if key is None:
            # A header, a `| locals |` line, a comment: the map starts a method at its
            # first statement, so name the code on either side instead of guessing.
            near = sorted((l, k.split(">>")[1]) for k, v in self.functions.items() for _, l in v)
            before = [p for p in near if p[0] < line][-1:]
            after = [p for p in near if p[0] > line][:1]
            raise ValueError("line %d has no code; the nearest lines that do: %s" % (
                line, ", ".join("%d (%s)" % p for p in before + after) or "none"))
        cls, sel = key.split(">>")
        if cls != self.entry:
            raise ValueError("line %d is in %s, a block or class the registry does not list; "
                             "break on a line of a %s method instead" % (line, key, self.entry))
        if sel not in self.ids:
            raise ValueError("%s is not in the VM's registry (run `methods`)" % sel)
        pairs = sorted(self.methods[sel], key=lambda p: (p[1], p[0]))
        off, at = next((o, l) for o, l in pairs if l >= line)
        return self.ids[sel], off, at, sel

    def where(self, mid, off):
        """(id, offset) from a `dbg:paused` line -> a sentence and the source line."""
        if mid < 0 or mid not in self.sels:
            return "outside the registry (in a block or the kernel), offset %d" % off, None
        sel, own = self.sels[mid]
        if not own or sel not in self.methods:
            return "in the kernel's %s, offset %d" % (sel, off), None
        pairs = [p for p in self.methods[sel] if p[0] <= off]
        line = max(pairs)[1] if pairs else min(self.methods[sel])[1]
        return "at %s:%d  %s" % (self.source, line, sel), self.line_text(line)


class Line:
    """One text line each way, from a board's cable or a host VM's stdio. A reader
    thread hands every whole line to `on_line` as it arrives (None when a host VM
    exits), so output reaches the person while they sit at the prompt and nothing
    waits on the next read."""

    def __init__(self, on_line, port=None, baud=115200, argv=None):
        if port:
            import serial
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = port, baud, 0.2
            s.dtr = s.rts = False      # see dbg_session.py: opens without a reset pulse
            s.open()
            self._write = lambda b: (s.write(b), s.flush())
            read = lambda: s.read(256)
        else:
            env = dict(os.environ, OEVM_DEBUG="1")
            self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, env=env, bufsize=0)
            self._write = lambda b: (self.proc.stdin.write(b), self.proc.stdin.flush())
            read = lambda: os.read(self.proc.stdout.fileno(), 256) or None
        threading.Thread(target=self._pump, args=(read, on_line), daemon=True).start()

    def _pump(self, read, on_line):
        buf = b""
        while True:
            chunk = read()
            if chunk is None:
                on_line(None)
                return
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                on_line(raw.decode("utf-8", "replace").rstrip("\r"))

    def send(self, line):
        self._write((line + "\n").encode())


# A line of source is many bytecodes, and the VM steps one at a time; `step` and `next`
# repeat until the line changes. A loop on one line (`1 to: n do: [ ... ]`) is many
# round trips, and a `step` into the kernel can be thousands, so the repeat stops here
# and says where it is rather than running on unseen.
LINE_STEP_LIMIT = 5000


class Session:
    WORDS = {"c": "dbg:continue", "continue": "dbg:continue",
             "s": "dbg:step", "step": "dbg:step",
             "n": "dbg:over", "next": "dbg:over",
             "f": "dbg:out", "finish": "dbg:out",
             "p": "dbg:pause", "pause": "dbg:pause",
             "i": "dbg:inspect", "inspect": "dbg:inspect"}

    def __init__(self, sm, out=sys.stdout):
        self.sm, self.out = sm, out
        self.line = None
        self.stopped = None        # (id, off) of the last stop, (None, None) at boot, None running
        self.gone = False
        self.seen = []             # every line from the VM, in order
        self.cv = threading.Condition()
        self.quiet = False         # fetching the registry: its lines are not the person's
        self.stepping = False      # a line step: only its LAST bytecode stop is shown

    def attach(self, line):
        self.line = line

    def say(self, s):
        self.out.write(s + "\n")
        self.out.flush()

    def on_line(self, l):
        """The reader thread's: record, then show, translating a stop into source terms."""
        m = l and PAUSED_RE.match(l)
        if l is None:
            self.gone = True
            self.say("-- the program ended")
        elif l == "dbg:ready":
            self.stopped = (None, None)
            self.say("-- stopped at boot, before the program's first line")
        elif m:
            mid, off = int(m.group(2)), int(m.group(3))
            self.stopped = (mid, off)
            if not (self.stepping and m.group(1) == "step"):
                where, text = self.sm.where(mid, off)
                self.say("-- stopped (%s) %s" % (m.group(1), where))
                if text:
                    self.say(text)
        elif not self.quiet:
            self.say(l)
        with self.cv:
            self.seen.append(l)
            self.cv.notify_all()

    def ask(self, line, pattern, timeout):
        """Send `line`, then wait for a VM line matching `pattern` (or the VM's end);
        answer the lines that arrived since the send. Marking BEFORE the send is what
        lets an answer that beats us to the wait still count."""
        with self.cv:
            mark = len(self.seen)
        if line is not None:
            self.line.send(line)
        return self.wait(pattern, timeout, mark)

    def wait(self, pattern, timeout, mark=None):
        rx = re.compile(pattern)
        with self.cv:
            i = len(self.seen) if mark is None else mark
            end = time.time() + timeout
            while True:
                while i < len(self.seen):
                    l = self.seen[i]
                    i += 1
                    if l is None or rx.search(l):
                        return self.seen[mark or 0:i] if mark is not None else []
                left = end - time.time()
                if left <= 0:
                    return self.seen[mark:] if mark is not None else []
                self.cv.wait(left)

    def registry(self):
        if not self.sm.ids:
            self.quiet = True
            try:
                self.sm.load_registry(self.ask("dbg:methods", r"^dbg:ok methods$|^dbg:error", 10))
            finally:
                self.quiet = False

    def stop_first(self):
        """A line typed while running stops the program; wait for that stop so the
        registry and the answer that follow are read in order."""
        if self.stopped is None:
            self.ask("dbg:pause", r"^dbg:ok pause$", 10)

    def do(self, cmd):
        word, _, arg = cmd.strip().partition(" ")
        arg = arg.strip()
        if not word:
            return True
        if word in ("q", "quit"):
            return False
        if word in ("h", "help"):
            self.say(__doc__.split("Commands", 1)[1].split("\n\n", 1)[0].strip())
            return True
        if word.startswith("dbg:"):
            if word in ("dbg:continue", "dbg:run"):
                self.stopped = None
                self.line.send(cmd.strip())
            else:
                self.ask(cmd.strip(), r"^dbg:(ok|error|stack|paused)", 10)
            return True
        if word in ("b", "break", "clear"):
            self.stop_first()
            self.registry()
            try:
                mid, off, at, sel = self.sm.resolve(arg)
            except ValueError as e:
                self.say("-- " + str(e))
                return True
            verb = "dbg:clear" if word == "clear" else "dbg:break"
            moved = "" if str(at) == arg.rpartition(":")[2] else " (line %s has no code; next is %d)" % (arg, at)
            self.say("-- %s %s:%d  %s%s" % (verb[4:], self.sm.source, at, sel, moved))
            self.ask("%s %d %d" % (verb, mid, off), r"^dbg:(ok|error)", 10)
            return True
        if word in ("w", "where"):
            if self.stopped is None:
                self.say("-- running")
            elif self.stopped == (None, None):
                self.say("-- at boot")
            else:
                self.registry()
                where, text = self.sm.where(*self.stopped)
                self.say("-- " + where + ("\n" + text if text else ""))
            return True
        if word in ("l", "list"):
            if not self.stopped or self.stopped[0] is None:
                self.say("-- not stopped in a line")
                return True
            self.registry()
            where, _ = self.sm.where(*self.stopped)
            m = re.search(r":(\d+) ", where)   # "at <file>:<line>  <selector>"
            if m:
                at = int(m.group(1))
                for n in range(max(1, at - 3), at + 4):
                    t = self.sm.line_text(n)
                    if t:
                        self.say(("=> " if n == at else "   ") + t)
            return True
        if word == "methods":
            self.stop_first()
            self.registry()
            for sel, mid in sorted(self.sm.ids.items(), key=lambda kv: kv[1]):
                pairs = self.sm.methods.get(sel)
                span = "%s:%d-%d" % (self.sm.source, min(l for _, l in pairs), max(l for _, l in pairs)) if pairs else "(no source lines)"
                self.say("%4d  %-24s %s" % (mid, sel, span))
            return True
        verb = self.WORDS.get(word)
        if verb is None:
            self.say("-- unknown: %s (type help)" % word)
            return True
        if verb == "dbg:pause":
            if self.stopped is not None:
                self.say("-- already stopped")
            else:
                self.stop_first()
            return True
        self.stop_first()
        if verb == "dbg:continue":
            self.stopped = None
            self.line.send(verb)
        elif verb == "dbg:inspect":
            self.ask(verb, r"^dbg:(stack|error)", 10)
        elif verb == "dbg:out":
            self.stopped = None
            self.ask(verb, r"^dbg:paused |^dbg:error", 30)
        else:
            self.step_line(verb)
        return True

    def here(self):
        """(id, line) of the current stop, line None outside the app's methods."""
        if not self.stopped or self.stopped[0] is None:
            return None
        self.registry()
        where, _ = self.sm.where(*self.stopped)
        m = re.search(r":(\d+) ", where)
        return self.stopped[0], m and int(m.group(1))

    def step_line(self, verb):
        """Repeat a bytecode step until the stop is on a different line of the app (a
        line inside a block or the kernel does not count for `step`, and `next` never
        goes there), a breakpoint or the end intervenes, or LINE_STEP_LIMIT runs out.
        Only the final stop is shown."""
        start = self.here()
        self.stepping = True
        try:
            for n in range(LINE_STEP_LIMIT):
                self.stopped = None
                seen = self.ask(verb, r"^dbg:paused |^dbg:error", 30)
                last = seen[-1] if seen else None
                m = last and PAUSED_RE.match(last)
                if not m or m.group(1) != "step" or self.gone:
                    return                        # a breakpoint, an error, the end
                now = self.here()
                if now and now[1] is not None and now != start:
                    break
            else:
                self.stepping = False
                self.say("-- still %s after %d steps; `step` again or `continue`"
                         % (self.sm.where(*self.stopped)[0], LINE_STEP_LIMIT))
                return
        finally:
            self.stepping = False
        where, text = self.sm.where(*self.stopped)
        self.say("-- stopped (step) " + where)
        if text:
            self.say(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", required=True, help="the .oimg.map.json written beside the image")
    ap.add_argument("--source", help="the .nsl it came from (default: beside the map)")
    ap.add_argument("--port", help="a board's console cable (else run the command after --)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("argv", nargs="*", help="host VM command line, after --")
    args = ap.parse_args()
    if bool(args.port) == bool(args.argv):
        ap.error("give exactly one of --port or a host VM command after --")

    try:
        sm = SourceMap(args.map, args.source)
        for a in args.argv:
            if a.endswith(".oimg"):
                sm.check_image(a)
    except ValueError as e:
        ap.error(str(e))
    s = Session(sm)
    s.attach(Line(s.on_line, port=args.port, baud=args.baud, argv=args.argv or None))
    # A board may already be waiting (its dbg:ready printed before we opened the port)
    # or running; either way the first command finds out. A host VM prints dbg:ready.
    s.wait(r"^dbg:ready$", 5 if args.port else 10, mark=0)
    interactive = sys.stdin.isatty()
    while not s.gone:
        try:
            cmd = input("(dbg) " if interactive else "")
        except EOFError:
            # Out of commands: watch the program to its next stop or its end, so a
            # piped script sees what its last `continue` led to.
            if s.stopped is None:
                s.wait(r"^dbg:paused |^dbg:ready$", 10)
            break
        if not interactive:
            s.say("(dbg) " + cmd)
        if not s.do(cmd):
            break
        if s.stopped is None:
            s.wait(r"^dbg:paused |^dbg:ready$", 0.1)   # let a prompt stop print first


if __name__ == "__main__":
    main()

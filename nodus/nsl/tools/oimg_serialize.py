"""oevm execute-in-place image (.oimg) serializer.

Produces a native-layout, clustered blob from the solidify object graph.
Reference encoding (low 2 bits = tag):
  00 app-object  payload = byte offset into blob (resolved as blob_base + off)
  01 builtin     payload = index into firmware builtin_registry[]
  10 small SMI   payload = sign-extended immediate value
  11 singleton   payload = id (0=nil, 1=true, 2=false)
"""
import struct

from prim_ids import PRIM_IDS   # rete#226: oevm/src/prim_names.def, the single source

TAG_APP, TAG_BUILTIN, TAG_SMI, TAG_SINGLETON = 0b00, 0b01, 0b10, 0b11


class RefWord:
    """Width-agnostic ref-word codec. SMI words may be negative Python ints;
    OimgWriter.word() masks them to two's-complement at the target width."""

    @staticmethod
    def app_offset(off):
        if off & 0b11:
            raise ValueError(f"app offset {off} not 4-aligned")
        return (off << 2) | TAG_APP

    @staticmethod
    def builtin(idx):
        return (idx << 2) | TAG_BUILTIN

    @staticmethod
    def smi(v):
        return (v << 2) | TAG_SMI

    @staticmethod
    def singleton(i):
        return (i << 2) | TAG_SINGLETON

    @staticmethod
    def decode(word):
        # word >> 2 is an arithmetic shift in Python, so SMI payloads keep sign.
        return (word & 0b11, word >> 2)


class OimgWriter:
    MAGIC = b"OIMG"

    def __init__(self, oop_size, version, registry_version):
        self.oop_size = oop_size
        self.version = version
        self.registry_version = registry_version

    def word(self, v):
        """Serialize a (possibly negative) ref-word as two's-complement at oop width."""
        mask = (1 << (self.oop_size * 8)) - 1
        return (v & mask).to_bytes(self.oop_size, "little")

    def u16(self, v):
        return struct.pack("<H", v)

    def u32(self, v):
        return struct.pack("<I", v)

    def header_bytes(self, blob_size, cluster_count, root_ref,
                     init_sel_ref=0, main_sel_ref=0, init_argc=0, entry_nslots=0,
                     kernel_vtable_ref=0, kernel_init_sel_ref=0, kernel_nslots=0,
                     serial_on_sel_ref=0,
                     supervisor_vtable_ref=0, supervisor_init_sel_ref=0,
                     supervisor_run_sel_ref=0, supervisor_nslots=0,
                     net_on_sel_ref=0):
        # ---- .oimg header byte layout (CONTRACT — mirror in oevm/src/oimg_loader.c) ----
        # All multi-byte scalars little-endian. W = oop_size (8 on 64-bit, 4 on 32-bit).
        # Ref-words (root/init/main/kernel_vtable/kernel_init_sel) are W-wide, W-aligned.
        #
        # format_version 8 (Milestone A Task 9b compaction): ONE fixed layout.
        # When the Kernel descriptor is present the serial/supervisor/net groups
        # are ALWAYS emitted (0 when the entry takes no such capability), so the
        # header always ends at the net word (A+12W, padded). The v7 tail word
        # (the retired concurrency capability ref at A+13W) is gone — fibers were
        # removed in Milestone A; v3-v7 headers are no longer written or loaded.
        #
        # format_version 9 (rete#190 stage 2): the header layout is UNCHANGED;
        # what changed is the Symbol record — its name bytes are now INLINE
        # (no separate strbytes allocation, no `name` ref-word). Bumped so a v8
        # image meets a v9 VM as a loud rejection rather than a silent misread.
        #
        #   off  width  field
        #   0    4      magic "OIMG"
        #   4    2      format_version (u16: 9 when Kernel descriptor present;
        #                1 for a kernel-less minimal blob — header ends at the
        #                entry descriptor; serializer unit tests only)
        #   6    2      flags (u16, =0)
        #   8    4      registry_version (u32)
        #   12   4      blob_size (u32, = total file size)
        #   16   4      cluster_count (u32)
        #   --- pad to W alignment (4 bytes pad when W==8: 20->24) ---
        #   A    W      root_ref            (entry vtable ref-word)
        #   A+W  W      init_sel_ref        (init-selector symbol ref-word, app-offset)
        #   A+2W W      main_sel_ref        ("main:args:" symbol ref-word, app-offset)
        #   A+3W 2      init_argc (u16)
        #   A+3W+2 2    entry_nslots (u16)
        #   --- pad whole header to 8 (ALIGN) so object region stays 8-aligned ---
        #   A+4W W      kernel_vtable_ref   (Kernel module vtable)
        #   A+5W W      kernel_init_sel_ref (Kernel init selector sym)
        #   A+6W 2      kernel_nslots (u16) (Kernel instance slots)
        #   --- pad whole header to ALIGN again ---
        #   A+7W W      serial_on_sel_ref   (ref to Kernel>>serialOn:; 0 when the
        #                                    entry takes no serial capability)
        #   --- pad whole header to ALIGN again ---
        #   A+8W  W     supervisor_vtable_ref   (Supervisor vtable)
        #   A+9W  W     supervisor_init_sel_ref (init_mirror: selector sym)
        #   A+10W W     supervisor_run_sel_ref  (run:platform:args:debug:)
        #   A+11W 2     supervisor_nslots (u16) (Supervisor instance slots)
        #   --- pad whole header to ALIGN again ---
        #   A+12W W     net_on_sel_ref      (ref to Kernel>>netOn:; 0 when the
        #                                    entry takes no net capability)
        #   --- pad whole header to ALIGN again ---
        # For W==8: root_ref@24 init_sel_ref@32 main_sel_ref@40 init_argc@48
        #           entry_nslots@50, kernel_vtable_ref@56, kernel_init_sel_ref@64,
        #           kernel_nslots@72, header padded to 80, serial_on_sel_ref@80,
        #           header padded to 88, supervisor_vtable_ref@88, supervisor_init_sel_ref@96,
        #           supervisor_run_sel_ref@104, supervisor_nslots@112, header padded to 120,
        #           net_on_sel_ref@120, header padded to 128.
        # (format_version 1: Kernel descriptor absent, header 56 bytes.)
        #
        # RESERVED — G4 line-table provision (https://github.com/davidg238/nsl-tuvm/issues/3 rider, decided 2026-07-04;
        # see docs/design/2026-07-04-G3-gc-strategy-decision.md). A future
        # format bump may append a `line_table_ref` ref-word at the header tail
        # (after net_on_sel_ref, same pad-to-ALIGN discipline) and/or a "LINETABLE"
        # kind at the END of KIND_ORDER. Both are structurally additive: the loader
        # reaches blob content only via ref-words resolved from the root graph, so
        # records no header/method ref points at are ignored bytes. Do NOT insert
        # new header fields before the tail or reorder KIND_ORDER — offsets and
        # cluster grouping are position-dependent. Compiler-side emission of line
        # tables (CompileError line numbers) is #139; this note only keeps
        # the format door open, per G4: "retrofitting line tables into an image
        # format is painful — decide the format early".
        if not kernel_vtable_ref and (serial_on_sel_ref or supervisor_vtable_ref
                                      or net_on_sel_ref):
            raise ValueError("v9 header: serial/supervisor/net descriptors "
                             "require the Kernel descriptor")
        version = 9 if kernel_vtable_ref else self.version
        h = self.MAGIC
        h += self.u16(version) + self.u16(0)               # flags = 0
        h += self.u32(self.registry_version)
        h += self.u32(blob_size) + self.u32(cluster_count)
        # word-align the ref-word block to its natural boundary
        if len(h) % self.oop_size:
            h += b"\x00" * (self.oop_size - (len(h) % self.oop_size))
        h += self.word(root_ref)
        h += self.word(init_sel_ref)
        h += self.word(main_sel_ref)
        h += self.u16(init_argc) + self.u16(entry_nslots)
        # CONTRACT: descriptor ref-words form a DENSE word-strided block. The loader
        # (oevm/src/oimg_loader.c) reads every descriptor at `a + n*W` (W = oop_size,
        # a = ALIGN_UP(20, W)) with NO inter-group padding, so the serializer must
        # pad each group to W (the ref-word width), NOT to ALIGN. On W==8 these
        # coincide (W==ALIGN); on W==4 they DIVERGE — padding to ALIGN here shifts
        # every kernel/serial/supervisor ref past where the loader reads it, yielding
        # a NULL kernel vtable → DNU at boot. Pad to W. The whole header is separately
        # rounded up to ALIGN (=8) by generate()'s `_align(header_len)` so the object
        # region stays double-aligned; that rounding is NOT this block's job.
        W = self.oop_size
        # pad to W before optional Kernel descriptor
        if len(h) % W:
            h += b"\x00" * (W - (len(h) % W))
        # v8: the Kernel descriptor gates the WHOLE descriptor block. When it is
        # present, every group below is emitted (0-filled when unused) so the
        # header has exactly one shape per oop width:
        #   kernel_vtable_ref @a+4W, kernel_init_sel_ref @a+5W, kernel_nslots (u16) @a+6W,
        #   serial_on_sel_ref @a+7W,
        #   supervisor_vtable_ref @a+8W, init_sel @a+9W, run_sel @a+10W, nslots (u16) @a+11W,
        #   net_on_sel_ref @a+12W (tail).
        if kernel_vtable_ref:
            h += self.word(kernel_vtable_ref)
            h += self.word(kernel_init_sel_ref)
            h += self.u16(kernel_nslots)
            if len(h) % W:
                h += b"\x00" * (W - (len(h) % W))
            h += self.word(serial_on_sel_ref)
            if len(h) % W:
                h += b"\x00" * (W - (len(h) % W))
            h += self.word(supervisor_vtable_ref)
            h += self.word(supervisor_init_sel_ref)
            h += self.word(supervisor_run_sel_ref)
            h += self.u16(supervisor_nslots)
            if len(h) % W:
                h += b"\x00" * (W - (len(h) % W))
            h += self.word(net_on_sel_ref)
            if len(h) % W:
                h += b"\x00" * (W - (len(h) % W))
        return h


# Object kinds, in cluster emit order. SYMBOL is in NEITHER dict (rete#190
# stage 2): Symbols are variable-size now (header + inline NUL-terminated
# bytes), so they are reserved individually and emitted by name, exactly like
# String — which has never appeared in either dict for the same reason. These
# two dicts describe only the fixed-size-per-kind cluster loop; _put resolves
# field offsets from the literal struct name it is handed, not through
# KIND_STRUCT.
KIND_ORDER = ["VTABLE", "METHOD", "CLOSURE", "FLOAT", "LARGEINT", "INSTANCE", "BOXEDBINDING"]
KIND_STRUCT = {
    "VTABLE": "Vtable", "METHOD": "CompiledMethod",
    "CLOSURE": "Closure", "FLOAT": "Float", "LARGEINT": "LargeInteger",
    "INSTANCE": "ObjectInstance", "BOXEDBINDING": "BoxedBinding",
}
# C int / uint32_t fields (4 bytes on every target ABI we support: x86-32/64, ESP32).
WIDTH4 = {"is_nested", "num_registers", "num_arguments", "access_modifier", "home_generation"}
# Fixed 64-bit fields, independent of oop width (LargeInteger sign/magnitude).
WIDTH8_SIGNED = {"sign"}
WIDTH8_UNSIGNED = {"mag"}
ALIGN = 8  # max struct alignment (covers double + 64-bit pointer); app offsets stay 4-aligned.


def _align(n, a=ALIGN):
    return (n + a - 1) & ~(a - 1)


class OimgSerializer:
    """Serialize a solidify object graph into a native-layout execute-in-place blob.

    Design decisions (resolved during implementation):
      * NULL pointer/ref fields are stored as ref-word 0; app-offset payload 0 is
        reserved as the NULL sentinel (the header occupies file offset 0, so no
        object is ever at offset 0). The VM's oimg_resolve / is_ptr accessors
        special-case 0 -> NULL.
      * App-object references are FILE-relative byte offsets (blob_base = mmap base);
        objects are 8-byte aligned.
      * Field widths: refs and internal C pointers = oop_size; C int/uint32 fields
        (WIDTH4) = 4; Float.value (double) = 8; other scalars (size_t) = oop_size.
      * header `blob_size` = total file size (so blob_end = base + blob_size).
    """

    def __init__(self, layout, registry, registry_version=None):
        if registry_version is None:
            # Default to the generated registry's version so the blob's
            # registry_version always matches builtins.def (avoids a stale
            # hardcoded default when BUILTIN_REGISTRY_VERSION is bumped).
            from builtins_registry import VERSION as registry_version
        self.layout = layout
        self.structs = layout["structs"]
        self.oop = layout["oop_size"]
        self._check_inline_payloads()
        self.reg = registry
        self.w = OimgWriter(self.oop, version=1, registry_version=registry_version)

    # The two variable-size records, and the field whose bytes ride INLINE after
    # the header. Kept here rather than inline at the use sites so adding a third
    # such record is one line, not a new check.
    _INLINE_PAYLOADS = (("Symbol", "name"), ("String", "bytes"))

    def _check_inline_payloads(self):
        """Refuse a layout that disagrees with the structs it claims to describe.

        rete#190 stage 2 moved Symbol's name bytes inline. The VM, dump_layout.c
        and this serializer were all updated together — but the per-target layout
        JSONs under zephyr/ are CAPTURED FROM HARDWARE and checked in, and they
        were not. The C6 file went on saying `name` was a pointer at offset 4
        while the VM read the inline bytes at offset 8, so the serializer wrote
        every Symbol's text four bytes early. The image loaded, `format_version`
        matched at 9 on both sides, and the first send died with

            Message 'undOuter:slotCount:' not understood

        i.e. `__boundOuter:slotCount:` missing exactly the four bytes of the
        offset error. Silent, and only on a cross-compiled target: a host build
        regenerates its layout from a freshly built dump_layout every time, so no
        host gate can ever see it.

        format_version guards the FORMAT and cannot catch this — both ends agreed
        on 9; they disagreed about the ABI. This is that guard one level down.

        The invariant is the one a flexible array member cannot violate: the
        payload is not a pointer, and it begins exactly where the header ends.
        """
        # rete#226 — REFUSE A LAYOUT THAT PREDATES `Symbol.prim_id`, BY NAME.
        # Narrowing Symbol.length to pay for prim_id left the struct size and every offset
        # unchanged, which is what kept images from growing — and is exactly why the
        # offset==size check below cannot see the change. A layout without prim_id would
        # bake ids nowhere and write `length` a word wide over the field, producing an
        # image that loads, passes format_version, and runs with EVERY primitive
        # unintercepted. Regenerate it: flash zephyr/layoutdump/ to the board and capture
        # its console. See zephyr/README.md, "Where the layout JSON comes from".
        if "Symbol" in self.structs and "prim_id" not in self.structs["Symbol"]["fields"]:
            raise ValueError(
                "STALE LAYOUT: Symbol has no `prim_id` field. This layout was captured "
                "before rete#226 moved primitive classification onto the Symbol, so it "
                "describes a VM that no longer exists. Regenerate it by flashing "
                "zephyr/layoutdump/ to the target board and capturing its console output "
                "— that output IS the file (zephyr/README.md).")

        for struct_name, field in self._INLINE_PAYLOADS:
            s = self.structs.get(struct_name)
            if s is None:
                raise ValueError(
                    f"layout has no `{struct_name}` struct — it is not a layout for this VM")
            f = s["fields"].get(field)
            if f is None:
                raise ValueError(
                    f"layout's `{struct_name}` has no `{field}` field — stale layout; "
                    f"regenerate it (see below)")
            if f.get("is_ptr") or f["offset"] != s["size"]:
                raise ValueError(
                    f"STALE LAYOUT: `{struct_name}.{field}` says "
                    f"offset={f['offset']} is_ptr={bool(f.get('is_ptr'))} against "
                    f"{struct_name} size={s['size']}.\n"
                    f"  The bytes ride INLINE, so the payload must start exactly at the "
                    f"end of the header (offset == size) and must not be a pointer.\n"
                    f"  This layout describes a VM that no longer exists. Writing an "
                    f"image against it produces text read at the wrong offset — an image "
                    f"that loads, passes the format_version check, and then fails with a "
                    f"garbled selector (rete#190 stage 2).\n"
                    f"  Regenerate it: flash zephyr/layoutdump/ to the target board and "
                    f"capture its console output — that output IS the file. See "
                    f"zephyr/README.md, 'Where the layout JSON comes from'.")

    # ---- reference encoding -------------------------------------------------
    def _smi_fits(self, v):
        """True iff `v` fits the SMI ref-word's signed immediate at this oop
        width. RefWord.smi packs (v << 2) | TAG_SMI into an oop*8-bit word: TWO
        bits are spent on the tag, so only oop*8-2 bits remain for the payload,
        which itself is a signed field — i.e. the true capacity is oop*8-3
        magnitude bits: [-(2^(oop*8-3)), 2^(oop*8-3) - 1]. This is the single
        source of truth for that boundary; _smi()'s guard, the largeint
        object-collection pass, and _const_ref's routing all call this so they
        can never drift apart (https://github.com/davidg238/nsl-tuvm/issues/4).
        """
        lo = -(1 << (self.oop * 8 - 3))
        hi = (1 << (self.oop * 8 - 3))
        return lo <= v < hi

    def _smi(self, v):
        # https://github.com/davidg238/nsl-tuvm/issues/4: a previous guard of oop*8-2 bits was one bit too lenient
        # (it ignored that the payload is signed on top of the 2 tag bits), so
        # gap-band values silently overflowed into the sign bit at load time
        # (e.g. 2**61 loaded back as -2**61). Callers MUST route anything that
        # doesn't fit here to a LargeInteger record instead (see _const_ref) —
        # this guard only ever raises, it never itself widens/corrects.
        if not self._smi_fits(v):
            raise ValueError(f"SMI literal {v} does not fit signed {self.oop*8-3}-bit immediate")
        return RefWord.smi(v)

    def _aref(self, key):
        return RefWord.app_offset(self.offset[key])

    def _vtable_ref(self, name):
        """A vtable reference: builtin if firmware-owned, else app, NULL if None."""
        if name is None:
            return 0
        if name in self.reg:
            return RefWord.builtin(self.reg[name])
        return self._aref(f"vt:{name}")

    def _symbol_ref(self, name):
        if name in self.reg:
            return RefWord.builtin(self.reg[name])
        return self._aref(f"sym:{name}")

    @staticmethod
    def _sanitize(name):
        # Mirror solidify.SolidifyCompiler.sanitize_symbol_name exactly so the
        # METHOD: block-method id matches the C path's Method_Block_* id.
        m = {':': '_', '=': '_eq', '+': '_add', '-': '_sub', '*': '_mul',
             '/': '_div', '<': '_lt', '>': '_gt', '&': '_and', '|': '_or',
             '!': '_not', '?': '_q', '@': '_at', '%': '_mod'}
        out = []
        for ch in name:
            if ch.isalnum() or ch == '_':
                out.append(ch)
            elif ch in m:
                out.append(m[ch])
            else:
                out.append(f"_u{ord(ch)}")
        return "".join(out)

    @classmethod
    def _method_block_id(cls, name):
        # name == "METHOD:<blk>:<selector...>"  (selector may contain ':')
        parts = name.split(":")
        blk = parts[1]
        sel = ":".join(parts[2:])
        return f"Method_Block_{blk}_{cls._sanitize(sel)}"

    def _const_ref(self, c):
        # std-lib 3c-1: String literal constant — a typed tuple ("STRING", value);
        # resolved to the materialized String record (keyed by value, dedup'd).
        if isinstance(c, tuple) and c[0] == "STRING":
            return self._aref(f"str:{c[1]}")
        if isinstance(c, str):
            if c.startswith("SMI:"):
                v = int(c.split(":", 1)[1])
                # https://github.com/davidg238/nsl-tuvm/issues/4: the compiler's own SMI/LargeInteger split (in
                # ns_compile_oevm.py) is one bit too lenient too, so a "SMI:"
                # constant here can still land in the gap band. Route those to
                # a LargeInteger record instead of letting _smi() raise (or,
                # pre-fix, silently corrupt).
                if not self._smi_fits(v):
                    return self._aref(f"largeint:{v}")
                return self._smi(v)
            # VTABLE:/METHOD: placeholders are resolved like Path A's patched
            # get_symbol_ref: a class vtable / a CompiledMethod, NOT a symbol.
            if c.startswith("VTABLE:"):
                return self._vtable_ref(c.split(":", 1)[1])
            if c.startswith("METHOD:"):
                return self._aref(f"cm:{self._method_block_id(c)}")
            if c.startswith("LARGEINT:"):
                return self._aref(f"largeint:{int(c.split(':', 1)[1])}")
            return self._symbol_ref(c)
        if isinstance(c, bool):
            return RefWord.singleton(1 if c else 2)
        if isinstance(c, int):
            # https://github.com/davidg238/nsl-tuvm/issues/4: same gap-band routing as the "SMI:" string branch
            # above, for constants that are raw (unwrapped) Python ints.
            if not self._smi_fits(c):
                return self._aref(f"largeint:{c}")
            return self._smi(c)
        if isinstance(c, float):
            return self._aref(f"float:{c!r}")
        return 0  # unknown -> NULL

    # ---- field writing ------------------------------------------------------
    def _scalar_bytes(self, fname, value):
        if fname in WIDTH4:
            return struct.pack("<I", value & 0xFFFFFFFF)
        if fname == "value":  # Float.value (double)
            return struct.pack("<d", value)
        if fname in WIDTH8_SIGNED:    # LargeInteger.sign (int64), fixed width
            return struct.pack("<q", value)
        if fname in WIDTH8_UNSIGNED:  # LargeInteger.mag (uint64), fixed width
            return struct.pack("<Q", value)
        return (value & ((1 << (self.oop * 8)) - 1)).to_bytes(self.oop, "little")

    def _field_width(self, struct_name, fname):
        """Byte width of a scalar field, DERIVED from the captured offsets.

        rete#226. The layout records offsets and the struct size, never widths, so a
        width restated here would be a second copy of the ABI — the exact thing
        layout-$BOARD.json exists to stop. The distance to the next field is the width,
        and for the last one it is the distance to the end of the header. This is what
        lets Symbol.length narrow from a word to 16 bits without a line changing here.
        """
        fields = self.structs[struct_name]["fields"]
        off = fields[fname]["offset"]
        later = sorted(f["offset"] for f in fields.values() if f["offset"] > off)
        end = later[0] if later else self.structs[struct_name]["size"]
        return end - off

    def _put_scalar(self, buf, base, struct_name, fname, value):
        w = self._field_width(struct_name, fname)
        if value >= (1 << (w * 8)):
            raise ValueError(
                f"{struct_name}.{fname} = {value} does not fit the {w} byte(s) the "
                f"captured layout gives it.")
        self._put(buf, base, struct_name, fname, value.to_bytes(w, "little"))

    def _put(self, buf, base, struct_name, fname, raw):
        foff = self.structs[struct_name]["fields"][fname]["offset"]
        buf[base + foff: base + foff + len(raw)] = raw

    def method_name_map(self, graph):
        """#164: blob-relative offset of every CompiledMethod -> its name.

        A CompiledMethod carries no name at runtime, so the VM can report a
        method only as `ptr - oimg_blob_base` — a blob-relative byte offset.
        This is the join key, and this map is the other half of it.

        Two names per entry, because neither alone is enough:
          - `qual`  "Vtable>>selector", the readable one, and the same
                    convention stmap.py uses so the two sidecars agree. Absent
                    for a method no vtable binds.
          - `id`    the serializer's own key (`Method_<Vtable>_<sel>`,
                    `Method_Block_...`), ALWAYS present. This is what names
                    BLOCKS, which no vtable binds and which are exactly what a
                    per-activation profile needs to see (#37: block activation
                    is the STIC gap).

        Call AFTER generate() — self.offset is assigned there.
        """
        qual = {}
        for vt_name, vt in graph.vtables.items():
            if vt_name in self.reg:          # firmware-owned; not serialized
                continue
            for sel, method_id in vt["bindings"]:
                # A method bound in more than one vtable keeps every name rather
                # than the last one to win: which binding was activated is not
                # knowable from the offset, and silently picking one would print
                # a confident wrong answer.
                qual.setdefault(method_id, []).append(f"{vt_name}>>{sel}")
        out = {}
        for m in graph.methods:
            off = self.offset.get(f"cm:{m['id']}")
            if off is None:
                continue
            out[str(off)] = {"id": m["id"], "qual": qual.get(m["id"], [])}
        return out

    # ---- main ---------------------------------------------------------------
    def generate(self, graph, entry_vtable_name,
                 init_sel_name=None, init_argc=0, main_sel_name="main:args:",
                 entry_nslots=0, kernel_vtable_name=None, kernel_init_sel_name=None,
                 kernel_nslots=0, serial_on_sel_name=None,
                 supervisor_vtable_name=None, supervisor_init_sel_name=None,
                 supervisor_run_sel_name=None, supervisor_nslots=0,
                 net_on_sel_name=None):
        # 1. Collect objects per kind and the trailing arrays they own.
        objs = {k: [] for k in KIND_ORDER}      # kind -> [key]
        floats = {}                              # repr -> value
        largeints = {}                           # int value -> int value

        # Collect every app symbol actually referenced: declared symbols, binding
        # selectors, and string constants. (The oimg path bypasses generate_c's
        # symbol pre-pass, so graph.symbols alone is not complete.)
        app_syms = set(graph.symbols.keys())
        app_strings = set()                      # String-constant values (3c-1)
        for vt in graph.vtables.values():
            for sel, _mid in vt["bindings"]:
                app_syms.add(sel)
        for m in graph.methods:
            for c in m["constants"]:
                # VTABLE:/METHOD: are vtable/CompiledMethod placeholders, not
                # symbols (resolved by _const_ref); SMI: is an immediate.
                if (isinstance(c, str) and not c.startswith("SMI:")
                        and not c.startswith("VTABLE:") and not c.startswith("METHOD:")):
                    app_syms.add(c)
                # std-lib 3c-1: String literals compile to a typed-tuple constant
                # ("STRING", value); dedup by value (Strings are immutable).
                elif isinstance(c, tuple) and c[0] == "STRING":
                    app_strings.add(c[1])
        # The entry descriptor's init/main selectors must be resolvable as blob
        # symbols even if they happen not to be bound/referenced anywhere else.
        if init_sel_name and init_sel_name not in self.reg:
            app_syms.add(init_sel_name)
        if main_sel_name and main_sel_name not in self.reg:
            app_syms.add(main_sel_name)
        # in-image debugger step 1: Supervisor selector symbols must also be
        # resolvable as blob symbols for the v5 header.
        if supervisor_init_sel_name and supervisor_init_sel_name not in self.reg:
            app_syms.add(supervisor_init_sel_name)
        if supervisor_run_sel_name and supervisor_run_sel_name not in self.reg:
            app_syms.add(supervisor_run_sel_name)
        self.app_syms = sorted(s for s in app_syms if s not in self.reg)
        self.app_strings = sorted(app_strings)
        # rete#190 stage 2: no objs["SYMBOL"] bucket — sym: offsets are assigned
        # by the variable-size reserve loop below, not the fixed-size KIND loop.

        for vt_name in graph.vtables:
            if vt_name not in self.reg:          # builtin vtables are firmware-owned
                objs["VTABLE"].append(f"vt:{vt_name}")

        # NOTE (Phase-2 refinement): methods are dumped flat. Methods bound only to
        # builtin (firmware-owned) vtables become unreferenced orphans in the blob —
        # harmless wasted bytes for the host proof, but Task 10 (load+run) should
        # switch to reachability-based emission from the root to prune them.
        for m in graph.methods:
            objs["METHOD"].append(f"cm:{m['id']}")
            objs["CLOSURE"].append(f"cl:{m['id']}")
            for c in m["constants"]:
                if isinstance(c, float):
                    floats.setdefault(repr(c), c)
                elif isinstance(c, str) and c.startswith("LARGEINT:"):
                    iv = int(c.split(":", 1)[1])
                    largeints.setdefault(iv, iv)
                elif isinstance(c, str) and c.startswith("SMI:"):
                    # https://github.com/davidg238/nsl-tuvm/issues/4: a "SMI:" constant that doesn't fit the SMI
                    # ref-word (the compiler's own SMI/LargeInteger split is one
                    # bit too lenient too) still needs a LargeInteger record —
                    # keyed into the same largeint:{v} namespace as LARGEINT:
                    # constants so an equal value dedups to one record.
                    iv = int(c.split(":", 1)[1])
                    if not self._smi_fits(iv):
                        largeints.setdefault(iv, iv)
                elif isinstance(c, int) and not isinstance(c, bool):
                    # Raw (unwrapped) Python-int constant, same gap-band rule.
                    if not self._smi_fits(c):
                        largeints.setdefault(c, c)
        for r in floats:
            objs["FLOAT"].append(f"float:{r}")
        for iv in largeints:
            objs["LARGEINT"].append(f"largeint:{iv}")

        # 2. Assign file-relative offsets: header, then object records by kind,
        #    then trailing arrays. Offset 0 reserved (NULL sentinel).
        # Use a sentinel (1) for kernel_vtable_ref to get the correct header
        # size: in v8 the Kernel descriptor gates the whole fixed descriptor
        # block (serial/supervisor/net slots are always emitted with it).
        _header_sentinel = 1 if kernel_vtable_name else 0
        header_len = len(self.w.header_bytes(0, 0, 0, kernel_vtable_ref=_header_sentinel))
        self.offset = {}
        cursor = _align(header_len)

        for kind in KIND_ORDER:
            sname = KIND_STRUCT[kind]
            size = self.structs[sname]["size"]
            for key in objs[kind]:
                cursor = _align(cursor)
                self.offset[key] = cursor
                cursor += size

        # trailing arrays
        def reserve(key, nbytes):
            nonlocal cursor
            cursor = _align(cursor)
            self.offset[key] = cursor
            cursor += nbytes

        # rete#190 stage 2: Symbols are VARIABLE-size (header + inline
        # NUL-terminated bytes), so they cannot ride the fixed-size-per-kind
        # reserve loop above — same treatment String has had since std-lib
        # 3c-1. The separate `strbytes:` allocation is gone: the payload lives
        # in the record itself, after `length`.
        sym_hdr = self.structs["Symbol"]["size"]
        for name in self.app_syms:
            reserve(f"sym:{name}", sym_hdr + len(name.encode("utf-8")) + 1)
        # std-lib 3c-1: String constants are VARIABLE-size (header + inline bytes),
        # so they cannot ride the fixed-size-per-kind reserve loop; reserve each
        # individually like a trailing array (bytes live inline after `length`).
        str_hdr = self.structs["String"]["size"]
        for sval in self.app_strings:
            reserve(f"str:{sval}", str_hdr + len(sval.encode("utf-8")))
        bind_sz = self.structs["Binding"]["size"]
        for vt_name, vt in graph.vtables.items():
            if vt_name not in self.reg and vt["bindings"]:
                reserve(f"bindings:{vt_name}", len(vt["bindings"]) * bind_sz)
        for m in graph.methods:
            reserve(f"insts:{m['id']}", len(m["instructions"]) * 4)
            if m["constants"]:
                reserve(f"consts:{m['id']}", len(m["constants"]) * self.oop)

        total = _align(cursor)
        buf = bytearray(total)

        # 3. Emit records.
        sym_bytes_off = self.structs["Symbol"]["fields"]["name"]["offset"]
        for name in self.app_syms:
            base = self.offset[f"sym:{name}"]
            # `length` is a BYTE count, not a character count — it must share the
            # UTF-8 basis with the reservation and the payload below, exactly as
            # the String block does with its own len(raw). Symbol `=` is a
            # length-then-byte compare (dispatch.c), so a non-ASCII symbol whose
            # length was a character count would never equal the same symbol
            # decoded through `platform asSymbol:`.
            nb = name.encode("utf-8")
            raw = nb + b"\x00"
            self._put(buf, base, "Symbol", "base.vtable", self.w.word(RefWord.builtin(self.reg["SymbolVtable"])))
            self._put_scalar(buf, base, "Symbol", "length", len(nb))
            # rete#226 — BAKE THE PRIMITIVE ID. Whether a selector names a primitive is a
            # pure function of its name and fixed for the life of the Symbol, so it is
            # decided here, once, instead of by a runtime cache whose unbounded probe was
            # #222/#192/#213 and whose 256 target slots filled seventeen seconds into every
            # boot. The map comes from oevm/src/prim_names.def — the same file dispatch.c
            # builds PRIM_NAMES from, so the list is not restated in Python.
            self._put_scalar(buf, base, "Symbol", "prim_id", PRIM_IDS.get(name, 0))
            nbase = base + sym_bytes_off
            buf[nbase: nbase + len(raw)] = raw

        # std-lib 3c-1: String constants — INLINE bytes (no separate strbytes
        # pointer; the payload lives in the record itself after `length`).
        ibytes_off = self.structs["String"]["fields"]["bytes"]["offset"]
        for sval in self.app_strings:
            base = self.offset[f"str:{sval}"]
            raw = sval.encode("utf-8")
            self._put(buf, base, "String", "base.vtable", self.w.word(RefWord.builtin(self.reg["StringVtable"])))
            self._put(buf, base, "String", "length", self._scalar_bytes("length", len(raw)))
            ibase = base + ibytes_off
            buf[ibase: ibase + len(raw)] = raw

        for vt_name, vt in graph.vtables.items():
            if vt_name in self.reg:              # firmware-owned; not serialized
                continue
            base = self.offset[f"vt:{vt_name}"]
            meta = vt.get("meta") or "VtableVtable"
            self._put(buf, base, "Vtable", "base.vtable", self.w.word(self._vtable_ref(meta)))
            self._put(buf, base, "Vtable", "delegate", self.w.word(self._vtable_ref(vt["delegate"])))
            nb = len(vt["bindings"])
            bptr = self._aref(f"bindings:{vt_name}") if nb else 0
            self._put(buf, base, "Vtable", "bindings", self.w.word(bptr))
            self._put(buf, base, "Vtable", "capacity", self._scalar_bytes("capacity", nb))
            self._put(buf, base, "Vtable", "size", self._scalar_bytes("size", nb))
            self._put(buf, base, "Vtable", "is_nested", self._scalar_bytes("is_nested", vt["is_nested"]))
            if nb:
                abase = self.offset[f"bindings:{vt_name}"]
                for i, (sel, method_id) in enumerate(vt["bindings"]):
                    bb = abase + i * bind_sz
                    self._put(buf, bb, "Binding", "symbol", self.w.word(self._symbol_ref(sel)))
                    self._put(buf, bb, "Binding", "method", self.w.word(self._aref(f"cl:{method_id}")))

        for m in graph.methods:
            base = self.offset[f"cm:{m['id']}"]
            self._put(buf, base, "CompiledMethod", "instructions", self.w.word(self._aref(f"insts:{m['id']}")))
            self._put(buf, base, "CompiledMethod", "instruction_count", self._scalar_bytes("instruction_count", len(m["instructions"])))
            self._put(buf, base, "CompiledMethod", "num_registers", self._scalar_bytes("num_registers", m["num_registers"]))
            self._put(buf, base, "CompiledMethod", "num_arguments", self._scalar_bytes("num_arguments", m["num_arguments"]))
            nc = len(m["constants"])
            cptr = self._aref(f"consts:{m['id']}") if nc else 0
            self._put(buf, base, "CompiledMethod", "constants", self.w.word(cptr))
            self._put(buf, base, "CompiledMethod", "constant_count", self._scalar_bytes("constant_count", nc))
            self._put(buf, base, "CompiledMethod", "access_modifier", self._scalar_bytes("access_modifier", m.get("access_modifier", 0)))
            ibase = self.offset[f"insts:{m['id']}"]
            for i, w in enumerate(m["instructions"]):
                buf[ibase + i * 4: ibase + i * 4 + 4] = struct.pack("<I", w & 0xFFFFFFFF)
            if nc:
                cbase = self.offset[f"consts:{m['id']}"]
                for i, c in enumerate(m["constants"]):
                    buf[cbase + i * self.oop: cbase + (i + 1) * self.oop] = self.w.word(self._const_ref(c))

            # closure for this method
            cl = self.offset[f"cl:{m['id']}"]
            self._put(buf, cl, "Closure", "base.vtable", self.w.word(RefWord.builtin(self.reg["BytecodeClosureVtable"])))
            self._put(buf, cl, "Closure", "home_ctx", self.w.word(0))           # NULL
            self._put(buf, cl, "Closure", "home_generation", self._scalar_bytes("home_generation", 0))
            self._put(buf, cl, "Closure", "entry_point", self.w.word(self._aref(f"cm:{m['id']}")))

        for r, v in floats.items():
            base = self.offset[f"float:{r}"]
            self._put(buf, base, "Float", "base.vtable", self.w.word(RefWord.builtin(self.reg["FloatVtable"])))
            self._put(buf, base, "Float", "value", self._scalar_bytes("value", v))

        for iv in largeints:
            base = self.offset[f"largeint:{iv}"]
            self._put(buf, base, "LargeInteger", "base.vtable", self.w.word(RefWord.builtin(self.reg["LargeIntegerVtable"])))
            self._put(buf, base, "LargeInteger", "sign", self._scalar_bytes("sign", -1 if iv < 0 else 1))
            self._put(buf, base, "LargeInteger", "mag", self._scalar_bytes("mag", abs(iv)))

        # 4. Header (now that total size + root offset are known).
        cluster_count = sum(1 for k in KIND_ORDER if objs[k])
        root_ref = self._vtable_ref(entry_vtable_name)
        # init/main selector symbols are app symbols (in the blob); resolve their
        # app-offset ref-words (offsets already assigned above).
        init_sel_ref = self._symbol_ref(init_sel_name) if init_sel_name else 0
        main_sel_ref = self._symbol_ref(main_sel_name) if main_sel_name else 0
        # std-lib step 3b: Kernel module descriptor (format_version 3).
        kernel_vtable_ref = self._vtable_ref(kernel_vtable_name) if kernel_vtable_name else 0
        kernel_init_sel_ref = self._symbol_ref(kernel_init_sel_name) if kernel_init_sel_name else 0
        # std-lib step 3c-3b: serial capability selector ref (format_version 4).
        serial_on_sel_ref = self._symbol_ref(serial_on_sel_name) if serial_on_sel_name else 0
        # R4: UDP/net capability selector ref.
        net_on_sel_ref = self._symbol_ref(net_on_sel_name) if net_on_sel_name else 0
        # in-image debugger step 1: Supervisor descriptor.
        supervisor_vtable_ref = self._vtable_ref(supervisor_vtable_name) if supervisor_vtable_name else 0
        supervisor_init_sel_ref = self._symbol_ref(supervisor_init_sel_name) if supervisor_init_sel_name else 0
        supervisor_run_sel_ref = self._symbol_ref(supervisor_run_sel_name) if supervisor_run_sel_name else 0
        header = self.w.header_bytes(total, cluster_count, root_ref,
                                     init_sel_ref=init_sel_ref,
                                     main_sel_ref=main_sel_ref,
                                     init_argc=init_argc,
                                     entry_nslots=entry_nslots,
                                     kernel_vtable_ref=kernel_vtable_ref,
                                     kernel_init_sel_ref=kernel_init_sel_ref,
                                     kernel_nslots=kernel_nslots,
                                     serial_on_sel_ref=serial_on_sel_ref,
                                     supervisor_vtable_ref=supervisor_vtable_ref,
                                     supervisor_init_sel_ref=supervisor_init_sel_ref,
                                     supervisor_run_sel_ref=supervisor_run_sel_ref,
                                     supervisor_nslots=supervisor_nslots,
                                     net_on_sel_ref=net_on_sel_ref)
        assert len(header) == header_len, \
            f"header size mismatch: precomputed {header_len}, emitted {len(header)}"
        buf[0:len(header)] = header
        return bytes(buf)

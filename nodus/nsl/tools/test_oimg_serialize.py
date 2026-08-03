import pytest
from oimg_serialize import RefWord, OimgWriter, OimgSerializer


def test_refword_roundtrip():
    assert RefWord.decode(RefWord.app_offset(0x40)) == (0b00, 0x40)
    assert RefWord.decode(RefWord.builtin(4)) == (0b01, 4)
    assert RefWord.decode(RefWord.smi(-3)) == (0b10, -3)
    assert RefWord.decode(RefWord.smi(7)) == (0b10, 7)
    assert RefWord.decode(RefWord.singleton(0)) == (0b11, 0)


def test_app_offset_must_be_aligned():
    with pytest.raises(ValueError):
        RefWord.app_offset(0x41)  # not 4-aligned


def test_writer_serializes_negative_smi_word_unsigned():
    # A negative SMI ref-word must serialize as two's-complement at oop width.
    w = OimgWriter(oop_size=8, version=1, registry_version=1)
    raw = w.word(RefWord.smi(-3))
    assert len(raw) == 8
    assert int.from_bytes(raw, "little") == (RefWord.smi(-3) & ((1 << 64) - 1))


def test_header_layout():
    w = OimgWriter(oop_size=8, version=1, registry_version=2)
    h = w.header_bytes(blob_size=0x100, cluster_count=4, root_ref=RefWord.builtin(3),
                       init_sel_ref=RefWord.app_offset(0x40),
                       main_sel_ref=RefWord.app_offset(0x80),
                       init_argc=1, entry_nslots=3)
    assert h[:4] == b"OIMG"
    import struct
    assert struct.unpack_from("<H", h, 4)[0] == 1   # version
    assert struct.unpack_from("<I", h, 8)[0] == 2   # registry_version
    assert struct.unpack_from("<I", h, 12)[0] == 0x100  # blob_size
    assert struct.unpack_from("<I", h, 16)[0] == 4      # cluster_count
    # ref-word block (W=8): root@24, init@32, main@40, then two u16s, pad to 8.
    assert struct.unpack_from("<Q", h, 24)[0] == RefWord.builtin(3)
    assert struct.unpack_from("<Q", h, 32)[0] == RefWord.app_offset(0x40)
    assert struct.unpack_from("<Q", h, 40)[0] == RefWord.app_offset(0x80)
    assert struct.unpack_from("<H", h, 48)[0] == 1   # init_argc
    assert struct.unpack_from("<H", h, 50)[0] == 3   # entry_nslots
    assert len(h) == 56 and len(h) % 8 == 0          # 8-aligned object region


@pytest.mark.parametrize("W", [4, 8])
def test_descriptor_block_is_dense_word_strided(W):
    """The loader (oevm/src/oimg_loader.c) reads every header descriptor ref-word
    at a uniform `a + n*W` stride (a = ALIGN_UP(20, W)), with NO inter-group
    padding. The serializer MUST lay them out at exactly those offsets for every
    oop width — otherwise 32-bit (W=4) images read kernel/serial/supervisor refs
    from the wrong offset (regression: NULL-vtable kernel instance → DNU).

    This guards the W==ALIGN coincidence that masked the bug on 64-bit only.
    """
    import struct
    w = OimgWriter(oop_size=W, version=1, registry_version=1)
    # A full v5 header: entry + kernel + serial + supervisor descriptors present.
    h = w.header_bytes(
        blob_size=0x100, cluster_count=4,
        root_ref=RefWord.builtin(3),
        init_sel_ref=RefWord.app_offset(0x40),
        main_sel_ref=RefWord.app_offset(0x80),
        init_argc=2, entry_nslots=1,
        kernel_vtable_ref=RefWord.app_offset(0x100),
        kernel_init_sel_ref=RefWord.app_offset(0x140),
        kernel_nslots=1,
        serial_on_sel_ref=RefWord.app_offset(0x180),
        supervisor_vtable_ref=RefWord.app_offset(0x1c0),
        supervisor_init_sel_ref=RefWord.app_offset(0x200),
        supervisor_run_sel_ref=RefWord.app_offset(0x240),
        supervisor_nslots=1,
    )
    a = (20 + (W - 1)) & ~(W - 1)            # ALIGN_UP(OIMG_PREFIX_FIXED, W)
    rd = (lambda off: struct.unpack_from("<I" if W == 4 else "<Q", h, off)[0])
    # loader offset -> expected ref-word value (mirrors oimg_loader.c read order)
    assert rd(a) == RefWord.builtin(3)                       # root @ a
    assert rd(a + W) == RefWord.app_offset(0x40)             # init_sel @ a+W
    assert rd(a + 2 * W) == RefWord.app_offset(0x80)         # main_sel @ a+2W
    assert rd(a + 4 * W) == RefWord.app_offset(0x100)        # kernel_vtable @ a+4W
    assert rd(a + 5 * W) == RefWord.app_offset(0x140)        # kernel_init @ a+5W
    assert rd(a + 7 * W) == RefWord.app_offset(0x180)        # serial_on @ a+7W
    assert rd(a + 8 * W) == RefWord.app_offset(0x1c0)        # supervisor_vtable @ a+8W
    assert rd(a + 9 * W) == RefWord.app_offset(0x200)        # supervisor_init @ a+9W
    assert rd(a + 10 * W) == RefWord.app_offset(0x240)       # supervisor_run @ a+10W


# ---------------------------------------------------------------------------
# _smi() capacity boundary (https://github.com/davidg238/nsl-tuvm/issues/4): the ref-word packs (v << 2) | TAG_SMI
# into an oop*8-bit word, so only oop*8-2 bits remain for the signed payload;
# TWO bits are consumed by the tag, so the true SMI capacity is oop*8-3
# magnitude bits: [-(2^(oop*8-3)), 2^(oop*8-3) - 1]. A guard of oop*8-2 (one
# bit too lenient) lets gap-band values silently overflow into the sign bit.
# ---------------------------------------------------------------------------

def _minimal_serializer(oop=8):
    # _smi()/_smi_fits() only need oop_size; structs/registry are irrelevant here.
    return OimgSerializer({"oop_size": oop, "structs": {}}, {}, registry_version=1)


def test_smi_accepts_true_capacity_boundary_values():
    ser = _minimal_serializer()
    hi = (1 << (ser.oop * 8 - 3)) - 1     # 2**61 - 1: largest representable SMI
    lo = -(1 << (ser.oop * 8 - 3))        # -2**61: smallest representable SMI
    assert RefWord.decode(ser._smi(hi)) == (0b10, hi)
    assert RefWord.decode(ser._smi(lo)) == (0b10, lo)


def test_smi_rejects_gap_band_values():
    ser = _minimal_serializer()
    over_hi = 1 << (ser.oop * 8 - 3)           # 2**61: one past the true max
    under_lo = -(1 << (ser.oop * 8 - 3)) - 1   # -2**61 - 1: one past the true min
    with pytest.raises(ValueError):
        ser._smi(over_hi)
    with pytest.raises(ValueError):
        ser._smi(under_lo)

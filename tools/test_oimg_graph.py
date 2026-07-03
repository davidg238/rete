import json
import struct
from oimg_serialize import OimgSerializer
from builtins_registry import REGISTRY, VERSION

LAYOUT = json.loads('''{"oop_size":8,"structs":{
 "Symbol":{"size":24,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false},
   "name":{"offset":8,"is_ref":false,"is_ptr":true},"length":{"offset":16,"is_ref":false,"is_ptr":false}}},
 "Vtable":{"size":48,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false},
   "delegate":{"offset":8,"is_ref":true,"is_ptr":false},"bindings":{"offset":16,"is_ref":false,"is_ptr":true},
   "capacity":{"offset":24,"is_ref":false,"is_ptr":false},"size":{"offset":32,"is_ref":false,"is_ptr":false},
   "is_nested":{"offset":40,"is_ref":false,"is_ptr":false}}},
 "Binding":{"size":16,"fields":{"symbol":{"offset":0,"is_ref":true,"is_ptr":false},
   "method":{"offset":8,"is_ref":true,"is_ptr":false}}},
 "CompiledMethod":{"size":56,"fields":{"instructions":{"offset":0,"is_ref":false,"is_ptr":true},
   "instruction_count":{"offset":8,"is_ref":false,"is_ptr":false},"num_registers":{"offset":16,"is_ref":false,"is_ptr":false},
   "num_arguments":{"offset":20,"is_ref":false,"is_ptr":false},"constants":{"offset":24,"is_ref":false,"is_ptr":true},
   "constant_count":{"offset":32,"is_ref":false,"is_ptr":false},"access_modifier":{"offset":40,"is_ref":false,"is_ptr":false}}},
 "Closure":{"size":32,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false},
   "home_ctx":{"offset":8,"is_ref":true,"is_ptr":false},"home_generation":{"offset":16,"is_ref":false,"is_ptr":false},
   "entry_point":{"offset":24,"is_ref":false,"is_ptr":true}}},
 "Float":{"size":16,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false},
   "value":{"offset":8,"is_ref":false,"is_ptr":false}}},
 "ObjectInstance":{"size":8,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false}}},
 "BoxedBinding":{"size":24,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false},
   "symbol":{"offset":8,"is_ref":true,"is_ptr":false},"method":{"offset":16,"is_ref":true,"is_ptr":false}}}
}}''')


class Graph:  # mirrors solidify.py field names
    symbols = {"increment": "Symbol_increment"}
    vtables = {"CounterVtable": {"delegate": None,
                                 "bindings": [("increment", "Method_CounterVtable_increment")],
                                 "meta": None, "is_nested": 0}}
    methods = [{"id": "Method_CounterVtable_increment", "num_registers": 4, "num_arguments": 0,
                "constants": ["SMI:1"],
                "instructions": [0x0000010C, 0x00000200, 0x02010306, 0x0000030D, 0x00000304],
                "access_modifier": 0}]


def test_header_and_root():
    blob = OimgSerializer(LAYOUT, REGISTRY).generate(Graph, "CounterVtable")
    assert blob[:4] == b"OIMG"
    assert struct.unpack_from("<H", blob, 4)[0] == 1          # format_version
    assert struct.unpack_from("<I", blob, 8)[0] == VERSION    # registry_version
    blob_size = struct.unpack_from("<I", blob, 12)[0]
    cluster_count = struct.unpack_from("<I", blob, 16)[0]
    assert cluster_count == 4   # SYMBOL, VTABLE, METHOD, CLOSURE
    assert blob_size == len(blob)


def test_increment_method_instructions_present():
    blob = OimgSerializer(LAYOUT, REGISTRY).generate(Graph, "CounterVtable")
    needle = b"".join(struct.pack("<I", w) for w in Graph.methods[0]["instructions"])
    assert needle in blob


def test_root_ref_points_at_a_vtable_record():
    blob = OimgSerializer(LAYOUT, REGISTRY).generate(Graph, "CounterVtable")
    # root_ref is the first word-sized field after the 20-byte fixed header, 8-aligned.
    root_ref = struct.unpack_from("<Q", blob, 24)[0]
    tag = root_ref & 0b11
    off = root_ref >> 2
    assert tag == 0b00            # app-object
    assert 0 < off < len(blob)    # points inside the blob


def test_entry_descriptor_in_header():
    blob = OimgSerializer(LAYOUT, REGISTRY).generate(
        Graph, "CounterVtable",
        init_sel_name="init_new", init_argc=0,
        main_sel_name="increment", entry_nslots=2)
    # init/main selector ref-words: app-offset, pointing inside the blob.
    init_ref = struct.unpack_from("<Q", blob, 32)[0]
    main_ref = struct.unpack_from("<Q", blob, 40)[0]
    assert (init_ref & 0b11) == 0b00 and 0 < (init_ref >> 2) < len(blob)
    assert (main_ref & 0b11) == 0b00 and 0 < (main_ref >> 2) < len(blob)
    assert struct.unpack_from("<H", blob, 48)[0] == 0   # init_argc
    assert struct.unpack_from("<H", blob, 50)[0] == 2   # entry_nslots
    # The init selector "init_new" was emitted as a blob symbol even though it is
    # not a binding selector in this graph.
    assert b"init_new\x00" in blob


# ---------------------------------------------------------------------------
# _const_ref branch coverage: VTABLE:, METHOD:, SMI:, plain symbol
# ---------------------------------------------------------------------------

class GraphWithConstRefs:
    """Graph whose method constants exercise all four _const_ref branches:
      - VTABLE:<Name>      -> app-offset ref to the vtable record
      - METHOD:<blk>:<sel> -> app-offset ref to the CompiledMethod record
      - SMI:<n>            -> SMI ref-word (tag 0b10)
      - plain symbol name  -> app-offset ref to the symbol record
    The block method id follows _method_block_id():
      METHOD:BlkA:at:put: -> Method_Block_BlkA_at_put_ (via _sanitize)
    """
    symbols = {"increment": "Symbol_increment"}
    vtables = {"HostVtable": {"delegate": None,
                              "bindings": [("increment", "Method_HostVtable_increment")],
                              "meta": None, "is_nested": 0}}
    methods = [
        # main method: constants cover all four branches
        {"id": "Method_HostVtable_increment", "num_registers": 2, "num_arguments": 0,
         "constants": [
             "VTABLE:HostVtable",           # branch: VTABLE: -> vtable ref
             "METHOD:BlkA:at:put:",         # branch: METHOD: -> cm: ref
             "SMI:7",                       # branch: SMI: -> smi ref-word
             "increment",                   # branch: plain symbol -> sym: ref
         ],
         "instructions": [0x0000010C],
         "access_modifier": 0},
        # block method referenced by METHOD:BlkA:at:put:
        # _method_block_id("METHOD:BlkA:at:put:") = "Method_Block_BlkA_at_put_"
        {"id": "Method_Block_BlkA_at_put_", "num_registers": 2, "num_arguments": 2,
         "constants": [],
         "instructions": [0x00000304],
         "access_modifier": 0},
    ]


def test_const_ref_branches():
    """_const_ref emits the correct ref-word tag and payload for each constant kind."""
    from oimg_serialize import OimgSerializer, RefWord

    ser = OimgSerializer(LAYOUT, REGISTRY)
    blob = ser.generate(GraphWithConstRefs, "HostVtable")

    # The serializer populates ser.offset during generate(); use it to derive
    # expected ref-words for each constant kind.

    # -- VTABLE:HostVtable -> app-offset pointing at the vt: record
    vtable_key = "vt:HostVtable"
    vtable_off = ser.offset[vtable_key]
    expected_vtable_ref = RefWord.app_offset(vtable_off)

    # -- METHOD:BlkA:at:put: -> app-offset pointing at the cm: record
    # _method_block_id splits on ':': parts=['METHOD','BlkA','at','put','']
    # blk='BlkA', sel='at:put:', _sanitize('at:put:') -> 'at_put_'
    block_cm_key = "cm:Method_Block_BlkA_at_put_"
    block_cm_off = ser.offset[block_cm_key]
    expected_method_ref = RefWord.app_offset(block_cm_off)

    # -- SMI:7 -> smi ref-word (tag 0b10)
    expected_smi_ref = RefWord.smi(7)

    # -- plain "increment" symbol -> app-offset pointing at sym: record
    sym_key = "sym:increment"
    sym_off = ser.offset[sym_key]
    expected_sym_ref = RefWord.app_offset(sym_off)

    # Read the serialized constant array for Method_HostVtable_increment
    consts_off = ser.offset["consts:Method_HostVtable_increment"]
    oop = LAYOUT["oop_size"]
    actual = [
        struct.unpack_from("<Q", blob, consts_off + i * oop)[0]
        for i in range(4)
    ]

    assert actual[0] == expected_vtable_ref,  f"VTABLE: ref mismatch: {actual[0]:#x} != {expected_vtable_ref:#x}"
    assert actual[1] == expected_method_ref,  f"METHOD: ref mismatch: {actual[1]:#x} != {expected_method_ref:#x}"
    assert actual[2] == expected_smi_ref,     f"SMI: ref mismatch: {actual[2]:#x} != {expected_smi_ref:#x}"
    assert actual[3] == expected_sym_ref,     f"symbol ref mismatch: {actual[3]:#x} != {expected_sym_ref:#x}"

    # Confirm tags match expectations
    assert (actual[0] & 0b11) == 0b00, "VTABLE: should be app-object tag"
    assert (actual[1] & 0b11) == 0b00, "METHOD: should be app-object tag"
    assert (actual[2] & 0b11) == 0b10, "SMI: should be smi tag"
    assert (actual[3] & 0b11) == 0b00, "plain symbol should be app-object tag"

"""Single source of truth for hand-authored bootstrap kernel methods that
cannot be expressed in Newspeak (e.g. unwindTo:value:, the NLR mechanism
itself). Both tools/solidify.py and tools/ns_compile_oevm.py install these
via add_unwind_to_value(); the C copy in oevm/src/kernel_boot.c is held in
sync by tools/check_kernel_parity.py (it cannot share Python source)."""

UNWIND_TO_VALUE = {
    "vtable_name": "MethodContextVtable",
    "selector": "unwindTo:value:",
    "num_registers": 6,
    "num_arguments": 2,
    "constants": ["==", "executeCleanup", "resumeWith:", "unwindTo:value:", "SMI:0"],
    "instructions": [
        # The NLR mechanism: walk the activation chain comparing each frame's
        # identity to the target context (R1), calling executeCleanup on each,
        # then either resumeWith: on the matching frame (base case) or
        # recursing via OP_LD_SENDER to the next outer frame (recursive case).
        #
        # Register layout (on entry):
        #   R0 = self (current MethodContext being examined)
        #   R1 = target context (the frame we are unwinding TO)
        #   R2 = return value
        #
        # Instruction-by-instruction:
        0x00000301, # 0:  OP_LD_REG   dest=3, src1=0       R3 = R0  (self)
        0x00010401, # 1:  OP_LD_REG   dest=4, src1=1       R4 = R1  (target)
        0x03000102, # 2:  OP_SEND     argc=1, sel=0("=="), rcvr=3   R3 = R3 == R4
        0x0007030B, # 3:  OP_JMP_IF_FALSE cond=3, offset=7  if R3 false, jump to 11
        #  --- base case: self IS the target ---
        0x00000301, # 4:  OP_LD_REG   dest=3, src1=0       R3 = R0
        0x03010002, # 5:  OP_SEND     argc=0, sel=1("executeCleanup"), rcvr=3
        0x00000301, # 6:  OP_LD_REG   dest=3, src1=0       R3 = R0
        0x00020401, # 7:  OP_LD_REG   dest=4, src1=2       R4 = R2  (value)
        0x03020102, # 8:  OP_SEND     argc=1, sel=2("resumeWith:"), rcvr=3
        0x00040300, # 9:  OP_LD_CONST dest=3, src1=4       R3 = SMI:0
        0x00000304, # 10: OP_RET      dest=3               return SMI:0
        #  --- recursive case: self is NOT the target ---
        0x00000301, # 11: OP_LD_REG   dest=3, src1=0       R3 = R0
        0x03010002, # 12: OP_SEND     argc=0, sel=1("executeCleanup"), rcvr=3
        0x00000301, # 13: OP_LD_REG   dest=3, src1=0       R3 = R0
        0x00030315, # 14: OP_LD_SENDER dest=3, src1=3      R3 = sender(R3)  [stratified: NOT a reflective send]
        0x00010401, # 15: OP_LD_REG   dest=4, src1=1       R4 = R1  (target)
        0x00020501, # 16: OP_LD_REG   dest=5, src1=2       R5 = R2  (value)
        0x03030202, # 17: OP_SEND     argc=2, sel=3("unwindTo:value:"), rcvr=3
        0x00000304, # 18: OP_RET      dest=3
    ],
}


def add_unwind_to_value(solidify):
    """Install unwindTo:value: into a SolidifyCompiler-like object.

    Passes all fields from UNWIND_TO_VALUE directly; does not import
    solidify or ns_compile_oevm (avoids circular imports).
    """
    solidify.add_method(**UNWIND_TO_VALUE)

#!/usr/bin/env python3
"""Parity guard: assert that the C copy of unwindTo:value: in
oevm/src/kernel_boot.c matches the Python source of truth in
tools/kernel_methods.py (UNWIND_TO_VALUE).

The C file cannot share the Python source directly, so this script is the
guard that would have caught the Task-3 drift between Python and C copies.

Exit 0 on match; exit 1 on any mismatch (prints the first differing index
and word values so a developer can identify the discrepancy immediately).
"""

import re
import sys
import os

# Locate kernel_boot.c relative to this script: tools/ -> .. -> oevm/src/
_here = os.path.dirname(os.path.abspath(__file__))
_KERNEL_BOOT_C = os.path.join(_here, "..", "..", "oevm", "src", "kernel_boot.c")
_KERNEL_BOOT_C = os.path.normpath(_KERNEL_BOOT_C)

# Import the single source of truth.
sys.path.insert(0, _here)
from kernel_methods import UNWIND_TO_VALUE


def _parse_c_insts(src):
    """Extract the hex words from Method_unwindTo_value__insts[]."""
    m = re.search(
        r'Method_unwindTo_value__insts\[\]\s*=\s*\{([^}]*)\}',
        src, re.DOTALL
    )
    if not m:
        raise ValueError("Could not find Method_unwindTo_value__insts[] in kernel_boot.c")
    body = m.group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.DOTALL)
    body = re.sub(r'//[^\n]*', '', body)
    words = [int(tok, 16) for tok in re.findall(r'0x[0-9A-Fa-f]+', body)]
    return words


def _parse_c_const_count(src):
    """Extract constant_count from the __compiled struct (the 6th initialiser field)."""
    # The compiled struct initialiser is:
    #   Method_unwindTo_value__compiled = {
    #       insts, instruction_count,
    #       num_registers, num_arguments,
    #       consts_ptr, constant_count,
    #       access_modifier
    #   };
    m = re.search(
        r'Method_unwindTo_value__compiled\s*=\s*\{'
        r'\s*[^,]+,\s*(\d+),'   # insts_ptr, instruction_count
        r'\s*(\d+),\s*(\d+),'   # num_registers, num_arguments
        r'\s*[^,]+,\s*(\d+),',  # consts_ptr, constant_count
        src, re.DOTALL
    )
    if not m:
        raise ValueError("Could not parse Method_unwindTo_value__compiled struct in kernel_boot.c")
    instruction_count = int(m.group(1))
    constant_count = int(m.group(4))
    return instruction_count, constant_count


def _parse_c_consts_count(src):
    """Count the initialisers in Method_unwindTo_value__consts[]."""
    m = re.search(
        r'Method_unwindTo_value__consts\[\]\s*=\s*\{([^}]*)\}',
        src, re.DOTALL
    )
    if not m:
        raise ValueError("Could not find Method_unwindTo_value__consts[] in kernel_boot.c")
    body = m.group(1)
    # Count comma-separated non-empty entries (cast expressions like `(oop)&...`)
    entries = [e.strip() for e in body.split(',') if e.strip()]
    return len(entries)


def main():
    if not os.path.exists(_KERNEL_BOOT_C):
        print(f"FAIL: kernel_boot.c not found at {_KERNEL_BOOT_C}", file=sys.stderr)
        sys.exit(1)

    with open(_KERNEL_BOOT_C) as f:
        src = f.read()

    expected_insts = UNWIND_TO_VALUE["instructions"]
    expected_const_count = len(UNWIND_TO_VALUE["constants"])
    expected_inst_count = len(expected_insts)

    try:
        c_insts = _parse_c_insts(src)
        c_inst_count, c_const_count = _parse_c_const_count(src)
        c_consts_n = _parse_c_consts_count(src)
    except ValueError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)

    # --- instructions ---
    if len(c_insts) != len(expected_insts):
        print(
            f"FAIL: instruction count mismatch: C has {len(c_insts)}, "
            f"Python has {len(expected_insts)}",
            file=sys.stderr,
        )
        sys.exit(1)
    for i, (c_word, py_word) in enumerate(zip(c_insts, expected_insts)):
        if c_word != py_word:
            print(
                f"FAIL: instruction[{i}] mismatch: C=0x{c_word:08X}, "
                f"Python=0x{py_word:08X}",
                file=sys.stderr,
            )
            sys.exit(1)

    # --- compiled struct sanity: instruction_count + constant_count ---
    if c_inst_count != expected_inst_count:
        print(
            f"FAIL: __compiled instruction_count is {c_inst_count}, "
            f"expected {expected_inst_count}",
            file=sys.stderr,
        )
        sys.exit(1)
    if c_const_count != expected_const_count:
        print(
            f"FAIL: __compiled constant_count is {c_const_count}, "
            f"expected {expected_const_count}",
            file=sys.stderr,
        )
        sys.exit(1)

    # --- consts array entry count ---
    if c_consts_n != expected_const_count:
        print(
            f"FAIL: __consts[] has {c_consts_n} entries, "
            f"expected {expected_const_count}",
            file=sys.stderr,
        )
        sys.exit(1)

    print("OK: kernel_boot.c unwindTo:value: matches the Python source of truth")
    sys.exit(0)


if __name__ == "__main__":
    main()

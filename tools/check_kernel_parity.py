#!/usr/bin/env python3
"""Retired parity guard.

This script used to assert that the C copy of `unwindTo:value:` in
oevm/src/kernel_boot.c matched the Python source of truth in
tools/kernel_methods.py (UNWIND_TO_VALUE). Milestone A Task 2 removed the
NLR mechanism from the compiler: it no longer installs `unwindTo:value:`
into solidified images, and tools/kernel_methods.py has been deleted. The
C copy in kernel_boot.c is deleted in a later task (VM-side NLR removal).

There is nothing left to keep in parity, so this check is a no-op kept only
so any existing invocation (CI, docs) keeps working and reports success
rather than failing with an ImportError.
"""

import sys


def main():
    print("OK: unwindTo:value: parity check retired (no-NLR model, Milestone A)")
    sys.exit(0)


if __name__ == "__main__":
    main()

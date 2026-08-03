# rete

An embedded fleet system: **nodus** (the device end of a protocol) and **porta**
(what orchestrates them), with the wire contract they share in `protocol/`.

    porta/       the orchestrator (Go)
    nodus/       the device end
      tuvm/      the VM (C) + Zephyr board ports
      nsl/       the dialect, compiler and kernel
      tests/     the nsl suite
    protocol/    the wire contract
    review/      the program ladder and claims ledger

The two halves are coupled only over the wire; they never import each other's code.

Gates:

    cd nodus/nsl && python3 tools/run_tests_oevm.py     # 239 passed, 0 failed
    cd porta && go test ./...
    cd nodus/tuvm && bash tests/porta_sync_host.sh      # PORTA SYNC OK

This is not a quickstart yet — see #79.

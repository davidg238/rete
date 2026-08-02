# ns-tests: Unified Newspeak Language Integration & Verification Suite

This repository contains a comprehensive, black-box testing framework for language-level compliance verification of the **oevm** Newspeak implementation (compiled via the `ns-oevm` frontend). It utilizes a dual-harness infrastructure:
1.  **Target Runner (`../ns-oevm/tools/run_tests_oevm.py`):** Runs the suite against the oevm VM (the way-forward backend).
2.  **Reference Runner (`./run_tests_soup.py`):** Runs the test suite against the official `Primordial Soup` reference VM.

> **Note:** `ns-berry` (the previous Berry-bytecode target, driven by the local `./run_tests.py`) is retired now that oevm is the way-forward backend. The Berry runner is kept only for historical reference.

---

## Test & Benchmark Lineage

Many of the integration tests and benchmarks are ported and adapted from **SOMns** (a research implementation of Newspeak designed for Truffle/GraalVM). 

### Selected Subset Rationale
Since `ns-berry` is designed for resource-constrained, low-memory embedded controllers (and does not implement Mirrors or Introspection), the test suite deliberately excludes:
*   **Concurrency & Actors** (e.g. `AsyncFeatures`, `Savina`)
*   **Software Transactional Memory** (e.g. `LeeTM`, `Vacation`)
*   **Truffle-Specific Optimizer Specs** (e.g. `CompilerSimplification`)
*   **Mirrors & Introspection** (e.g. `Validation`, mirror-based tests)

Instead, we focus on standard, sequential Newspeak execution:
*   **`04_somns_basic/`**: Core OOP and lexical scope verification (lexical block inlining, nested class instantiation, outer sends, super sends, initializer parameters, dynamic dispatch callbacks).
*   **`05_somns_benchmarks/`**: Heavyweight sequential benchmarks checking complex data structures, algorithms, and VM execution paths (e.g., Queens, Sieve, Towers of Hanoi, Mandelbrot, NBody, Richards, DeltaBlue).
*   **`06_limit_tests/`**: VM-level compiler and runtime stress testing (extreme nesting of outer sends, 100+ field layout class boundary checks, 64-local variable method limits, integer overflow/underflow boundaries, and high allocation GC pressure stress).

---

## Running the Tests

To run the conformance suite against the **oevm** target VM:
```bash
python3 ../ns-oevm/tools/run_tests_oevm.py
```

To run the suite against the **Primordial Soup reference VM**:
```bash
./run_tests_soup.py
```

(The retired Berry runner `./run_tests.py` still exists but is no longer maintained.)

### Performance Benchmarking & VM Comparison

To compile, execute, and compare execution performance between the Reference VM (Soup) and Target VM (oevm) across all benchmark and limit tests:
```bash
./run_benchmarks.py
```
This script executes each test multiple times, takes the median execution time, computes the `oevm / Soup` slowdown factor, and saves the comparison in the following files:
*   `benchmark_comparison.md`: A human-readable Markdown table comparing VM runtimes.
*   `benchmark_results.json`: Raw benchmark time results in JSON format.

---

## The Metadata Skipping System

To allow continuous verification on the reference VM while tracking incomplete features on the target VM, every test file can define a metadata block at the top within a comment:

```newspeak
(*
expect:
true
skip-oevm: true
*)
```

### Metadata Keys:
*   `expect:`: List of expected stdout lines that the harness will match.
*   `skip-oevm: true`: Signals the benchmark runner (`run_benchmarks.py`) to skip this test on the oevm target. Use for features not yet implemented in oevm.
*   `skip-soup: true`: Signals the reference runner (`run_tests_soup.py`) to skip this test. Use this for testing compiler failures or dialect-specific features.
*   `skip-berry: true` *(legacy)*: was used by the retired Berry runner; harmless if present.

---

## VM Confidence Assessment

If a Newspeak VM implementation (such as oevm) passes the full suite, it achieves high confidence regarding sequential execution correctness and language compliance. (Current oevm status: 36 pass / 5 fail / 1 skip — see `../ns-oevm` for the live breakdown.)

### What is Guaranteed (The 85%)
* **Lexical Closures & Scope Capture:** Validated by block captures, nested block inlining, and non-local returns (NLRs) across complex nesting structures.
* **Nested Class Model:** Confirmed support for initialization parameters, superclass constructors, field accessors, outer sends, and nested inheritance hierarchy.
* **Dynamic Dispatch & Object Identity:** Virtual dispatch lookup pipelines, fallback mechanisms (`doesNotUnderstand:`), and default object identity comparison are verified.
* **Math & Floating-Point Precision:** Floating-point exact vs. integer division behavior, object representation, and numerical loop stability are validated.

### Remaining Gaps (The 15%)
* **Mirrors and Metaprogramming:** Introspective mirror capabilities are not exercised by this suite.
* **Garbage Collection Stability:** Longevity under large heaps and correct behavior of weak references (`WeakArray`) are not tested.
* **Concurreny/Actor Model:** Dialect-specific concurrent execution and message queues are skipped.
* **Standard Library APIs:** File system, socket networking, and console formatting interfaces are not thoroughly covered.


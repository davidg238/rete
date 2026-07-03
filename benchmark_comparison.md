# Newspeak VM Performance Comparison
Generated on: 2026-06-18 16:13:04
Runs per test: 5 (median time reported)

| Group | Benchmark / Test | Reference (Soup) Time | Target (oevm) Time | oevm / Soup |
|---|---|---|---|---|
| 05_somns_benchmarks | 01_queens.ns | 0.002550s | 0.002250s | 0.88x |
| 05_somns_benchmarks | 02_sieve.ns | 0.001761s | 0.001418s | 0.81x |
| 05_somns_benchmarks | 03_towers.ns | 0.003784s | 0.004196s | 1.11x |
| 05_somns_benchmarks | 04_mandelbrot.ns | 2.729796s | 3.889935s | 1.42x |
| 05_somns_benchmarks | 05_nbody.ns | 0.008662s | 0.014073s | 1.62x |
| 05_somns_benchmarks | 06_richards.ns | 0.042069s | 0.059814s | 1.42x |
| 05_somns_benchmarks | 07_deltablue.ns | 0.002498s | 0.001625s | 0.65x |
| 06_limit_tests | 01_deep_nesting.ns | 0.001419s | 0.000369s | 0.26x |
| 06_limit_tests | 02_large_class.ns | 0.005167s | 0.000388s | 0.08x |
| 06_limit_tests | 03_many_locals.ns | 0.001457s | 0.001110s | 0.76x |
| 06_limit_tests | 04_integer_overflow.ns | 0.001331s | 0.000616s | 0.46x |
| 06_limit_tests | 05_gc_stress.ns | 0.012317s | 0.019669s | 1.60x |
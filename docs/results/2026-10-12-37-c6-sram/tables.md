### c6spare, AWFY

| row (ms) | flash ×1 | SRAM ×1 | SRAM ÷ flash |
|---|--:|--:|--:|
| Bounce | 78 | 77 | 0.987 |
| List | 332 | 337 | 1.015 |
| Permute | 183 | 184 | 1.005 |
| Richards | 5816 | 5882 | 1.011 |
| Queens | 181 | 182 | 1.006 |
| Sieve | 74 | 74 | 1.000 |
| Towers | 530 | 449 | 0.847 |
| CD | 17917 | 14872 | 0.830 |
| DeltaBlue | 716 | 634 | 0.885 |

Geomean SRAM ÷ flash over 9 rows (≥ 50 ms): 0.951

### c6spare, STIC

| row (ms) | flash ×1 | SRAM ×1 | SRAM ÷ flash |
|---|--:|--:|--:|
| alloc | 11795 | 11946 | 1.013 |
| arrayWrite | 17968 | 18006 | 1.002 |
| dictionaryWrite | 6082 | 6048 | 0.994 |
| floatMath | 23328 | 23450 | 1.005 |
| integerMath | 3692 | 3733 | 1.011 |
| collectionIterate | 10646 | 10647 | 1.000 |
| collectionWrite | 14283 | 13521 | 0.947 |
| stringCompare | 2786 | 2796 | 1.004 |

Geomean SRAM ÷ flash over 8 rows (≥ 50 ms): 0.997


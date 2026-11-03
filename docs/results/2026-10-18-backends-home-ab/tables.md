### awfy: move ÷ pre

| row (ms) | pre 1 | pre 2 | move 1 | move 2 | move ÷ pre |
|---|--:|--:|--:|--:|--:|
| Bounce | 74 | 74 | 74 | 74 | 1.000 |
| CD | 13369 | 13368 | 13367 | 13370 | 1.000 |
| DeltaBlue | 569 | 568 | 568 | 569 | 1.000 |
| List | 282 | 282 | 282 | 282 | 1.000 |
| Permute | 173 | 173 | 173 | 173 | 1.000 |
| Queens | 160 | 160 | 160 | 160 | 1.000 |
| Richards | 5221 | 5221 | 5221 | 5221 | 1.000 |
| Sieve | 70 | 70 | 70 | 70 | 1.000 |
| Towers | 394 | 394 | 394 | 394 | 1.000 |

Geomean over 9 rows (≥ 50 ms): 1.000

### performance: move ÷ pre

| row (ms) | pre 1 | pre 2 | move 1 | move 2 | move ÷ pre |
|---|--:|--:|--:|--:|--:|
| alloc | 11321 | 11321 | 11321 | 11321 | 1.000 |
| arrayWrite | 16699 | 16699 | 16699 | 16699 | 1.000 |
| collectionIterate | 10668 | 10668 | 10668 | 10668 | 1.000 |
| collectionWrite | 11940 | 11940 | 11940 | 11940 | 1.000 |
| dictionaryWrite | 5219 | 5219 | 5220 | 5219 | 1.000 |
| floatMath | 23216 | 23216 | 23216 | 23216 | 1.000 |
| hanoi8 | 2 | 2 | 2 | 2 | 1.000 |
| integerMath | 3649 | 3649 | 3649 | 3649 | 1.000 |
| stringCompare | 2161 | 2161 | 2161 | 2161 | 1.000 |

Geomean over 8 rows (≥ 50 ms): 1.000


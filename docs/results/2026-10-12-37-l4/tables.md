### c6spare, AWFY, board profile — L4(c) against master

| row (ms) | master ×1 | L4(c) ×1 | L4(c) ÷ master |
|---|--:|--:|--:|
| Bounce | 77 | 76 | 0.987 |
| List | 337 | 333 | 0.988 |
| Permute | 184 | 182 | 0.989 |
| Richards | 5882 | 5810 | 0.988 |
| Queens | 182 | 180 | 0.989 |
| Sieve | 74 | 74 | 1.000 |
| Towers | 449 | 443 | 0.987 |
| CD | 14872 | 14670 | 0.986 |
| DeltaBlue | 634 | 619 | 0.976 |

Geomean L4(c) ÷ master over 9 rows (≥ 50 ms): 0.988

### c6spare, AWFY, board profile — L3-lite against L4(c)

| row (ms) | L4(c) ×1 | L3-lite ×1 | L3-lite ÷ L4(c) |
|---|--:|--:|--:|
| Bounce | 76 | 76 | 1.000 |
| List | 333 | 331 | 0.994 |
| Permute | 182 | 183 | 1.005 |
| Richards | 5810 | 5809 | 1.000 |
| Queens | 180 | 181 | 1.006 |
| Sieve | 74 | 74 | 1.000 |
| Towers | 443 | 442 | 0.998 |
| CD | 14670 | 14694 | 1.002 |
| DeltaBlue | 619 | 618 | 0.998 |

Geomean L3-lite ÷ L4(c) over 9 rows (≥ 50 ms): 1.000

### c6spare, STIC — L4(c) against master

| row (ms) | master ×1 | L4(c) ×1 | L4(c) ÷ master |
|---|--:|--:|--:|
| alloc | 11946 | 11965 | 1.002 |
| arrayWrite | 18006 | 18030 | 1.001 |
| dictionaryWrite | 6048 | 5978 | 0.988 |
| floatMath | 23450 | 23217 | 0.990 |
| integerMath | 3734 | 3578 | 0.958 |
| collectionIterate | 10647 | 10941 | 1.028 |
| collectionWrite | 13522 | 13475 | 0.997 |
| stringCompare | 2796 | 2778 | 0.994 |

Geomean L4(c) ÷ master over 8 rows (≥ 50 ms): 0.995

### c6spare, STIC — L3-lite against L4(c)

| row (ms) | L4(c) ×1 | L3-lite ×1 | L3-lite ÷ L4(c) |
|---|--:|--:|--:|
| alloc | 11965 | 11916 | 0.996 |
| arrayWrite | 18030 | 18043 | 1.001 |
| dictionaryWrite | 5978 | 5990 | 1.002 |
| floatMath | 23217 | 23147 | 0.997 |
| integerMath | 3578 | 3576 | 0.999 |
| collectionIterate | 10941 | 10926 | 0.999 |
| collectionWrite | 13475 | 13450 | 0.998 |
| stringCompare | 2778 | 2783 | 1.002 |

Geomean L3-lite ÷ L4(c) over 8 rows (≥ 50 ms): 0.999

### c6b, AWFY, board profile — L4(c) against master

| row (ms) | master ×1 | L4(c) ×1 | L4(c) ÷ master |
|---|--:|--:|--:|
| Bounce | 77 | 76 | 0.987 |
| List | 337 | 333 | 0.988 |
| Permute | 183 | 182 | 0.995 |
| Richards | 5883 | 5810 | 0.988 |
| Queens | 181 | 180 | 0.994 |
| Sieve | 75 | 75 | 1.000 |
| Towers | 449 | 443 | 0.987 |
| CD | 14872 | 14670 | 0.986 |
| DeltaBlue | 634 | 619 | 0.976 |

Geomean L4(c) ÷ master over 9 rows (≥ 50 ms): 0.989

### c6b, AWFY, board profile — L3-lite against L4(c)

| row (ms) | L4(c) ×1 | L3-lite ×1 | L3-lite ÷ L4(c) |
|---|--:|--:|--:|
| Bounce | 76 | 76 | 1.000 |
| List | 333 | 331 | 0.994 |
| Permute | 182 | 182 | 1.000 |
| Richards | 5810 | 5809 | 1.000 |
| Queens | 180 | 181 | 1.006 |
| Sieve | 75 | 74 | 0.987 |
| Towers | 443 | 442 | 0.998 |
| CD | 14670 | 14695 | 1.002 |
| DeltaBlue | 619 | 619 | 1.000 |

Geomean L3-lite ÷ L4(c) over 9 rows (≥ 50 ms): 0.998

### c6b, STIC — L4(c) against master

| row (ms) | master ×1 | L4(c) ×1 | L4(c) ÷ master |
|---|--:|--:|--:|
| alloc | 11946 | 11966 | 1.002 |
| arrayWrite | 18005 | 18031 | 1.001 |
| dictionaryWrite | 6047 | 5979 | 0.989 |
| floatMath | 23450 | 23216 | 0.990 |
| integerMath | 3734 | 3578 | 0.958 |
| collectionIterate | 10647 | 10941 | 1.028 |
| collectionWrite | 13521 | 13476 | 0.997 |
| stringCompare | 2796 | 2778 | 0.994 |

Geomean L4(c) ÷ master over 8 rows (≥ 50 ms): 0.995

### c6b, STIC — L3-lite against L4(c)

| row (ms) | L4(c) ×1 | L3-lite ×1 | L3-lite ÷ L4(c) |
|---|--:|--:|--:|
| alloc | 11966 | 11916 | 0.996 |
| arrayWrite | 18031 | 18044 | 1.001 |
| dictionaryWrite | 5979 | 5989 | 1.002 |
| floatMath | 23216 | 23147 | 0.997 |
| integerMath | 3578 | 3577 | 1.000 |
| collectionIterate | 10941 | 10926 | 0.999 |
| collectionWrite | 13476 | 13451 | 0.998 |
| stringCompare | 2778 | 2782 | 1.001 |

Geomean L3-lite ÷ L4(c) over 8 rows (≥ 50 ms): 0.999

### weact-1-5, AWFY, board profile — L4(c) against master

| row (ms) | master ×1 | L4(c) ×1 | L4(c) ÷ master |
|---|--:|--:|--:|
| Bounce | 78 | 76 | 0.974 |
| List | 356 | 352 | 0.989 |
| Permute | 187 | 183 | 0.979 |
| Richards | 6459 | 6380 | 0.988 |
| Queens | 182 | 178 | 0.978 |
| Sieve | 69 | 67 | 0.971 |
| Towers | 488 | 482 | 0.988 |
| CD | 18395 | 18226 | 0.991 |
| DeltaBlue | 694 | 678 | 0.977 |

Geomean L4(c) ÷ master over 9 rows (≥ 50 ms): 0.982

### weact-1-5, AWFY, board profile — L3-lite against L4(c)

| row (ms) | L4(c) ×1 | L3-lite ×1 | L3-lite ÷ L4(c) |
|---|--:|--:|--:|
| Bounce | 76 | 79 | 1.039 |
| List | 352 | 355 | 1.009 |
| Permute | 183 | 188 | 1.027 |
| Richards | 6380 | 6484 | 1.016 |
| Queens | 178 | 184 | 1.034 |
| Sieve | 67 | 70 | 1.045 |
| Towers | 482 | 489 | 1.015 |
| CD | 18226 | 18392 | 1.009 |
| DeltaBlue | 678 | 689 | 1.016 |

Geomean L3-lite ÷ L4(c) over 9 rows (≥ 50 ms): 1.023

### weact-1-5, STIC — L4(c) against master

| row (ms) | master ×1 | L4(c) ×1 | L4(c) ÷ master |
|---|--:|--:|--:|
| alloc | 14204 | 14108 | 0.993 |
| arrayWrite | 17252 | 16499 | 0.956 |
| dictionaryWrite | 6320 | 6219 | 0.984 |
| floatMath | 24180 | 24019 | 0.993 |
| integerMath | 3073 | 2920 | 0.950 |
| collectionIterate | 13209 | 12840 | 0.972 |
| collectionWrite | 15171 | 14955 | 0.986 |
| stringCompare | 2850 | 2819 | 0.989 |

Geomean L4(c) ÷ master over 8 rows (≥ 50 ms): 0.978

### weact-1-5, STIC — L3-lite against L4(c)

| row (ms) | L4(c) ×1 | L3-lite ×1 | L3-lite ÷ L4(c) |
|---|--:|--:|--:|
| alloc | 14108 | 14232 | 1.009 |
| arrayWrite | 16499 | 17746 | 1.076 |
| dictionaryWrite | 6219 | 6370 | 1.024 |
| floatMath | 24019 | 24245 | 1.009 |
| integerMath | 2920 | 3145 | 1.077 |
| collectionIterate | 12840 | 13659 | 1.064 |
| collectionWrite | 14955 | 15211 | 1.017 |
| stringCompare | 2819 | 2879 | 1.021 |

Geomean L3-lite ÷ L4(c) over 8 rows (≥ 50 ms): 1.037

### weact-1-7, AWFY, board profile — L4(c) against master

| row (ms) | master ×1 | L4(c) ×1 | L4(c) ÷ master |
|---|--:|--:|--:|
| Bounce | 78 | 76 | 0.974 |
| List | 356 | 352 | 0.989 |
| Permute | 187 | 183 | 0.979 |
| Richards | 6459 | 6380 | 0.988 |
| Queens | 182 | 178 | 0.978 |
| Sieve | 69 | 66 | 0.957 |
| Towers | 488 | 482 | 0.988 |
| CD | 18396 | 18226 | 0.991 |
| DeltaBlue | 694 | 679 | 0.978 |

Geomean L4(c) ÷ master over 9 rows (≥ 50 ms): 0.980

### weact-1-7, AWFY, board profile — L3-lite against L4(c)

| row (ms) | L4(c) ×1 | L3-lite ×1 | L3-lite ÷ L4(c) |
|---|--:|--:|--:|
| Bounce | 76 | 79 | 1.039 |
| List | 352 | 354 | 1.006 |
| Permute | 183 | 188 | 1.027 |
| Richards | 6380 | 6484 | 1.016 |
| Queens | 178 | 184 | 1.034 |
| Sieve | 66 | 70 | 1.061 |
| Towers | 482 | 490 | 1.017 |
| CD | 18226 | 18390 | 1.009 |
| DeltaBlue | 679 | 688 | 1.013 |

Geomean L3-lite ÷ L4(c) over 9 rows (≥ 50 ms): 1.025

### weact-1-7, STIC — L4(c) against master

| row (ms) | master ×1 | L4(c) ×1 | L4(c) ÷ master |
|---|--:|--:|--:|
| alloc | 14204 | 14108 | 0.993 |
| arrayWrite | 17252 | 16499 | 0.956 |
| dictionaryWrite | 6321 | 6219 | 0.984 |
| floatMath | 24180 | 24019 | 0.993 |
| integerMath | 3073 | 2920 | 0.950 |
| collectionIterate | 13209 | 12840 | 0.972 |
| collectionWrite | 15171 | 14955 | 0.986 |
| stringCompare | 2849 | 2819 | 0.989 |

Geomean L4(c) ÷ master over 8 rows (≥ 50 ms): 0.978

### weact-1-7, STIC — L3-lite against L4(c)

| row (ms) | L4(c) ×1 | L3-lite ×1 | L3-lite ÷ L4(c) |
|---|--:|--:|--:|
| alloc | 14108 | 14232 | 1.009 |
| arrayWrite | 16499 | 17746 | 1.076 |
| dictionaryWrite | 6219 | 6370 | 1.024 |
| floatMath | 24019 | 24245 | 1.009 |
| integerMath | 2920 | 3145 | 1.077 |
| collectionIterate | 12840 | 13659 | 1.064 |
| collectionWrite | 14955 | 15211 | 1.017 |
| stringCompare | 2819 | 2879 | 1.021 |

Geomean L3-lite ÷ L4(c) over 8 rows (≥ 50 ms): 1.037


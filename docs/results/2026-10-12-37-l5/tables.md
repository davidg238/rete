## run2: master against L5 as merged (`aad8a67b`, l5b)

### c6spare, AWFY, board profile

| row (ms) | master ×1 | L5 ×1 | L5 ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 75 | 0.987 |
| List | 333 | 326 | 0.979 |
| Permute | 182 | 179 | 0.984 |
| Richards | 5809 | 5693 | 0.980 |
| Queens | 181 | 178 | 0.983 |
| Sieve | 74 | 74 | 1.000 |
| Towers | 443 | 436 | 0.984 |
| CD | 14670 | 14454 | 0.985 |
| DeltaBlue | 619 | 603 | 0.974 |

Geomean L5 ÷ master over 9 rows (≥ 50 ms): 0.984

### c6spare, STIC

| row (ms) | master ×1 | L5 ×1 | L5 ÷ master |
|---|--:|--:|--:|
| alloc | 11965 | 11690 | 0.977 |
| arrayWrite | 18030 | 18031 | 1.000 |
| dictionaryWrite | 5978 | 5892 | 0.986 |
| floatMath | 23217 | 23216 | 1.000 |
| integerMath | 3578 | 3578 | 1.000 |
| collectionIterate | 10941 | 10940 | 1.000 |
| collectionWrite | 13475 | 13155 | 0.976 |
| stringCompare | 2778 | 2655 | 0.956 |

Geomean L5 ÷ master over 8 rows (≥ 50 ms): 0.987

### c6b, AWFY, board profile

| row (ms) | master ×1 | L5 ×1 | L5 ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 76 | 1.000 |
| List | 332 | 327 | 0.985 |
| Permute | 182 | 179 | 0.984 |
| Richards | 5809 | 5693 | 0.980 |
| Queens | 180 | 178 | 0.989 |
| Sieve | 74 | 74 | 1.000 |
| Towers | 444 | 436 | 0.982 |
| CD | 14669 | 14454 | 0.985 |
| DeltaBlue | 620 | 603 | 0.973 |

Geomean L5 ÷ master over 9 rows (≥ 50 ms): 0.986

### c6b, STIC

| row (ms) | master ×1 | L5 ×1 | L5 ÷ master |
|---|--:|--:|--:|
| alloc | 11965 | 11690 | 0.977 |
| arrayWrite | 18031 | 18031 | 1.000 |
| dictionaryWrite | 5979 | 5891 | 0.985 |
| floatMath | 23216 | 23216 | 1.000 |
| integerMath | 3578 | 3578 | 1.000 |
| collectionIterate | 10940 | 10941 | 1.000 |
| collectionWrite | 13475 | 13154 | 0.976 |
| stringCompare | 2778 | 2654 | 0.955 |

Geomean L5 ÷ master over 8 rows (≥ 50 ms): 0.987

### weact-1-5, AWFY, board profile

| row (ms) | master ×1 | L5 ×1 | L5 ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 75 | 0.987 |
| List | 352 | 332 | 0.943 |
| Permute | 183 | 178 | 0.973 |
| Richards | 6380 | 5963 | 0.935 |
| Queens | 179 | 173 | 0.966 |
| Sieve | 66 | 66 | 1.000 |
| Towers | 482 | 458 | 0.950 |
| CD | 18227 | 17553 | 0.963 |
| DeltaBlue | 678 | 647 | 0.954 |

Geomean L5 ÷ master over 9 rows (≥ 50 ms): 0.963

### weact-1-5, STIC

| row (ms) | master ×1 | L5 ×1 | L5 ÷ master |
|---|--:|--:|--:|
| alloc | 14108 | 13587 | 0.963 |
| arrayWrite | 16499 | 16499 | 1.000 |
| dictionaryWrite | 6219 | 5991 | 0.963 |
| floatMath | 24019 | 24018 | 1.000 |
| integerMath | 2920 | 2919 | 1.000 |
| collectionIterate | 12840 | 12840 | 1.000 |
| collectionWrite | 14955 | 14486 | 0.969 |
| stringCompare | 2819 | 2633 | 0.934 |

Geomean L5 ÷ master over 8 rows (≥ 50 ms): 0.978

### weact-1-7, AWFY, board profile

| row (ms) | master ×1 | L5 ×1 | L5 ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 74 | 0.974 |
| List | 352 | 332 | 0.943 |
| Permute | 183 | 178 | 0.973 |
| Richards | 6381 | 5963 | 0.934 |
| Queens | 178 | 173 | 0.972 |
| Sieve | 66 | 66 | 1.000 |
| Towers | 481 | 458 | 0.952 |
| CD | 18227 | 17553 | 0.963 |
| DeltaBlue | 679 | 648 | 0.954 |

Geomean L5 ÷ master over 9 rows (≥ 50 ms): 0.963

### weact-1-7, STIC

| row (ms) | master ×1 | L5 ×1 | L5 ÷ master |
|---|--:|--:|--:|
| alloc | 14108 | 13587 | 0.963 |
| arrayWrite | 16499 | 16499 | 1.000 |
| dictionaryWrite | 6219 | 5991 | 0.963 |
| floatMath | 24019 | 24018 | 1.000 |
| integerMath | 2919 | 2919 | 1.000 |
| collectionIterate | 12840 | 12840 | 1.000 |
| collectionWrite | 14956 | 14486 | 0.969 |
| stringCompare | 2819 | 2632 | 0.934 |

Geomean L5 ÷ master over 8 rows (≥ 50 ms): 0.978

## run1: master against L5 first cut (`85525dba`, +844 B) and the one-word arena header (`38c9b557`)

### c6spare, AWFY, board profile

| row (ms) | master ×1 | L5 first cut ×1 | L5 first cut ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 75 | 0.987 |
| List | 333 | 320 | 0.961 |
| Permute | 182 | 179 | 0.984 |
| Richards | 5810 | 5620 | 0.967 |
| Queens | 181 | 176 | 0.972 |
| Sieve | 74 | 75 | 1.014 |
| Towers | 443 | 428 | 0.966 |
| CD | 14670 | 14298 | 0.975 |
| DeltaBlue | 619 | 603 | 0.974 |

Geomean L5 first cut ÷ master over 9 rows (≥ 50 ms): 0.978

| row (ms) | master ×1 | header ×1 | header ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 76 | 1.000 |
| List | 333 | 333 | 1.000 |
| Permute | 182 | 182 | 1.000 |
| Richards | 5810 | 5830 | 1.003 |
| Queens | 181 | 181 | 1.000 |
| Sieve | 74 | 74 | 1.000 |
| Towers | 443 | 445 | 1.005 |
| CD | 14670 | 14727 | 1.004 |
| DeltaBlue | 619 | 617 | 0.997 |

Geomean header ÷ master over 9 rows (≥ 50 ms): 1.001

### c6spare, STIC

| row (ms) | master ×1 | L5 first cut ×1 | L5 first cut ÷ master |
|---|--:|--:|--:|
| alloc | 11965 | 11653 | 0.974 |
| arrayWrite | 18030 | 18031 | 1.000 |
| dictionaryWrite | 5978 | 5852 | 0.979 |
| floatMath | 23217 | 23217 | 1.000 |
| integerMath | 3578 | 3578 | 1.000 |
| collectionIterate | 10941 | 10941 | 1.000 |
| collectionWrite | 13475 | 13093 | 0.972 |
| stringCompare | 2779 | 2546 | 0.916 |

Geomean L5 first cut ÷ master over 8 rows (≥ 50 ms): 0.980

| row (ms) | master ×1 | header ×1 | header ÷ master |
|---|--:|--:|--:|
| alloc | 11965 | 11985 | 1.002 |
| arrayWrite | 18030 | 18031 | 1.000 |
| dictionaryWrite | 5978 | 5986 | 1.001 |
| floatMath | 23217 | 23241 | 1.001 |
| integerMath | 3578 | 3578 | 1.000 |
| collectionIterate | 10941 | 10940 | 1.000 |
| collectionWrite | 13475 | 13411 | 0.995 |
| stringCompare | 2779 | 2797 | 1.006 |

Geomean header ÷ master over 8 rows (≥ 50 ms): 1.001

### c6b, AWFY, board profile

| row (ms) | master ×1 | L5 first cut ×1 | L5 first cut ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 75 | 0.987 |
| List | 333 | 320 | 0.961 |
| Permute | 182 | 179 | 0.984 |
| Richards | 5809 | 5621 | 0.968 |
| Queens | 181 | 176 | 0.972 |
| Sieve | 74 | 75 | 1.014 |
| Towers | 443 | 428 | 0.966 |
| CD | 14670 | 14299 | 0.975 |
| DeltaBlue | 619 | 602 | 0.973 |

Geomean L5 first cut ÷ master over 9 rows (≥ 50 ms): 0.977

| row (ms) | master ×1 | header ×1 | header ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 76 | 1.000 |
| List | 333 | 333 | 1.000 |
| Permute | 182 | 182 | 1.000 |
| Richards | 5809 | 5830 | 1.004 |
| Queens | 181 | 180 | 0.994 |
| Sieve | 74 | 74 | 1.000 |
| Towers | 443 | 445 | 1.005 |
| CD | 14670 | 14727 | 1.004 |
| DeltaBlue | 619 | 617 | 0.997 |

Geomean header ÷ master over 9 rows (≥ 50 ms): 1.000

### c6b, STIC

| row (ms) | master ×1 | L5 first cut ×1 | L5 first cut ÷ master |
|---|--:|--:|--:|
| alloc | 11965 | 11653 | 0.974 |
| arrayWrite | 18031 | 18031 | 1.000 |
| dictionaryWrite | 5979 | 5853 | 0.979 |
| floatMath | 23216 | 23216 | 1.000 |
| integerMath | 3578 | 3578 | 1.000 |
| collectionIterate | 10940 | 10940 | 1.000 |
| collectionWrite | 13476 | 13093 | 0.972 |
| stringCompare | 2778 | 2547 | 0.917 |

Geomean L5 first cut ÷ master over 8 rows (≥ 50 ms): 0.980

| row (ms) | master ×1 | header ×1 | header ÷ master |
|---|--:|--:|--:|
| alloc | 11965 | 11985 | 1.002 |
| arrayWrite | 18031 | 18030 | 1.000 |
| dictionaryWrite | 5979 | 5985 | 1.001 |
| floatMath | 23216 | 23241 | 1.001 |
| integerMath | 3578 | 3578 | 1.000 |
| collectionIterate | 10940 | 10940 | 1.000 |
| collectionWrite | 13476 | 13411 | 0.995 |
| stringCompare | 2778 | 2797 | 1.007 |

Geomean header ÷ master over 8 rows (≥ 50 ms): 1.001

### weact-1-5, AWFY, board profile

| row (ms) | master ×1 | L5 first cut ×1 | L5 first cut ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 74 | 0.974 |
| List | 352 | 323 | 0.918 |
| Permute | 183 | 176 | 0.962 |
| Richards | 6380 | 5849 | 0.917 |
| Queens | 178 | 170 | 0.955 |
| Sieve | 66 | 66 | 1.000 |
| Towers | 482 | 450 | 0.934 |
| CD | 18227 | 17319 | 0.950 |
| DeltaBlue | 678 | 637 | 0.940 |

Geomean L5 first cut ÷ master over 9 rows (≥ 50 ms): 0.949

| row (ms) | master ×1 | header ×1 | header ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 76 | 1.000 |
| List | 352 | 352 | 1.000 |
| Permute | 183 | 182 | 0.995 |
| Richards | 6380 | 6443 | 1.010 |
| Queens | 178 | 178 | 1.000 |
| Sieve | 66 | 66 | 1.000 |
| Towers | 482 | 470 | 0.975 |
| CD | 18227 | 18291 | 1.004 |
| DeltaBlue | 678 | 679 | 1.001 |

Geomean header ÷ master over 9 rows (≥ 50 ms): 0.998

### weact-1-5, STIC

| row (ms) | master ×1 | L5 first cut ×1 | L5 first cut ÷ master |
|---|--:|--:|--:|
| alloc | 14108 | 13487 | 0.956 |
| arrayWrite | 16499 | 16605 | 1.006 |
| dictionaryWrite | 6219 | 5923 | 0.952 |
| floatMath | 24019 | 23981 | 0.998 |
| integerMath | 2920 | 2883 | 0.987 |
| collectionIterate | 12840 | 12833 | 0.999 |
| collectionWrite | 14955 | 14349 | 0.959 |
| stringCompare | 2819 | 2540 | 0.901 |

Geomean L5 first cut ÷ master over 8 rows (≥ 50 ms): 0.969

| row (ms) | master ×1 | header ×1 | header ÷ master |
|---|--:|--:|--:|
| alloc | 14108 | 14069 | 0.997 |
| arrayWrite | 16499 | 16498 | 1.000 |
| dictionaryWrite | 6219 | 6214 | 0.999 |
| floatMath | 24019 | 24116 | 1.004 |
| integerMath | 2920 | 2919 | 1.000 |
| collectionIterate | 12840 | 12840 | 1.000 |
| collectionWrite | 14955 | 14949 | 1.000 |
| stringCompare | 2819 | 2819 | 1.000 |

Geomean header ÷ master over 8 rows (≥ 50 ms): 1.000

### weact-1-7, AWFY, board profile

| row (ms) | master ×1 | L5 first cut ×1 | L5 first cut ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 74 | 0.974 |
| List | 352 | 323 | 0.918 |
| Permute | 183 | 176 | 0.962 |
| Richards | 6380 | 5849 | 0.917 |
| Queens | 179 | 170 | 0.950 |
| Sieve | 66 | 66 | 1.000 |
| Towers | 482 | 450 | 0.934 |
| CD | 18225 | 17319 | 0.950 |
| DeltaBlue | 678 | 637 | 0.940 |

Geomean L5 first cut ÷ master over 9 rows (≥ 50 ms): 0.949

| row (ms) | master ×1 | header ×1 | header ÷ master |
|---|--:|--:|--:|
| Bounce | 76 | 76 | 1.000 |
| List | 352 | 352 | 1.000 |
| Permute | 183 | 183 | 1.000 |
| Richards | 6380 | 6444 | 1.010 |
| Queens | 179 | 178 | 0.994 |
| Sieve | 66 | 66 | 1.000 |
| Towers | 482 | 470 | 0.975 |
| CD | 18225 | 18289 | 1.004 |
| DeltaBlue | 678 | 680 | 1.003 |

Geomean header ÷ master over 9 rows (≥ 50 ms): 0.998

### weact-1-7, STIC

| row (ms) | master ×1 | L5 first cut ×1 | L5 first cut ÷ master |
|---|--:|--:|--:|
| alloc | 14108 | 13487 | 0.956 |
| arrayWrite | 16499 | 16605 | 1.006 |
| dictionaryWrite | 6219 | 5922 | 0.952 |
| floatMath | 24019 | 23981 | 0.998 |
| integerMath | 2920 | 2883 | 0.987 |
| collectionIterate | 12840 | 12833 | 0.999 |
| collectionWrite | 14955 | 14349 | 0.959 |
| stringCompare | 2819 | 2540 | 0.901 |

Geomean L5 first cut ÷ master over 8 rows (≥ 50 ms): 0.969

| row (ms) | master ×1 | header ×1 | header ÷ master |
|---|--:|--:|--:|
| alloc | 14108 | 14069 | 0.997 |
| arrayWrite | 16499 | 16498 | 1.000 |
| dictionaryWrite | 6219 | 6214 | 0.999 |
| floatMath | 24019 | 24116 | 1.004 |
| integerMath | 2920 | 2919 | 1.000 |
| collectionIterate | 12840 | 12840 | 1.000 |
| collectionWrite | 14955 | 14949 | 1.000 |
| stringCompare | 2819 | 2819 | 1.000 |

Geomean header ÷ master over 8 rows (≥ 50 ms): 1.000


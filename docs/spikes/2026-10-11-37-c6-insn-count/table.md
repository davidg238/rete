
logs/c6-pm-base.log

| row | bytecodes | cycles | native inst | CPI | native/bytecode | cycles/bytecode | ms (min–max) |
|---|--:|--:|--:|--:|--:|--:|--:|
| baseLoop | 9.0M | 457M | 328M | 1.39 | 36.5 | 50.7 | 2853–2854 |
| blockValue | 14.0M | 1116M | 819M | 1.36 | 58.5 | 79.7 | 6744–6978 |
| methodSend | 13.0M | 1164M | 871M | 1.34 | 67.0 | 89.5 | 7273–7274 |
| blockCapture | 16.0M | 1210M | 908M | 1.33 | 56.8 | 75.6 | 7454–7569 |
| closureMake | 8.0M | 631M | 477M | 1.32 | 59.7 | 78.8 | 3900–4006 |
| ocDo | 3.5M | 163M | 121M | 1.34 | 34.6 | 46.5 | 1017–1017 |
| ocAt | 26.4M | 1957M | 1420M | 1.38 | 53.8 | 74.1 | 12234–12234 |

logs/c6-pm-lean.log

| row | bytecodes | cycles | native inst | CPI | native/bytecode | cycles/bytecode | ms (min–max) |
|---|--:|--:|--:|--:|--:|--:|--:|
| baseLoop | 9.0M | 347M | 287M | 1.21 | 31.9 | 38.6 | 2170–2171 |
| blockValue | 14.0M | 959M | 748M | 1.28 | 53.5 | 68.5 | 5750–5993 |
| methodSend | 13.0M | 1005M | 803M | 1.25 | 61.8 | 77.3 | 6279–6280 |
| blockCapture | 16.0M | 1040M | 833M | 1.25 | 52.1 | 65.0 | 6391–6510 |
| closureMake | 8.0M | 549M | 446M | 1.23 | 55.8 | 68.6 | 3389–3496 |
| ocDo | 3.5M | 132M | 111M | 1.19 | 31.8 | 37.8 | 826–826 |
| ocAt | 26.4M | 1653M | 1303M | 1.27 | 49.4 | 62.6 | 10330–10330 |

| row | cycles new/old | native inst new/old | ms new/old (min) |
|---|--:|--:|--:|
| baseLoop | 0.761 | 0.875 | 0.761 |
| blockValue | 0.859 | 0.913 | 0.853 |
| methodSend | 0.863 | 0.922 | 0.863 |
| blockCapture | 0.860 | 0.917 | 0.857 |
| closureMake | 0.871 | 0.935 | 0.869 |
| ocDo | 0.812 | 0.917 | 0.812 |
| ocAt | 0.844 | 0.918 | 0.844 |

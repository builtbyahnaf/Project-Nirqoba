# Stage 4 raw measurements

Internal `time.now()` milliseconds, 5 runs per configuration.
Median is the middle run; best is the fastest. Checksums are equal across
representations for the same workload, which shows identical work.

| case | rows | rep | workload | best ms | median ms | mean ms | checksum | runs |
|---|---|---|---|---|---|---|---|---|
| cache | 65536 | aos | single | 5 | 6.0 | 9.2 | 429490176000 | 18,11,6,5,6 |
| cache | 65536 | aos | multi | 8 | 9.0 | 10.0 | 1288470528000 | 11,13,9,9,8 |
| cache | 65536 | aos | random | 19 | 20.0 | 19.8 | 429490176000 | 21,20,19,19,20 |
| cache | 65536 | aos | write | 13 | 18.0 | 17.0 | 65935 | 13,17,19,18,18 |
| cache | 65536 | soa | single | 2 | 2.0 | 2.4 | 429490176000 | 3,2,3,2,2 |
| cache | 65536 | soa | multi | 3 | 4.0 | 4.4 | 1288470528000 | 4,7,4,3,4 |
| cache | 65536 | soa | random | 21 | 22.0 | 22.6 | 429490176000 | 21,26,22,22,22 |
| cache | 65536 | soa | write | 11 | 12.0 | 11.8 | 65935 | 12,11,12,11,13 |
| cache | 65536 | aosoa | single | 5 | 6.0 | 6.0 | 429490176000 | 7,7,5,5,6 |
| cache | 65536 | aosoa | multi | 9 | 9.0 | 9.4 | 1288470528000 | 9,9,10,10,9 |
| cache | 65536 | aosoa | write | 11 | 13.0 | 16.0 | 65935 | 24,11,13,12,20 |
| bandwidth | 2000000 | aos | single | 46 | 48.0 | 48.2 | 39999980000000 | 46,47,50,50,48 |
| bandwidth | 2000000 | aos | multi | 49 | 57.0 | 55.8 | 119999940000000 | 60,54,59,57,49 |
| bandwidth | 2000000 | aos | random | 166 | 167.0 | 167.8 | 39782300757120 | 166,167,166,171,169 |
| bandwidth | 2000000 | aos | write | 70 | 71.0 | 71.6 | 2000039 | 71,72,70,75,70 |
| bandwidth | 2000000 | soa | single | 14 | 15.0 | 15.2 | 39999980000000 | 17,15,16,14,14 |
| bandwidth | 2000000 | soa | multi | 28 | 32.0 | 30.8 | 119999940000000 | 33,32,33,28,28 |
| bandwidth | 2000000 | soa | random | 167 | 169.0 | 171.2 | 39782300757120 | 178,174,168,169,167 |
| bandwidth | 2000000 | soa | write | 39 | 45.0 | 43.8 | 2000039 | 39,43,46,46,45 |
| bandwidth | 2000000 | aosoa | single | 46 | 48.0 | 48.2 | 39999980000000 | 50,48,47,46,50 |
| bandwidth | 2000000 | aosoa | multi | 55 | 56.0 | 57.6 | 119999940000000 | 56,60,56,61,55 |
| bandwidth | 2000000 | aosoa | write | 68 | 70.0 | 70.8 | 2000039 | 70,74,70,72,68 |

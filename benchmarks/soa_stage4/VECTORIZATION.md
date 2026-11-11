# Vectorisation observed in emitted IR

Counts come from `--emit=llvm` output, so this is what the compiler actually
produced, not an inference from timings.

| representation | vector types | vector loads | vector stores | vector reduces |
|---|---|---|---|---|
| aos | 0 | 0 | 0 | 0 |
| soa | 23 | 6 | 0 | 3 |
| aosoa | 0 | 0 | 0 | 0 |

## n64-hardware-test

**Identifier registers**

| Register | Field | Expected | Notes |
|----------|-------|----------|-------|
| CP0 PRId | impl [15:8] | `0x0B` | VR4300 |
| CP0 PRId | rev  [7:0]  | varies  | see below |
| CP1 FCR0 | impl [15:8] | `0x0B` | VR4300 FPU |
| CP1 FCR0 | rev  [7:0]  | `0x00` | all known units |

**PRId revision mapping (so far)**

| rev  | interpretation |
|------|----------------|
| `0x10` | 1.0 — early retail (mulmul bug present) |
| `0x22` | 2.2 — later retail |
| `0x40` | 4.0 — iQue Player |

Not all board revisions have been tested.

---

**Bug probes**

| Probe | What it tests | Expected on early units |
|-------|--------------|------------------------|
| `mulmul` | FP double-multiply hazard (NaN/Zero/Inf operands) | FAIL |
| `sra`    | 32-bit arithmetic right shift 64-bit state leak | STUB |
| `mult`   | 32-bit signed multiply sign-extension anomaly | STUB |
| `div`    | 32-bit signed divide sign-extension anomaly | STUB |

STUB = probe scaffolded, asm not yet validated on hardware.

---

### Adding a new probe

1. Write a `probe_result_t probe_foo(void)` function returning `PASS()`, `FAIL(detail)`, or `STUB()`.
2. Add an entry to the `probes[]` table in `main()`.
3. If you have known revision↔result mappings, add an `interpret_foo()` helper.

Probes **must be pure** (no side effects, same result every call) — they are run twice: once for the OSD and once for the debug log.

---

### Building

Requires libdragon **preview** branch.

```
make
```

The CI workflow builds automatically on push.

---

### References

- [n64brew wiki — VR4300 bugs](https://n64brew.dev/wiki/VR4300)

## n64-hardware-test

**Identifier registers**

| Register | Field | Expected | Notes |
|----------|-------|----------|-------|
| CP0 PRId | impl [15:8] | `0x0B` | VR4300 |
| CP0 PRId | rev  [7:0]  | varies | see below |
| CP1 FCR0 | impl [15:8] | `0x0B` | VR4300 FPU |
| CP1 FCR0 | rev  [7:0]  | `0x00` | all known units |

**PRId revision mapping (so far)**

| rev  | interpretation |
|------|----------------|
| `0x10` | 1.0 |
| `0x22` | 2.2 |
| `0x40` | 4.0 (iQue Player) |

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

reminders for new probes:

1. Write a `probe_result_t probe_foo(void)` function returning `PASS()`, `FAIL(detail)`, or `STUB()`.
2. Add an entry to the `probes[]` table in `main()`.
3. If you have known revision↔result mappings, add an `interpret_foo()` helper.

Probes are run twice: once for the OSD and once for the debug log.

---

using libdragon preview branch

github actions builds automatically on push

---

from documentation provided in the libultra SDK:

---

1. Bug: Back-to-Back Floating Point Multiplies May Give Incorrect Results

Description

The following back-to-back multiply code sequence in the processor pipeline has the potential of producing an incorrect result in the second multiply:


    mul.[s,d]fd,fs,ft 
    mul.[s,d]fd,sf,st or [D]MULT[U] rs,rt

The error happens only when the first multiply is single- or double-precision floating-point operation and when one or both of its source operands are:


    Signalling Not-a-Number (sNaN9, 0 (Zero), or infinity (Inf).

The second multiply instruction may produce an incorrect result depending on the operands of the 1st multiply and the operands of the and multiply. The second multiply can be a multiply of any data type: floating-point or integer, single- or double-precision, signed or unsigned integer.

This code sequence can occur in the pipeline in two ways:

    The multiplies are back-to-back in the source code.
    The first multiply is in a branch delay slot and the second multiply is the target instruction of the branch. 

Software Workaround:

When an instruction of any kind (e.g. NOP) is executed between the two multiply instructions, the problem will not occur.

Release 2.0C includes a patch (patchSG0001118) to the C compiler and Assembler which reorders multiply code to avoid this bug. You must use the compiler option described in the patch release notes, which are located in


    patch1118/rlnotes/patchSG0001118/chl.z

If you use a different compiler or code in assembly language, you need to work around the problem as noted above.

Release 2.0C also includes a "checker" program called by makerom to ensure that programs are compiled with the proper compiler options to avoid this bug.

Affected Versions

This problem happens on versions 1.x, 2.0 and 2.1 of the CPU. 

---

### References

- [n64brew wiki — VR4300 bugs](https://n64brew.dev/wiki/VR4300)
- [u64check man page](https://help.graphica.com.au/irix-6.5.30/man/1/u64check)
# Differential test corpus (`cvm-fuzz` + `run_corpus.sh`)

CronoVM's hand-written conformance suite (`tests/conformance/conf_*.{c,cpp}`) pins
specific, known constructs. It is precise but narrow: historically, most
translator/VM miscompiles (spill / alloca-spill / invoke-spill / narrow-int /
global-align / zext-unsigned / sitofp-narrow / dup-operand-regfree /
phi-parallel-copy, …) were found *late*, when a real program (a game port)
crashed hours into a run. The corpus closes that gap by generating a broad,
randomised stream of programs and checking each **differentially**: a generated
program is compiled both **natively** (host clang — the oracle) and **on the VM**
(cvm-cc), and the two results are compared. A divergence is a translator/VM bug.

It plugs into the *existing* differential machinery (`tests/conformance/driver.c`,
`vm_entry.c`, `test_e2e`): every generated program exports `int conf_main(void)`
returning a single **int32 FNV-1a checksum**.

## Why a home-grown generator (and not csmith / gcc c-torture)

* **No `-m32` needed.** Each program uses only **fixed-width** types and folds an
  **int32** checksum, so the host (LP64) and the VM (ILP32) agree bit-for-bit —
  `int` is 32-bit on both. csmith leans on `long`/pointers, which forces a 32-bit
  native oracle (WSL-only here). A plain native build is the oracle.
* **MIT-clean.** The generator is ours; nothing GPL is vendored (rules out
  importing gcc c-torture / c-testsuite into this MIT repo).
* **Tuned to our bug history.** It deliberately stresses the recurring classes:
  high simultaneous live-variable pressure (spills), `i8`/`i16` narrow
  intermediates and narrow **signed** compares (narrow-icmp / sign-extend),
  `i64` arithmetic (the soft runtime), branch/loop/switch merges (phi +
  parallel copies), `noinline` calls (caller-save spills across calls), and
  by-value struct copies + local-array (`alloca`) indexing.
* It runs in the normal Windows/MSYS dev loop, like the rest of the suite.

csmith remains a sensible **future** second layer for the bugs we don't think to
generate; it would share this same runner.

## The hard invariant: generated programs are UB-free

If a generated program had undefined behaviour, native and VM could diverge
*legitimately* — a false positive. The generator guarantees UB-freedom **by
construction** (see the header of `tools/cvm-fuzz/cvm-fuzz.c`):

* all arithmetic/shift/rotate/division is evaluated in an explicit unsigned
  *compute type* (`uint32_t`/`uint64_t`) so signed-integer-promotion can never
  overflow (the classic `uint16_t * uint16_t → int` trap);
* shift counts are constants `< width`; divisors are forced nonzero with `| 1`;
* array indices are masked into range (power-of-two length);
* every variable is initialised at declaration (no uninitialised reads);
* no `long`/`size_t`/heap-pointer value is folded into the checksum, and no
  floating point (host hardware vs VM soft-float rounding could diverge — a
  deliberate future extension).

This is verified with UBSan: every seed compiled `clang -fsanitize=undefined`
runs clean. (`-fsanitize=integer` additionally flags *unsigned* wraparound, which
is well-defined C and intentional here — do **not** gate on it.)

## Usage

```sh
# one program for a seed (reproducible: same seed -> identical program)
cvm-fuzz --seed 42 -o prog.c          # [--stats] appends the knobs used
cvm-fuzz --seed 42 --no-rot64 -o prog.c   # skip 64-bit rotates (see GAP below)

# run COUNT seeds differentially at the given cvm-cc opt levels
run_corpus.sh <cvm-cc> <test_e2e> <clang> <cvm-fuzz> [COUNT] [SEED_BASE] [OPTS]
#   defaults: COUNT=100  SEED_BASE=1  OPTS="O1 O2"
#   FUZZ_FLAGS=--no-rot64 run_corpus.sh ...   # GAP-free baseline
```

Each seed's native oracle is computed **once** (the checksum is opt-level
independent for a UB-free program) and the VM build is compared to it at **every**
requested opt level — the IR, and thus the translator path, differs per level (a
miscompile can be `-O2`-only). Failing programs are saved under
`tests/corpus/fails/` (git-ignored) as `seed<N>_<opt>_{gap,miscompile}.c`.

Outcomes: **PASS** / **GAP** (cvm-cc couldn't translate) / **MISCOMPILE** (wrong
checksum) / **NATIVE-SKIP** (oracle build failed — shouldn't happen given the UB
invariant). Exit status is 0 iff every seed passed at every level.

## Minimising a failure

`tests/corpus/reduce.sh` is a ddmin line reducer. Its "interesting" predicate is
*native self-consistent (`-O0 == -O2`, i.e. well-defined) AND VM `-O2` disagrees*,
so it shrinks a `MISCOMPILE` to a small repro suitable for a permanent
`conf_*` fixture once fixed.

```sh
reduce.sh <cvm-cc> <test_e2e> <clang> fails/seedN_O2_miscompile.c min.c
```

## It is NOT a default `ctest` gate (yet)

The corpus is a *fuzzer*: on a translator with open bugs it is red by design, so
it is intentionally **not** wired into the default `ctest` run. Run it on demand.
Once the bugs it surfaces are fixed (and captured as `conf_*` fixtures), a
bounded GAP-free baseline (`FUZZ_FLAGS=--no-rot64`) can become a gating target.

### Findings so far

* **FIXED — narrow `udiv`/`urem`/`sdiv`/`srem` operand normalisation.** The first
  run's `-O2`/`-O3`-only `MISCOMPILE` reduced (via `reduce.sh`) to
  `(uint16_t)((uint32_t)a / ((uint32_t)b | 1))`, which clang narrows to `udiv i16`
  at `-O2`; the translator divided the raw 32-bit registers without masking the
  operands to 16 bits, so high garbage corrupted the quotient. Fixed in
  `translator.c`; guarded by `conf_narrow_div`. (See the CHANGELOG.)
* **GAP — `llvm.fshl.i64` / `fshr.i64` unsupported.** 64-bit funnel shifts (what
  clang canonicalises a 64-bit rotate to) are not lowered; the handler caps at
  `fbits <= 32` (`translator.c`). Tied to the i64 legalizer work. Use
  `--no-rot64` for a baseline that avoids it. STILL OPEN.

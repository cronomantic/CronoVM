# Changelog

All notable changes to CronoVM are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project adheres to [Semantic Versioning](https://semver.org/) — once
1.0 is reached. Pre-1.0 releases may break compatibility on any minor
version bump; breaks are called out explicitly under **Breaking**.

## [Unreleased]

### Added

- **Translator: `llvm.fshl.i64` / `llvm.fshr.i64` (64-bit funnel shift / rotate).**
  clang canonicalises a 64-bit rotate to these; the funnel-shift handler previously
  capped at `fbits <= 32` and rejected i64. They now lower to a soft-runtime call
  into `cvm_int64_rt` (`__cvm_fshl64` / `__cvm_fshr64`, sret i64), exactly like i64
  div/rem and variable shifts; the `--probe-runtime` scan flags the intrinsic so
  cvm-cc auto-links the i64 runtime. cvm-cc now compiles the hand-written soft
  runtime TUs scalar (`-fno-vectorize -fno-slp-vectorize`): at `-O2`/`-O3` clang
  would otherwise auto-vectorise the helpers into vector `phi`/`select` ops the
  legaliser doesn't accept (user TUs keep vectorisation). Guard: `conf_fshl_i64`.
  (Found by the differential corpus — it was the last open `--no-rot64` gap.)
- **Differential test corpus — `cvm-fuzz` generator + `run_corpus.sh`
  (`tools/cvm-fuzz/`, `tests/corpus/`, `docs/corpus.md`).** A seed-reproducible
  generator emits broad, randomised, **UB-free** C programs (each exporting
  `int conf_main(void)` returning an int32 FNV checksum) that the runner builds
  both natively (oracle) and on the VM (cvm-cc) and compares — catching
  translator/VM miscompiles *proactively* instead of via a downstream game crash.
  Uses only fixed-width types + an int32 result so the LP64 host and ILP32 VM
  agree without a `-m32` oracle; MIT-clean (no GPL corpus vendored). Tuned to the
  recurring bug classes (register-pressure spills, narrow-int + narrow signed
  compares, `i64`, phi/branch/switch merges, caller-save spills, struct copy,
  `alloca` indexing). UB-freedom is guaranteed by construction (all arithmetic in
  an unsigned compute type, masked shifts, guarded division, in-bounds indexing,
  no uninit reads) and verified with `clang -fsanitize=undefined`. The runner
  builds each seed at every requested opt level (a miscompile can be `-O2`-only)
  and saves failing programs for `reduce.sh` (ddmin) minimisation. It is a
  fuzzer, not a default `ctest` gate. (First run already surfaced two real issues:
  the narrow `udiv`/`urem`/`sdiv`/`srem` operand-normalisation miscompile fixed
  below, plus an unlowered `llvm.fshl.i64` funnel-shift gap — see `docs/corpus.md`.)
- **VM diagnostics subsystem (`docs/debugging.md`; compile flag `CVM_DIAG`, CMake
  `-DCVM_DIAG=ON`).** An opt-in, off-by-default, env-var-driven set of instruments
  built into the interpreter for chasing memory corruption and codegen miscompiles in
  carts: a **redzone allocator** (`CVM_REDZONE` + `CVM_RZ_MALLOC/FREE/REALLOC/CALLOC`)
  that traps the first out-of-bounds heap store with a backtrace, holder scan, register
  dump and stack-frame window; **write/value watchpoints** (`CVM_WADDR`, `CVM_WVAL`); a
  **misaligned-pointer-write detector** (`CVM_MISP` + `_TARGET/_FUNC/_AND3`, covering
  `memcpy` too); **slot write-history ring buffers** (`CVM_RING_A/B`); **register
  trip-wires** (`CVM_TRIP_PC` + `CVM_TRIP_SP`); an instruction dump (`CVM_PCDUMP` +
  `_RB/_R1/_SP`); and a per-word last-writer-PC map. `PRIVATE` to the `cvm` target — no
  ABI or `cvm.h` impact, zero overhead when off. Cracked the Exult `Usecode_internal::
  run()` spilled-alloca heap corruption (a 1.4M-element relocate over-walking the heap
  from a garbage near-NULL vector `this`); the worked example is in `docs/debugging.md`.
- **`cvm-dis` — a bytecode disassembler (`tools/cvm-dis/`).** Decodes the CODE
  section of a `.crom`/`.bin` image into readable instructions and labels function
  entry points and `CALL` targets via the optional `<image>.sym` sidecar (written by
  cvm-translate under `CVM_SYMS`). Modes: `--func N` (a symbol index), `--pc PC N`,
  `--around PC B A`, or no arg to list functions. Pure C, no LLVM dependency; built +
  installed alongside `cvm-cc`. Reads `include/cvm.h` for the opcode/section enums so
  it stays in sync with the ISA. (FUNCS slots are `(symidx)<<1`, so a runtime
  call-target slot maps to `.sym` index `slot/2`.) Used to localise a translator/
  runtime fault back to its source function.
- **Conformance: C++ exception-unwind + allocator-alignment fixtures.**
  `conf_pico_cpp_exc_unwind` (a throw propagated through MANY frames each owning
  non-trivial STL locals — `std::set<unsigned>`/`std::string` cleanup landingpads —
  past intermediate non-matching `try`s, caught by `std::exception&`, with a combined
  cleanup+catch frame); `conf_pico_cpp_exc_ctor` (a throw FROM a constructor,
  verifying the EH cleanup destroys ONLY the already-constructed members); and
  `conf_pico_cpp_align` (`operator new` / array-new / `std::vector` buffer + element
  addresses are 4-aligned across reallocation growth); and `conf_pico_cpp_alloca_spill`
  (many distinct stack-variable addresses kept live across a pressure point so a batch
  of `alloca` pointers spill, then read back through their reloaded pointers — guards
  the spilled-alloca-pointer fix). All differential vs clang++; they extend the EH
  corpus beyond `conf_cpp_exc`'s shallow surface and add allocator alignment +
  spilled-alloca-pointer coverage.
- **Translator: fixed-vector `load`/`store` (memory copy) + vector arithmetic,
  and pointer-element vectors `<N x ptr>`.** The vector legaliser previously
  handled only libc++'s num_get *movemask* idiom (insertelement / shufflevector /
  vector-icmp / sext|zext|trunc / `bitcast <N x i1>→iN`). clang's `-O2`+
  auto-vectoriser also emits, for ordinary code, (1) a vector **memory copy** —
  a small struct/union copy becomes `load`/`store <2 x ptr>` (the Exult
  `Usecode_value` union-copy case) — and (2) vector **arithmetic** — the
  soft-float `i64`→`f64` runtime (`__cvm_f_from_i64`) becomes a `<N x i32> xor`.
  Both are now lowered per-lane: `load`/`store` move N element-sized lanes
  (`LDB`/`LDH`/`LDW` by element width; a pointer lane is pointer-width = `LDW`)
  between memory and the value's frame slots; `add`/`sub`/`mul`/`and`/`or`/`xor`/
  `shl`/`lshr`/`ashr` run per lane (narrow right-shifts normalise the shiftee
  first, as the scalar path). `<N x ptr>` is now in the subset (a pointer lane is
  one 32-bit slot). Net effect: a `-O2`/`-O3` C++ cart translates, not just an
  `-O1` one. New differential fixtures `conf_vector_ops` (explicit `vector_size`
  load/store/arith across i8/i16/i32 lanes — built at the suite's `-O1`, so it
  exercises the paths deterministically) and `conf_pico_cpp_variant_vector` (a
  tagged-union-of-non-trivials in a `std::vector`, the Exult `Usecode_value`
  pattern). Suite 36 → 38. C carts (no vectors) unaffected.
- **Vendored libc++ `std::filesystem` library → a SEPARATE `cxxfs.bc`.** The
  `llvmorg-22.1.6` filesystem `src/` subset (`operations`/`directory_iterator`/
  `directory_entry`/`path`/`filesystem_error`/`filesystem_clock` + the private
  headers `error.h`/`file_descriptor.h`/`posix_compat.h`/`path_parser.h`/
  `time_utils.h`/`format_string.h`) is vendored under `runtime/lib/libcxx/src/
  filesystem/` and built by `build_cxxio.sh` into its OWN module `cxxfs.bc` — NOT
  `cxxio.bc`. Rationale: the filesystem TUs call a POSIX FS surface
  (`open`/`stat`/`openat`/`lstat`/`readlink`/`statvfs`/…) that only an embedder
  can satisfy, so folding them into `cxxio.bc` would make EVERY iostream consumer
  (including the `conf_pico_cpp_iostream` fixture, which links no embedder) fail
  translation on the undefined POSIX calls. `cvm-cc` does NOT auto-link `cxxfs.bc`;
  a cart that uses `std::filesystem` links it EXPLICITLY alongside its POSIX
  backend (real `stat`/`open` from the cart's `cron_sys.c` + a small ENOSYS shim
  for the rest). Surfaced by the Exult port (gamedat crash-backup). C++-only.
- **Translator: `llvm.cttz.i8` / `.i16`** (count-trailing-zeros at narrow widths).
  `cttz` previously handled only `.i32`; generalised to i8/i16/i32 — mask the
  operand to the value width first (like `ctpop`, so phantom sign-extended high
  bits don't skew the count) and return the width for a zero operand. Emitted by
  a libc++ algorithm in the Exult renderer. Covered by `conf_int_intrin` (narrow
  `__builtin_ctzg`, incl. the zero→width path).
- **libc++ algorithm SIMD path DISABLED** (`_LIBCPP_HAS_ALGORITHM_VECTOR_UTILS 0`
  in the vendored `__algorithm/simd_utils.h`). libc++ EXPLICITLY vectorises
  `find`/`count`/`mismatch`/… with `ext_vector_type` + a movemask, emitting
  `load/store <N x iM>` the VM has no types for (only the num_get movemask idiom
  is legalised). Forcing it off makes those algorithms use scalar loops. VM-wide,
  hermetic (no `-D`); a one-line marked `[CRONOPIO]` edit. Rebuilds `cxxio.bc`.
- **`cvm_cxxrt.cpp`: C++17 over-aligned `operator new`/`delete` (`std::align_val_t`).**
  clang emits these for any over-aligned type (e.g. Exult's `AdvancedOptions_gump`);
  they forward to the same C allocator (the VM's memory is byte-addressed with no
  alignment trap, so the requested alignment is not honoured — revisit with
  `memalign` if a real over-aligned datum needs it). All six forms (new/new[] +
  plain/sized delete, each with `align_val_t`).
- **Vendored libc++ `hash.cpp`: `std::__hash_memory`** (the ABI-exported murmur2/
  cityhash memory hasher). With our headers `_LIBCPP_AVAILABILITY_HAS_HASH_MEMORY`
  is set, so the header only declares it; the definition (its upstream home, built
  `-D_LIBCPP_BUILDING_LIBRARY` so the `_LIBCPP_NOESCAPE`/`_NOEXCEPT` signature
  matches) now lives in `hash.cpp` next to `__next_prime`, both `std::unordered_map`
  dependencies in `cxxio.bc`. Reached via libc++'s `std::hash`. C++-only.
- **Vendored libc++ `regex.cpp`** (added to `build_cxxio.sh`): `std::regex`
  matcher-node vtables/`regex_error` for `std::regex_replace` (Exult `game.cc`
  mod-name cleanup). One marked `[CRONOPIO]` edit narrows the ClassNames ctype-mask
  table through `unsigned char` (picolibc's signed `ctype_base::mask`). C++-only.
- **`cvm_cxxstl.cpp`: `std::bad_weak_ptr`, `std::to_string(int/long/long long)`,
  `std::stoi`, `std::chrono::steady_clock::now()`.** Out-of-line library symbols the
  header-only toolchain doesn't ship, reached by the Exult engine. Free functions
  are defined in the versioned `std::__1` inline namespace (else they mangle
  non-versioned and don't satisfy libc++'s `_ZNSt3__1…` decls). `steady_clock::now`
  returns a monotonic counter (no guaranteed hardware clock). C++-only.
- **picolibc: `lround`** (`libm/common/s_lround`, double→long). Exult's usecode
  intrinsics (`Paint_map::paint`) round with it; the 32-bit-`long` path is bit
  manipulation (no i64), `double` legalises via the soft-float runtime.

- **Float round-to-integral opcodes `FFLOOR` (0x3D) / `FCEIL` (0x3E) / `FTRUNC`
  (0x3F).** Single-precision round-to-integral has no closed-form bit trick (unlike
  `fabs`/`copysign`), so each lowers to one native opcode whose host evaluates
  `floorf`/`ceilf`/`truncf` — correct across the full range (NaN/Inf propagate,
  `|x| >= 2^23` is already integral), the same host-libm-call shape as `FSQRT`. The
  translator lowers `llvm.{floor,ceil,trunc}.f32` to them. Surfaced by libc++'s
  `std::ceil` in the `__hash_table` rehash (max-load-factor) — reached when the
  Exult port compiles its `FontManager` (`std::unordered_map`). `f64` variants are
  unaffected (they legalise via the soft-float runtime). Covered by `conf_float`
  (extended with `floorf`/`ceilf`/`truncf` over signed `.5`/non-`.5` fractions).

- **Vendored libc++ `hash.cpp` (`std::__next_prime`).** The `__hash_table` rehash
  also calls `std::__next_prime` (the next-prime bucket-count helper), an out-of-line
  library symbol the header-only toolchain doesn't ship. Added `hash.cpp` to the
  vendored libc++ src subset (`runtime/lib/libcxx/src/`, tag `llvmorg-22.1.6`, the
  same provenance as `locale.cpp`/`ios.cpp`/...) and to `build_cxxio.sh`, so
  `cxxio.bc` defines it. C++-only (no C cart links `cxxio.bc`). Same Exult-FontManager
  origin as the float-round opcodes above.

- **`cvm_cxxstl.cpp`: `std::bad_function_call` out-of-line members.** libc++ makes
  this exception's destructor its key function (the vtable + `type_info` live in the
  dylib), so any cart that invokes a possibly-empty `std::function` referenced
  `_ZTINSt3__117bad_function_callE` externally → "extern not supported". The
  destructor and `what()` are now defined here (under the same availability macros
  the libc++ headers use), emitting the vtable + `type_info` locally — same pattern
  as `bad_cast`/`bad_alloc`. Surfaced by the Exult port installing its file-stream
  factories as `std::function`. C++-only (no C cart links `cvm_cxxstl`).

- **`cxxio.bc` now builds with RTTI** (dropped `-fno-rtti` from `build_cxxio.sh`),
  so the vendored libc++ iostream/locale classes emit their `type_info`. A real
  C++ program (the Exult port) subclasses the iostream classes — a custom
  `std::streambuf` over a ROM/`SDL_IOStream` source — and uses `dynamic_cast`,
  which needs the base classes' RTTI; `-fno-rtti` left it externally undefined
  ("typeinfo for `std::basic_istream<char>` has no initializer"). Toolchain-only
  (no C cart links `cxxio.bc`); conformance corpus still 32/32.

- **Translator: `cvm_sys_*` host syscalls under C++ exception handling.** When a
  syscall call site sits in an EH scope, clang emits it as an `invoke` (any
  `extern "C"` callee is assumed to be able to throw), but the translator only
  recognised the `cvm_sys_` syscall form on the plain-`call` path — so the
  `invoke` form failed with "extern is not supported". A host syscall cannot throw
  a C++ exception, so the invoke's unwind edge is dead: the syscall lowering is
  now factored into `cg_emit_syscall_body` and shared by both the `call` dispatch
  and the `invoke` handler, the latter emitting the `SYSCALL` and branching
  straight to the normal successor (no EH frame). Surfaced by the first C++ cart
  (the Exult port) calling a `cron_*`/`cvm_sys_*` wrapper inside an iostream
  scope. (The differential conformance corpus has no host syscalls, so this is
  covered by the C++-cart toolchain smoke rather than a `conf_*` fixture.)

- **C++ `<iostream>`/`<sstream>`/`<fstream>`/`<locale>` support** (the vendored
  libc++ stream/locale library). `__config_site` now enables `LOCALIZATION` +
  `FILESYSTEM` and selects the NEWLIB locale dispatch (to picolibc's xlocale
  `*_l`); a matching libc++-22.1.6 `src/` subset (locale/ios/iostream/ostream/
  fstream/system_error/error_category) is vendored under `runtime/lib/libcxx-src/`
  and built by `build_cxxio.sh` into `cxxio.bc`. `cvm-cc` auto-links `cxxio.bc`
  when a program references the stream/locale ABI — a new `--probe-runtime` bit
  `CVM_PROBE_IOSTREAM` (implies `CXXSTL`); C programs and iostream-free C++ never
  pay for it. The cart's `picolibc.bc` supplies the locale `*_l` functions when
  built `--with-locale` (`build_picolibc.sh`). cvm-cc compiles C++ with
  `-D_GNU_SOURCE -mlong-double-64` so picolibc's xlocale is visible and no
  x86_fp80 long double is formed. New `conf_pico_cpp_iostream` differential
  fixture (num_get/num_put parse+format, `long long`, `double`, fstream
  round-trip). Known minor gap: `eofbit` after a parse that lands exactly on EOF
  differs on the VM (cosmetic; `if (in)` / `while (in >> x)` are correct).
- **Translator: `llvm.ctlz.i64`** (count-leading-zeros of a 64-bit value),
  lowered as `hi ? clz32(hi) : 32 + clz32(lo)`. Emitted by libc++
  `__to_chars_integral` (64-bit number formatting).
- **Translator: `llvm.invariant.start`/`llvm.invariant.end`** dropped as no-ops
  (pure memory hints, alongside `llvm.lifetime.*`). libc++ marks the classic
  locale invariant after init.

- **Translator: fixed integer vectors via per-lane scalarisation.** Clang lowers
  libc++ `std::char_traits<char>::find` (used by `std::num_get` to scan the digit
  atoms when parsing a number from a stream) to a SIMD *movemask* idiom even for a
  non-SIMD target: a scalar splat (`insertelement` + `shufflevector`), a vector
  compare (`icmp eq <N x i8>` → `<N x i1>`), and a `bitcast <N x i1>` to an
  integer mask. The translator now legalises a fixed `<N x iM>` integer vector
  (`M ≤ 32`) by scalarising it into `N` consecutive frame slots (one lane per
  slot, like the `i64`/`i65` wide values), lowering each vector op to per-lane
  scalar ops: `insertelement`, `shufflevector` (incl. the splat), `extractelement`,
  vector `icmp`, element-wise `sext`/`zext`/`trunc`, and `bitcast <N x i1> → iN`
  (the movemask: lane *k* → bit *k*). Register pressure stays bounded regardless
  of lane count (each lane's result is stored to its slot, so the emit-scratch
  cursor is rewound per lane). Vectors are function-local only (no
  calling-convention encoding — rejected across a call boundary); any unhandled
  vector op is rejected loudly rather than miscompiled. Guarded by the
  `conf_vector_movemask` differential fixture (32- and 16-lane). General SIMD
  scalarisation — hardens the VM for any C++ that hits a vector idiom, not just
  the Exult port that surfaced it.
- **Translator: 33..65-bit integer legalisation (`sadd.with.overflow.i33` /
  `.i65`).** The translator now lowers the odd wide integer widths clang emits
  for libc++ `std::num_get`'s overflow-checked stream parsing — an `int` is
  range-checked in `i33`, a `long long` in `i65`. A 33..64-bit value is a
  "wide2" 2-slot value (like `i64`) kept canonically sign-extended to 64 bits,
  so `sext`/`zext`/`trunc`/`icmp` reuse the existing `i64` paths unchanged;
  `i65` is a "wide3" 3-slot value `{w0, w1, sign}`. `sadd.with.overflow.iN`
  flags overflow by a canonicalise-mismatch (the 64-bit signed sum is exact for
  `N <= 63`; for `i65` the sign word carries bit 64). Only the exact op cluster
  num_get emits is legalised (`sext`/`zext`/`sadd.with.overflow`/`extractvalue`/
  `trunc`/`icmp slt …, 0`); other arithmetic at an odd width is rejected loudly
  rather than miscompiled. Guarded by the `conf_overflow_wide` differential
  fixture. (Hardens the VM for any C++ `iostream` integer parse, not just the
  Exult port that surfaced it.)
- **libm (picolibc), on demand:** `double` `exp`, `acos`, and `log` are now
  available (built into `picolibc.bc` when a program references them). `log`
  pulls in `__math_divzero` (for `log(0)`), so the divide-by-zero math-error
  path is included too.
- **`cvm-cc` input limit removed.** The compiler driver's input/bitcode/argv
  arrays grow dynamically instead of a fixed 256-entry cap, so a single
  invocation can compile/link an arbitrary number of translation units.

### Fixed

- **Translator: `zext`/`sext iN->i64` (N<32) now normalise the narrow source to N
  bits before forming the i64 low word.** The widening wrote the i64 low word
  straight from the source register; a narrow `iN` value can carry garbage above
  bit N-1 (notably because a negative narrow constant is materialised
  sign-extended, so a prior `xor iN` with one leaves 1s up top). That garbage
  landed in the low word and, in a subsequent `add i64`, fabricated a spurious
  carry into the high word (an off-by-one in the high 32 bits) — while the low
  word, masked downstream, looked correct, which hid it. Now both widenings mask
  the source to its width first (`zext`: zero-extend; `sext`: sign-extend, which
  also fixes `src >> 31` replicating the wrong bit). This single root cause was
  behind a CLUSTER of ~10 differential-corpus seeds (failing at `-O1` and `-O2`).
  Guard: `conf_zext_i64`. Conformance pass.
- **Translator: narrow (`iN`, N<32) `udiv`/`urem`/`sdiv`/`srem` now normalise their
  operands to N bits before the divide.** The VM's `DIV`/`DIVU`/`MOD`/`MODU` operate
  on the full 32-bit register, but a narrow value can carry garbage above bit N-1
  (the kept zero/sign-extended invariant only holds straight out of a load or
  `trunc`). For shifts this was already handled (`lshr`/`ashr`), but the
  divide/remainder path emitted the op directly on the raw registers — and unlike
  `add`/`mul`/`and`/`xor`, division depends on the FULL operand, so the quotient/
  remainder was wrong. The handler now zero-extends both operands for `udiv`/`urem`
  and sign-extends both for `sdiv`/`srem` (the same normalisation the shifts use).
  Triggered when clang's `-O2` narrows a 32-bit divide to `iN` once it proves the
  operands fit (`(uint16_t)((uint32_t)a / ((uint32_t)b|1)) -> udiv i16`), so it was
  invisible at `-O1` and to a hand-written `-O1` suite — **found by the new
  differential corpus** (`tools/cvm-fuzz`). Guard: `conf_narrow_div` (uses
  `_BitInt(N)` to force the narrow op at the conformance `-O1` build level; fails
  without the fix, passes with). Conformance 43/43.
- **Translator: a SPILLED `alloca` pointer is now written to its value-spill slot
  (the real Exult egg bug).** When the register file is exhausted (a function whose
  live-SSA count reaches `CG_MAX_SSA_REG` — only giant functions such as Exult's
  `Usecode_internal::run()`), an `alloca`'s pointer value spills to a value-spill
  slot like any scalar. Two defects left that slot holding stale garbage: (1) the
  prologue alloca-pointer materialisation did `ADD dst, SP, off` with
  `dst = regs[idx]`, but for a spilled alloca `regs[idx]` is the `CG_REG_SPILLED`
  sentinel (register R0, a syscall scratch) → the address was computed into R0 and
  never stored to the slot; (2) the `LLVMAlloca` body case was a no-op, so the
  generic spilled-result store persisted its UNINITIALISED `def_reg` (a stale
  scratch register) to the slot, clobbering it. Every later reload of the pointer
  then read a wild address — surfaced as `Usecode_value::add_values` being called
  with `this == 0x1`, corrupting a `std::vector<Usecode_value>` and crashing later
  in an unrelated `std::set` destructor. Fixed: the prologue skips spilled allocas
  (their dead `ADD R0` is dropped), and the `LLVMAlloca` body case computes
  `ADD def_reg, SP, off` (offset via the new `cg_alloca_offset_of`) so the generic
  store persists the real alloca address. Guard: `conf_pico_cpp_alloca_spill` (260
  stack-variable addresses kept live across a pressure point so a batch of alloca
  pointers spill, each read back through its reloaded pointer; bisected to a sharp
  threshold — one spilled alloca passed by luck, two miscompiled). DOOM/Quake
  unaffected (no function spills an alloca pointer).
- **Translator: global data layout now honours a global's EXPLICIT `align`, not
  just its type's ABI alignment.** clang lowers a C++ class with an anonymous union
  of non-trivial members as a PACKED struct (`<{ ... }>`, ABI alignment 1) yet still
  emits the global with the higher `align N` its members need. The layout pass used
  `LLVMABIAlignmentOfType` alone, placing such globals 1-byte-aligned at misaligned
  offsets (Exult's `Usecode_value no_ret` / the `zval` statics → misaligned
  reads/writes that smashed adjacent objects). Fixed: `align = max(ABI alignment,
  LLVMGetAlignment(gv))`. Found via `cvm-dis` + an alignment trap while hunting the
  egg bug; a genuine independent layout bug.
- **C++ runtime: an uncaught exception now HALTs cleanly instead of spinning the
  CPU forever.** `cvm_cxxrt.cpp`'s last-resort terminate path (`__cvm_eh_terminate`,
  the no-handler tail of `eh_unwind`, `__cxa_pure_virtual`,
  `__cxa_allocate_exception` on OOM, and `std::terminate`) used `for (;;) {}`, so
  any throw that no frame caught pegged a core indefinitely (observed as the
  unwinder at ~97% with no progress). They now `__builtin_trap()`, which the
  translator lowers to `HALT` — control returns to the host with a clean stop, no
  libc dependency (so it also works in the freestanding conformance harness).
  Surfaced by an Exult optional-file probe whose `file_open_exception` reached a
  context with no enclosing `try`. NOTE: this is the ONLY EH defect found — the
  type-match walk itself (`catch (std::exception&)` over a multi-level derived
  throw, including catch-by-library-base) was verified CORRECT in-situ and is now
  guarded by `conf_cpp_exc_base` + `conf_pico_cpp_exc_stdexc`.
- **Translator: `sitofp`/`uitofp` from `i64` to `double`** now reads the full
  64-bit operand. It was always lowered as a 32-bit conversion
  (`__cvm_f_from_i32`/`u32`), so `(double)int64` read only the low word / garbage
  (e.g. `(double)(uint64_t)25` returned ~3e5). The i64 source now routes to new
  soft-float helpers `__cvm_f_from_i64`/`__cvm_f_from_u64` with the operand passed
  as a wide (lo,hi) pair; the helpers compose the result from the 32-bit
  primitives as `hi*2^32 + lo` (exact halves, single rounding). Surfaced by
  picolibc `strtod`'s `__atod_engine` (it builds the mantissa in a `uint64_t`),
  which had made every `>> double` / `strtod` return garbage.
- **Translator: GlobalAlias in a constant initialiser** (e.g. a C++ vtable slot
  for the complete-object destructor `~T`, which libc++ emits as an alias to the
  base-object destructor when identical) is now resolved to its aliasee instead
  of being rejected (it is neither a function nor a global variable). Unblocked
  the libc++ locale facet vtables.
- **Translator: `zext iN -> i32` now masks to the source width** instead of being
  a bare register MOV. The MOV relied on the invariant that narrow values are
  always zero-extended in their register (true for `LDB`/`LDH` loads and `Trunc`,
  which mask) — but narrow integer *constants* are materialised sign-extended,
  and a function argument of unsigned-narrow type carries that bit pattern into
  the callee. So `zext` of a narrow UNSIGNED value with its high bit set produced
  a negative result, e.g. `(unsigned char)160` evaluated to `-96`. ZExt now
  emits an `AND` to the source width (a no-op when already zero-extended, a
  correction when sign-extended); `SExt` is unchanged. Guarded by the
  `conf_zext_unsigned` differential fixture.
- **Translator: arbitrary narrow integer widths (`iN`, `N` < 32).** Register-SSA
  values of odd widths (e.g. clang's `i14` from `-O1` narrowing) now translate —
  including the narrow `LShr`/`AShr` shiftee normalisation — alongside
  poison/undef values in global initialisers (clang's switch lookup tables).

## [0.4.0] — 2026-05-30

The **Beta** milestone. (The intermediate `v0.2.0` and `v0.3.0` tags shipped
without changelog sections; this entry consolidates everything since `0.1.0`.)
Since 0.1.0, CronoVM gained a complete C standard
library (picolibc) and C++ standard library (libc++ STL), software 64-bit
integer and `double` arithmetic, cooperative coroutines, C++ exceptions and
RTTI, and a differential conformance corpus. The runtime, toolchain and
language support are now stable enough for real projects (the binary format and
ABI may still change pre-1.0; breaks are flagged under **Breaking**).

### Added

- **C++ standard library (libc++ STL).** `cvm-cc` compiles C++20 against the
  toolchain Clang's own libc++ (`-stdlib=libc++`, version-matched). Working:
  `std::vector`, `std::string`, `std::map`, `<memory>`
  (`unique_ptr`/`shared_ptr`/`weak_ptr`), and the full integer/pointer
  `std::atomic` surface — plus `throw`/`catch` across the std exception
  hierarchy. The out-of-line std exception classes + `shared_ptr` control block
  are provided by `runtime/lib/cvm_cxxstl.cpp` (compiled against libc++ so Clang
  emits matching vtables/`type_info`); freestanding configuration lives in two
  small overrides, `runtime/lib/__config_site` and `__external_threading` (the
  libc++ thread API mapped onto cron coroutines). Auto-linked only when a module
  references the std exception ABI (a `--probe-runtime` `CXXSTL` bit).
- **picolibc is *the* C library, end to end.** Promoted from the phase-1 bitcode
  surface (below) to the full standard library: the SDK's hand-written libc
  became a thin machine port, and the complete stdio comes from picolibc
  tinystdio (`printf`/`scanf`/`FILE`/`fopen` over a host POSIX backend
  `open`/`read`/`write`/`lseek`/`close`). Allocator is a per-build choice
  (picolibc's sbrk-backed malloc, or a tuned O(1)-free allocator).
- **`std::atomic` / atomic intrinsics.** The translator lowers `atomicrmw`,
  `cmpxchg` and `fence` for scalar (i8/i16/i32/ptr) and i64 widths to plain
  load/op/store — correct under the cooperative, no-preemption model — and
  `llvm.trap`/`llvm.debugtrap` to `HALT`.
- **RTTI / `dynamic_cast`.** `__dynamic_cast` + the Itanium `type_info` ABI in
  `cvm_cxxrt`; `cvm-cc` no longer forces `-fno-rtti`.
- **Native C++ input + ABI runtime.** `cvm-cc` compiles `.cpp/.cc/.cxx`
  directly and auto-links `cvm_cxxrt` — `operator new`/`delete`, `__cxa_*`,
  global constructors, and the setjmp/longjmp exception unwinder.
- **Differential conformance corpus** (`tests/conformance/`). Each fixture is
  built for the VM *and* natively and the results are compared bit-for-bit, so
  unlowered intrinsics and miscompiles are caught proactively. 28 fixtures span
  C, C++ (OO/templates/lambdas/EH/RTTI), picolibc, the STL containers,
  `<memory>` and `std::atomic`.
- **f64<->i64 bitcast legalisation** plus the i64/`double` software runtimes
  reaching full coverage (mul/div/rem, shifts, conversions, the 64-bit calling
  convention).
- **Cross-platform CI.** GitHub Actions builds and tests on Linux (Clang + GCC),
  Windows (MSYS2 UCRT Clang) and macOS, plus bare-metal sanity builds for
  `thumbv6m`, `thumbv7m` and `rv32imc`.

### Changed

- **Project version → 0.4.0, status Beta.** Consumers now
  `find_package(CronoVM 0.4 …)`.

The remaining entries below were also part of this cycle:

### Added

- **picolibc as the C standard library (phase 1: 32-bit surface).** picolibc is
  vendored as a submodule (`external/picolibc`, pinned to upstream 1.8.11) and a
  curated subset is compiled straight to one i386-elf bitcode module via
  `runtime/lib/build_picolibc.sh` (clang `--target=i386-elf -ffreestanding
  -emit-llvm`, the SAME flags `cvm-cc` uses), configured by the hand-written
  `runtime/lib/picolibc.h` (freestanding, `__TINY_STDIO`, single global errno,
  no locale/wide/threads; `__PREFER_SIZE_OVER_SPEED` to keep the string routines
  in-bounds for the bounded VM address space). The result, `picolibc.bc` (a
  gitignored build artifact), is the pure embedder-independent C surface; the
  OS layer (errno storage, malloc/free, future write/read/sbrk/exit) is the
  embedder's machine port. This phase covers the 32-bit string/stdlib/ctype/
  numeric surface (strlen/strcmp/strstr/mem*, abs/div/atoi/atol/strtol,
  qsort/bsearch, …) PLUS the full 64-bit integer surface (llabs/lldiv/imaxabs/
  atoll/strtoul/strtoll/strtoull), unblocked by the abs.i64 + with.overflow
  lowerings below. New differential fixture `tests/conformance/conf_pico.c` links
  `picolibc.bc` on the VM side and checks it byte-for-byte against the host libc.
- **Three-way integer compare (`llvm.scmp` / `llvm.ucmp`, any width).** clang
  folds the spaceship idiom `(a > b) - (a < b)` — pervasive in `qsort`/`bsearch`
  comparators — into these intrinsics (result −1/0/1). The translator lowers them
  branch-free as two `CMP`s and a `SUB`, sign-/zero-extending narrow operands to
  32 bits first (mirroring the min/max lowering). Surfaced by the picolibc
  conformance fixture.
- **`llvm.abs.i64`** — the i64 case of the abs intrinsic (the i8/16/32 cases
  already existed). Lowered branch-free on the i64 slot pair via the sign mask:
  `m = hi >>(arith) 31; res = (x ^ (m:m)) − (m:m)`. Unblocks `llabs`/`imaxabs`.
- **Overflow-checked arithmetic (`llvm.{uadd,umul}.with.overflow`, i32 + i64).**
  The `{iN, i1}` aggregate clang emits for `__builtin_*_overflow` and for
  `strtoul`/`strtoull` digit accumulation. The value + overflow flag are computed
  at the call site (operands live) and stored to per-call aggregate frame slots
  (a new `agg_slot` map, like `i64_slot`); the consuming `extractvalue`s read them
  back — field 0 (the iN result; the i64 case aliases its slots onto the call's,
  so it costs no copy) and field 1 (the i1 flag). This extends `extractvalue`
  beyond landingpad results. i32 overflow uses the VM's `MULHU` (umul → high32 ≠ 0)
  / carry (uadd → sum < a); the i64 umul computes the schoolbook 64×64 high half
  to detect any bit ≥ 2⁶⁴. Unsigned add/mul only (picolibc's usage); other forms
  are rejected loudly. New fixtures exercise both differentially.

- **C++ exceptions (`try` / `catch` / `throw`)**. `cvm-cc` no longer forces
  `-fno-exceptions`; the translator lowers the Itanium EH instructions
  (`invoke` / `landingpad` / `resume` / `llvm.eh.typeid.for`) onto the VM's
  `SETJMP`/`LONGJMP` opcodes — there is no DWARF unwinder. Each `invoke` becomes
  a per-landingpad descriptor (the catch type-info list, emitted into DATA) on a
  thread-local frame chain plus a `SETJMP`; `__cxa_throw` walks the chain and
  `longjmp`s into the matching frame. The exception runtime (the unwinder +
  `__cxa_throw`/`begin_catch`/`end_catch`/`rethrow`/`allocate_exception` and
  `std::terminate`) lives in `runtime/lib/cvm_cxxrt.cpp`, auto-linked for any C++
  input. Supports typed catch, catch-by-base (most-derived-first clause order),
  `catch (...)`, non-trivial destructors run as cleanups during unwinding,
  `throw;` (rethrow), and propagation across functions with no handler of their
  own. Not supported: dynamic exception specifications (`throw()` filter
  clauses) and the MSVC/SEH model. With RTTI already done, this clears the last
  C++ language blocker for engines like Exult.
- **Cooperative-coroutine primitive `CVM_OP_CORO_SWAP` (opcode `0x3C`)**.
  Atomic save+load of a 16-byte execution-context record
  `{u32 pc, u32 sp, u32 dest, u32 status}` at heap addresses passed in
  `R[A]` (from) and `R[B]` (to). On first resume of a fresh coroutine
  (`status == CORO_FRESH`) the opcode reads `pc` as a function index and
  does a `FUNCS` lookup so the entry can be set up from cart C code with
  no extra VM primitive; on subsequent resumes (`status == CORO_SUSPENDED`)
  it uses `pc` as a raw program counter. Lets a cart implement any
  cooperative concurrency model (libco-style schedulers, Lua-style
  yield-with-value generators, async/await desugaring) on top of three
  small SDK functions — `cron_coro_init` / `cron_coro_swap` /
  `cron_coro_yield` (declared in Cronopio's `sdk/include/coro.h`).
  - On `FRESH` resume the opcode writes `R[to.dest] = to` so the entry fn
    receives the new coroutine pointer as its arg0 (`to.dest` is
    conventionally 0 = R0 per ABI). On `SUSPENDED` resume the register
    file is preserved intact, so the cart's caller-saved regs survive
    the swap without needing `returns_twice` at the call site. "Who
    resumed me" is recoverable from the cart-managed `to->resumer` field
    (the SDK's `cron_coro_swap` wrapper sets it just before the opcode).
  - Critical correctness detail: the translator routes
    `__cvm_coro_swap_raw(from, to)` through the **full user-call lowering
    protocol** (caller-saved SSA-reg spill to the cart's frame, arg
    placement in `R0`/`R1`) and substitutes `CORO_SWAP R0, R1` for the
    `CALL` at emission time. The VM register file is **shared across
    coroutines** (the opcode only saves PC/SP/status, not the regs); the
    spill protocol is what makes register lifetimes correct across a
    yield.
  - New error code `CVM_E_BAD_CORO_STATE` (issued when a `CORO_SWAP`
    target has `status` ∈ {`CORO_RUNNING`, `CORO_DEAD`} — illegal
    re-entrancy or resumer chasing a finished coro). New e2e tests:
    `e2e_coro_basic` and `e2e_coro_pingpong` (4 cases, all green). Spec
    in [`docs/isa.md`](docs/isa.md).

### Fixed

- **Data offset 0 aliased the null pointer.** Globals were laid out starting at
  data offset 0, and since VM pointers are heap offsets, the first global had
  address 0 — indistinguishable from a null pointer. This was a latent ambiguity
  (`if (&global)` / null compares) and a concrete C++ exceptions bug: a
  `catch (T&)` clause whose type-info happened to land at address 0 looked exactly
  like the catch-all / cleanup sentinel and silently swallowed every exception.
  The translator now reserves an 8-byte guard at data offset 0 so address 0 means
  only "null". **Behaviour change:** global addresses shift by the guard size, so
  carts must be rebuilt against this version (output is otherwise unchanged).
- **Reference allocator (`runtime/lib/cvm_alloc.h`) was O(n²) for
  allocation-heavy workloads.** `cvm_malloc` did a first-fit walk over EVERY
  block from the heap start on each call, and `cvm_free` scanned the WHOLE
  heap to coalesce on each call — both O(total blocks). Mounting a ~10k-entry
  archive (UQM's `libs/uio` over a `.uqm` ZIP) spent **minutes** there.
  Rewrote it as an explicit doubly-linked free list + boundary-tag footers:
  `cvm_malloc` now walks only FREE blocks, `cvm_free` coalesces with both
  physical neighbours and splices in O(1). Same external API and header
  format (header at `ptr-4`, low bit = FREE), so `realloc` and friends are
  unchanged; min block grows 8 → 16 (footer + room for the free links). The
  UQM content-mount dropped from ~1–3 min to ~1 s. Allocator suite + all e2e
  fixtures (incl. `free_list` split/coalesce/reuse) stay green (133/133).

- **Register pool double-free when a value fills several operand slots of one
  instruction** (e.g. `x*x` → `llvm.fmuladd.f32(x, x, acc)`, or any `f(a, a)`).
  The per-operand register-free loop reached such a value once per occurrence
  and pooled its register twice; two later defs could then draw the SAME
  register and clobber each other. In a counted loop this manifested as an
  **infinite loop** — the exit `icmp` was assigned the register still holding
  the loop counter's increment (also the back-edge phi source), so the counter
  never advanced. Fix: `cg_free_reg` now keeps the free pool a set (no
  duplicates). New e2e fixture `dup_operand.c`.

### Added

- **`setjmp`/`longjmp` (opcodes `SETJMP` 0x3A, `LONGJMP` 0x3B).** Real non-local
  jumps. A `jmp_buf` captures `{resume pc, SP, dest reg}` in the heap; `longjmp`
  restores SP+pc and writes the value to the saved dest register, resuming right
  after `setjmp` (0 on the direct call, the longjmp value — mapped to 1 if 0 —
  on return). Cheap because the register file/pc are interpreter locals and the
  call stack lives in the heap, so the convention's existing caller-saved spills
  cover the live state across the jump. The translator lowers `setjmp`/`longjmp`
  calls to the opcodes (no library body). New e2e fixture `setjmp.c`. (First
  exercised by a C program using `setjmp`/`longjmp` for error recovery.)

- **More intrinsic lowerings.** The translator now lowers `llvm.fabs.f32`
  (AND `0x7FFFFFFF`), `llvm.copysign.f32`, `llvm.bswap.i16` / `llvm.bswap.i32`
  (shift/mask/or), and `llvm.cttz.i32` (count-trailing-zeros loop, like the
  existing `ctlz`). These previously errored as "unsupported intrinsic"; clang
  emits them from `fabsf`/`copysignf`, byte-swap idioms, and power-of-two `log2`.

- **f64 / f32 global initialisers.** `serialize_constant` writes a `double`
  `ConstantFP` global as its native 8-byte IEEE-754 little-endian image (f32 as
  4 bytes), matching how the soft-float runtime reads doubles from memory.
  Previously only f32 constants were serialised, so any `double` global was
  rejected "unsupported initializer shape".

- **`cvm-cc -D<macro>[=val]`.** The driver now accepts and forwards `-D` to
  clang (was rejected as an unknown option), so a build can predefine macros —
  e.g. selecting an optional libc feature.

- **Integrity seal section (`CVM_SEC_SEAL`).** `cvm-translate --seal` appends a
  12-byte seal — magic `'C','R','M','1'`, version, and a CRC-32 of all preceding
  file bytes — as the last section. New public `cvm_crc32()` and
  `cvm_seal_check()` (1 = sealed & valid, 0 = unsealed, -1 = corrupt). The loader
  ignores the section; a host verifies it before running. Lets a launcher both
  identify valid cartridges (cheap magic via `cvm_peek_section`) and reject
  corrupt/tampered ones (CRC at load).

- **Source locations (`file:line`) in translator diagnostics.** `cvm-cc` now
  compiles each TU with `-gline-tables-only`, and the translator reads the
  current instruction's debug location, so every rejection prints
  `file:line: in 'func': message` instead of just the function name. No `.bin`
  impact (debug metadata is dropped when VM bytecode is emitted); minimal
  bitcode/compile overhead. Unsupported `llvm.*` intrinsics with no dedicated
  lowering now say "unsupported intrinsic 'X'" rather than the misleading
  "extern is not supported".

- **64-bit calling convention (phase 3 — completes i64/f64).** `i64`/`double`
  may now be function arguments and return values, across real calls. The
  convention is word-based and generalises the scalar one (no-64-bit
  signatures get byte-identical code): arguments fill word positions, a 64-bit
  value taking two consecutive words (lo, hi) — words 0–7 in `R0..R7`, the rest
  on the stack, straddling allowed; a 64-bit return comes back in `R0`(lo):`R1`
  (hi). The callee prologue reads each argument word into its param's home
  (register for a scalar, two frame slots for a 64-bit param); `ret` of a wide
  value loads its slots into R0:R1; a wide-returning call stores R0:R1 into the
  result's slots. Distinct from the soft-runtime helpers' `sret` ABI (those
  stay void + i32/pointer). With this the i64/f64 legaliser is complete — a
  stock `double`/`long long` C interface translates and runs. New e2e fixture
  `abi64` (i64/f64 params+returns, mixed scalar/wide args, five-i64 args that
  straddle to the stack, nested wide calls). Removed the obsolete
  `translator_reject_double` test (double is no longer rejected). Full suite
  120/120; DOOM rebuilds unchanged.
- **64-bit `phi`, `select`, variable shifts, and `sqrt` (completes the in-body
  64-bit surface).** Loop-carried `i64`/`double` `phi`s now work (the phi-move
  pass emits a lo and a hi word-move per wide phi, reusing the scalar parallel-
  copy / conflict-staging path). `select` of a wide value lowers to two
  parallel word-MOVs. Variable-amount `i64` `shl`/`lshr`/`ashr` lower to a soft
  runtime call (`__cvm_{shl,shr,sar}64`; constant amounts stay inline).
  `llvm.sqrt.f64` (and the plain `sqrt` libcall clang emits without
  `-fno-math-errno`) lowers to `__cvm_fsqrt` (Newton–Raphson, exponent-halving
  seed). With this, the i64 and f64 surfaces are complete inside a function
  body; the only remaining gap is the 64-bit calling convention. New e2e
  fixture `i64f64_gaps`.
- **`i64` multiply / divide / remainder (completes the i64 legaliser).**
  `i64` `mul` is lowered inline (`lo = al*bl`, `hi = mulhu(al,bl) + al*bh +
  ah*bl`, via `MUL`/`MULHU`). `udiv`/`sdiv`/`urem`/`srem` lower to a soft
  runtime CALL into the new `runtime/lib/cvm_int64_rt.c` (`__cvm_{u,s}div64` /
  `__cvm_{u,s}mod64`, a ~64-iteration restoring long division on the `cvm_i64`
  struct, sret return — same ABI path as the f64 helpers); cvm-cc auto-links it
  when a module uses i64 div/rem (the `--probe-runtime` exit code is now a
  bitmask: `10` f64, `20` i64, `30` both). Also: `freeze` is now accepted
  (lowered as identity — clang emits it to block poison propagation, e.g.
  around the div operands), and `llvm.experimental.noalias.scope.decl` is
  dropped as a no-op (clang emits it when inlining the restrict-qualified
  runtime helpers). New e2e fixture `i64_muldiv`. The remaining i64 gaps are
  `phi`/`select`, variable-amount shifts, and the 64-bit calling convention.
- **cvm-cc auto-links the soft-float runtime.** When a program uses `double`,
  cvm-cc now links `cvm_float64_rt.c` automatically — `double` "just works"
  without hand-listing the runtime TU. It runs the new `cvm-translate
  --probe-runtime` on each compiled module (which exits `10`/`CVM_PROBE_F64`
  when a `double` appears, `0` otherwise) and adds the runtime to the link set
  only when needed, so integer-only programs link no soft-float code. Skipped
  if the user already listed the runtime TU (no duplicate-symbol error).
- **`f64` (double) legalisation in the translator (phase 2).** Native `double`
  is no longer rejected. It reuses the i64 two-slot storage, but since f64 ops
  can't be open-coded, each is lowered to a soft-float runtime CALL into the new
  `runtime/lib/cvm_float64_rt.c` (`__cvm_f*` external wrappers over
  `cvm_float64.h`); cvm-cc links that TU into any module using `double`. Inline
  exceptions: `fneg`/`fabs` (sign-bit flip), constants, load/store. The calls
  reuse clang's existing `cvm_f64` ABI (two i32 words per arg, `sret` for a
  64-bit return) — which the translator already handled — so no new ABI work;
  the result slot's address is passed as the sret pointer. Each f64-runtime-call
  is registered as a caller-save spill point so the existing liveness-narrowed
  save/restore protects live registers across it. Lowered: fadd/fsub/fmul/fdiv,
  fcmp (all 16 predicates, one call + optional 0/1 negation), sitofp/uitofp/
  fptosi/fptoui/fpext/fptrunc, fneg, double const/load/store, and the
  `llvm.fmuladd.f64`/`llvm.fma.f64` (→ fmul+fadd, intermediate in the result
  slot) / `llvm.fabs.f64` / `llvm.copysign.f64` intrinsics — so default-O1
  `double` code (which clang contracts `a*b±c` into `fmuladd`) translates
  without `-ffp-contract=off`. New e2e fixtures `f64_slice` and `f64_fma`,
  linked via cvm-cc against the runtime. **Not yet** lowered (clear error):
  other `llvm.*.f64` math intrinsics (e.g. `llvm.sqrt.f64`), double
  `phi`/`select`, and double across a function boundary (64-bit calling
  convention, a later phase). See `docs/translator.md` → "f64 (double)
  legalisation".
- **`i64` legalisation in the translator (phase 1).** Native 64-bit integers
  are no longer rejected: code that uses `long long` inside a function body now
  translates without hand-writing `cvm_int64.h`. Each `i64` SSA value lives in
  **two consecutive value-spill slots** (lo/hi words) and every `i64` operation
  is lowered to explicit 32-bit word arithmetic — no register ever holds 64
  bits, so the ISA stays 32-bit (no new opcodes). Lowered: `sext`, `zext`,
  `load`, `store` (incl. constant `i64` stores, the old struct-zero-init
  special case), `add`/`sub` (carry/borrow via `CMP_LTU`), `and`/`or`/`xor`,
  constant-amount `shl`/`lshr`/`ashr`, `trunc`, and `icmp` (eq/ne + all ordered
  predicates). New e2e fixture `i64_slice` (sext + i64 const store + volatile
  i64 load + add-with-carry + lshr 32 + trunc). **Not yet** legalised (clear
  error): `i64` `mul`/`div`/`rem`, `phi`/`select`, variable-amount shifts, and
  `i64` across a function boundary (args/returns — the 64-bit calling
  convention is a later phase). `f64` is a follow-on that reuses the two-slot
  machinery but lowers to `cvm_float64.h` soft-float calls. See
  `docs/translator.md` → "64-bit integer legalisation".
- **`QDIV6432` opcode (0x39) — general 64/32 unsigned divide.** Computes
  `R[A] = (u32)(((((u64)(u32)R[A]) << 32) | (u32)R[B]) / (u32)R[C])` in one
  host op, trapping on `R[C]==0` like `DIV`/`DIVU`. `A` is both the dividend's
  high word and the destination (a tied operand), which lets a 3-source/1-dest
  divide fit the 3-register encoding. Where `QDIV1616`'s numerator is fixed to
  `R[B]<<16`, this takes an arbitrary 64-bit dividend in two halves, so a
  software 64-bit long division collapses to a single instruction. Surfaced
  from `cvm_intrin_qdiv_64_32(hi, lo, divisor)` (`runtime/lib/cvm_intrin.h`);
  the translator stages it as `MOV tmp,hi; QDIV6432 tmp,lo,div; MOV dst,tmp`
  so the divide can't be corrupted by the destination aliasing an input. New
  e2e fixture `qdiv6432` (6 phases). Motivation: DOOM's `CVM_CrossDiv`
  (a 32×32 product difference / divisor) was a 64-iteration software loop at
  ~12.8% of in-level interpreter time; routing it through `QDIV6432` removed it
  from the hot path (−12.4% total in-level instructions on E1M1).

- **Optional interpreter self-time profiler (`-DCVM_PROFILE`).** Building
  `src/cvm.c` with `CVM_PROFILE` defined makes `cvm_exec_at` attribute every
  executed instruction to the currently-running FUNCS index, tracked with a
  shadow call stack kept in step with `CALL`/`CALLR`/`RET`. The result is
  per-function *self* instruction counts (exclusive of callees) — where the
  interpreter actually spends cycles. Surface: `cvm_prof_counts[]` /
  `cvm_prof_len` / `cvm_prof_total`, `cvm_profile_reset(func_count)`, plus
  caller attribution (`cvm_prof_watch = <fid>` → `cvm_prof_caller[caller]++`
  on each call into that fid). Zero overhead when `CVM_PROFILE` is undefined
  (the `PROF_*` hooks expand to no-ops), so the default build is unchanged.
  Pairs with the new `CVM_SYMS` sidecar to resolve indices to names; Cronopio's
  `headless_prof` is a ready-made driver.
- **`CVM_SYMS` symbol sidecar from the translator.** With the `CVM_SYMS`
  environment variable set, `cvm-translate` writes `<out>.bin.sym` —
  `fid<TAB>entry_offset<TAB>name` per user function, where `fid` is the runtime
  FUNCS index (the `+1` for the reserved `FUNCS[0]` null-fn slot is applied).
  Lets index-only tooling (e.g. the `CVM_PROFILE` profiler) map back to source
  names. Off by default; the loader never reads it. See [docs/translator.md].
- **`CVM_SEC_ROM` (section type 10) — read-only cartridge data.** A binary
  can bake an arbitrary byte blob (e.g. a game WAD) into the `.bin`; the
  loader copies it into the heap (layout `DATA | BSS | REGIONS | ROM |
  RESERVE`, before RESERVE so `cvm_sys_heap_start` is unchanged) and
  exposes its offset/size on `cvm_image` (`rom_offset`/`rom_size`) and to
  the program via two new auto-bound built-in syscalls `cvm_sys_rom_base`
  / `cvm_sys_rom_size`. Read-only by convention (the VM enforces only heap
  bounds). The translator/`cvm-cc` gain a `--rom=FILE` flag that appends
  the file's bytes as the section. Motivated by Cronopio needing to ship a
  multi-MB WAD without compiling a giant C array. New ctest `rom`
  (`tests/test_rom.c`) covers load, byte-exact heap placement, the built-in
  syscalls, and the no-ROM case.

### Added

- **Constant `store i64` split into two word stores.** clang lowers
  struct/array zero-init and small copies with 8-byte `store i64` chunks
  even on a 32-bit target (e.g. zeroing a vec4). The VM has no 64-bit
  register, but a *constant* i64 store splits cleanly into `STW` at addr
  and addr+4; a dynamic i64 store (which the type subset can't produce) is
  rejected. New ctest `e2e_struct_init` (`tests/fixtures/struct_init.c`).

- **`float` loads/stores and `llvm.fmuladd.f32` lowering.** Storing or
  loading an `f32` to memory (a float in a global/struct/array) was rejected
  ("store: unsupported value type") even though f32 shares the 32-bit
  register file — it now lowers to `STW`/`LDW`. And `a*b + c` on floats,
  which clang folds to `llvm.fmuladd.f32` under the default fp-contract, now
  lowers to `FMUL` + `FADD`. Together these unblock float vector/matrix maths
  in memory — the basis of the Cronopio 3D pipeline header. New ctest
  `e2e_float_mem` (`tests/fixtures/float_mem.c`).

### Fixed

- **`i1` (boolean) constants now materialise zero-extended (0/1), not
  sign-extended.** `cg_reg_for` loaded constant operands with
  `LLVMConstIntGetSExtValue`, so `i1 true` became -1. clang lowers
  `cond ? 0 : 1` on a reused boolean to `zext(xor i1 %cond, true)`, and with
  `true` = -1 the xor became a full-width NOT (`!1` → -2, `!0` → -1) — a silent
  boolean miscompile (it dropped DOOM's keyboard events, whose event type is
  `down ? ev_keydown : ev_keyup`). i1 constants now use the 0/1 zero-extended
  value; wider integers keep sign-extension (normalised at the use). Regression
  test `tests/fixtures/xor_i1.c` → `e2e_xor_i1`.

- **Transient registers are now recycled per instruction — fixes spurious
  "ran out of registers" in constant-heavy functions.** `cg_reg_for`
  materialises constants, globals and constant-expressions into registers
  above `ssa_reg_high` and never caches them across instructions, but
  `next_reg` was only reset per function, so it grew monotonically through
  emission. A function issuing many calls with immediate arguments (e.g. a
  console frame that fires dozens of graphics syscalls) exhausted the 245-
  register file even though no two of those scratch values were ever live
  at once. Codegen now resets `next_reg` to `ssa_reg_high` at the start of
  each instruction; an instruction's SSA *result* always lands in a
  pre-allocated register (< `ssa_reg_high`), so the scratch above it is dead
  once the instruction finishes. New ctest `e2e_reg_pressure`
  (`tests/fixtures/reg_pressure.c`, 120 constant-addressed global stores).

- **`llvm.memset/​memcpy/​memmove.*.i64` accepted.** clang emits the i64-length
  variant (not just i32) depending on flags — e.g. zeroing a large static or
  filling a struct in hosted mode. The VM is 32-bit (a length over 4 GiB is
  impossible), so the translator now takes a constant i64 length that fits in
  32 bits; only a dynamic i64 length (which the type subset can't produce) is
  rejected. `cvm-cc` additionally now passes `-ffreestanding`, matching the
  fixture build and keeping clang on the i32-length intrinsics in the first
  place.

- **Constant-expression GEP/cast operands now lower correctly.** clang -O1
  emits a `ConstantExpr` getelementptr as the pointer operand whenever code
  touches a global at a fixed offset (`store v, ptr getelementptr(@g, k)`)
  — common for any function that writes more than one element of a global
  array, or reads/writes a struct field. `cg_reg_for` had no case for
  constant expressions and bailed with "operand has no register assigned",
  rejecting otherwise-valid carts (it surfaced in a Cronopio cart that
  initialised a colormap array). Now constant GEPs fold to base + static
  offset (via the new `cg_const_gep_offset`, mirroring the GEP-instruction
  index walk) and `bitcast`/`addrspacecast`/`int<->ptr` constant casts
  reinterpret the operand's register. New ctest `e2e_const_gep`
  (`tests/fixtures/const_gep.c`) stores to fixed indices of an external
  global and returns 2*n+3.

- **FUNCS section now emitted when a function's address is taken even if
  it is never internally called.** Previously the translator gated FUNCS
  emission on `has_calls` (did codegen emit a CALL/CALLR?). A binary that
  hands a function pointer to the host via a syscall — for the host to
  invoke later through `cvm_call` — got a pointer value (`fidx + 1`) that
  indexed an absent table, so `cvm_call` trapped with `CVM_E_BAD_FUNCS`.
  Codegen now sets a `funcs_referenced` flag wherever a function value is
  materialised as an operand, and FUNCS is emitted when
  `has_calls || funcs_referenced`. New regression test `e2e_fnptr_export`
  (`tests/fixtures/fnptr_export.c` + `tests/test_fnptr_export.c`) exercises
  exactly this: a callback registered through a syscall, never CALLed
  internally, then invoked by the host via `cvm_call`. This is the usage
  pattern the Cronopio console relies on for its per-frame callback.

### Added

- **`cvm_call` host API** — re-enter the VM at a function registered in
  the FUNCS table without bouncing through the image's entry point.
  Driven by Cronopio's frame-callback model (a fantasy console host that
  needs to invoke a cart-registered `frame()` every 1/60 s without
  reloading the image each tick). Signature:
  `int cvm_call(struct cvm_image *img, uint32_t fn_index,
                 const int32_t *args, uint32_t arg_count,
                 int32_t *return_value);`
  Internally factors the previous `cvm_run_args` body into a static
  `cvm_exec_at(img, start_pc, …)` helper; `cvm_run_args` now forwards
  to it with `img->entry` as the start PC and `cvm_call` does the same
  with `FUNCS[fn_index]`. Each call gets a fresh register file; only
  the heap persists between calls. Error surface piggy-backs on the
  existing codes — `CVM_E_BAD_FUNCS` (no FUNCS section),
  `CVM_E_BAD_FUNC_INDEX` (index out of range), `CVM_E_NULL_FUNC_PTR`
  (index 0). New ctest entry `cvm_call` (`tests/test_cvm_call.c`)
  exercises both the success and error surface end-to-end on hand-
  assembled blobs.

- **rv32imc cross-compile sanity build.** New
  `cmake/toolchains/riscv32-none-elf.cmake` targets 32-bit
  RISC-V with integer multiply + compressed instructions
  (rv32imc, soft-float ABI ilp32) — the embedded baseline that
  covers ESP32-C3, CH32V103/203, GD32VF103, BL602, etc. Uses
  `riscv64-unknown-elf-gcc` (the Ubuntu package targets all
  RISC-V variants from one binary) with
  `--specs=picolibc.specs` for headers, since the GCC
  package on Ubuntu doesn't bundle a libc (unlike
  `gcc-arm-none-eabi`).
  Footprint on rv32imc (`-Os`, `-DCVM_NO_STDLIB_FALLBACK`):
  **7005 bytes total** (`.text` 5198, `.rodata` 1180, strings
  615) — `.text` is +24 % vs Cortex-M3 because RV32 instructions
  are mostly 4 B (the C extension shortens common ops to 2 B but
  not all of them), while Thumb-2 is denser at 2 B for most
  arithmetic. 20 undefined symbols, all in the existing
  allowlist. The allowlist gained 16 compiler-rt soft-float
  helpers (`__addsf3`, `__subsf3`, `__mulsf3`, `__divsf3`,
  `__negsf2`, six SF compare helpers, four SF↔int conversions);
  ARM toolchains never saw these because GCC renames them to
  `__aeabi_f*` under the EABI wildcard. `linux-cortex-m-sanity`
  CI job now matrices over `{thumbv7m, thumbv6m, rv32imc}`.

- **thumbv6m-none-eabi cross-compile sanity build.** New
  `cmake/toolchains/thumbv6m-none-eabi.cmake` targets Cortex-M0 /
  M0+ / RP2040 (`-mcpu=cortex-m0 -mthumb -mfloat-abi=soft`).
  Same allowlist as thumbv7m; M0 additionally pulls in
  `__aeabi_uidiv*`, `__aeabi_lmul`, and `__gnu_thumb1_case_uhi`
  (libgcc helper for Thumb-1 switch-jump-table dispatch — M0 has
  no efficient table-dispatch instruction). Added a
  `__gnu_thumb1_*` wildcard to the allowlist to cover the latter
  family. Footprint on Cortex-M0 (`-Os`,
  `-DCVM_NO_STDLIB_FALLBACK`): **6125 bytes** (`.text` 4404, plus
  the same rodata/strings as M3) — +224 B vs M3, +3.8 %. CI job
  `linux-cortex-m-sanity` now matrices over `{thumbv7m,
  thumbv6m}`.

### Changed

- **`cvm_d_div` rounds to nearest-even.** Previously truncated
  (round-toward-zero, within 1 ULP of IEEE). Now bit-exact against
  hardware IEEE 754 binary64 for ratios that fit in 53 bits.
  Implementation: the restoring-division loop runs one extra
  iteration to produce a guard bit; the residual remainder
  supplies the sticky bit; round-up fires when `G && (S || LSB)`
  with carry-out into the exponent on a 0x1FFFFF…FF + 1 boundary.
  `cvm_d_add` / `cvm_d_sub` / `cvm_d_mul` still truncate — div is
  the operation where bit-exactness mattered most for the fixtures
  we care about, and the others would each need their own
  guard/sticky plumbing for full RNE. Documented in the header's
  trade-offs section.

### Notes

- **Embedded footprint measured.** Cross-built `libcvm.a` for
  `thumbv7m-none-eabi` (Cortex-M3-class, soft-float) with
  `-Os` + `-DCVM_NO_STDLIB_FALLBACK`: **5901 bytes total** —
  4180 B `.text`, 1024 B dispatch jump table, 577 B error
  strings, 120 B switch tables, 0 `.data`, 0 `.bss`.
  ~9 % of a 64 KiB STM32F103 flash.
  Compile-time opcode gating (`#ifdef CVM_ENABLE_*`) was on
  the roadmap but the measurement retired it — removing an
  opcode saves ~34 B in the handler body but cannot shrink
  the dispatch table, which indexes the full 0–255 opcode
  space regardless. The full ISA stays in every build.

## [0.1.0] — 2026-05-11

First public release. Closes the foundation phase: the VM runs C
programs compiled by clang via the bitcode translator end-to-end,
with calling convention, memory model, allocator hooks, host memory
regions, full `f32` and (software-emulated) `i64`/`f64` arithmetic,
and a cross-compile sanity build for Cortex-M targets.

### Added

#### Toolchain

- `cvm-cc` — single-command driver wrapping clang + cvm-translate.
  Compiles `.c → .bin` with pass-through flags for `--heap-reserve`,
  `--stack-reserve`, `--region`, `-I`, `-O<n>`. Default `-O1` (matches
  what mem2reg promotion needs). `.bc` input bypasses clang.
- `cvm-translate` — bitcode translator. Reads LLVM bitcode (binary
  `.bc`), validates against the CronoVM IR subset, emits a CronoVM
  `.bin`. Rejects vectors, atomics, exceptions, threads, and types
  outside i1/i8/i16/i32/f32 + pointer with a clear error message.

#### Interpreter (55 opcodes)

- Integer arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `DIVU`, `MOD`,
  `MODU`, `AND`, `OR`, `XOR`, `SHL`, `SHR`, `SAR`, `MULH`, `MULHU`.
- Float arithmetic: `FADD`, `FSUB`, `FMUL`, `FDIV`, `FNEG`, `FSQRT`,
  four `FCMP_*` predicates, `F2I_S`/`F2I_U` (saturating), `I2F_S`/
  `I2F_U`.
- Control flow: `JMP`, `BEQ`, `BNE`, six `CMP_*`, `CALL`, `RET`,
  `CALLR`, `JMPR`, `SYSCALL`, `HALT`.
- Memory: `LDW`/`STW`, `LDB`/`STB`, `LDH`/`STH`,
  `MEMCPY`/`MEMSET`/`MEMMOVE`.
- Constants: `MOVI`, `MOV`, `MOVHI` (arbitrary 32-bit immediates via
  `MOVI + MOVHI`).

The interpreter is a single C source (`src/cvm.c`) with
computed-goto dispatch under GCC/Clang and a switch fallback for
MSVC. `f32` shares the integer register file (bitcast is a no-op
`MOV`). `i64` and `f64` are rejected at the ISA level — code that
needs them uses the runtime headers below.

#### Binary format

- Header + 9 section types (`CODE`, `DATA`, `BSS`, `IMPORTS`,
  `DEBUG`, `HEAP_RESERVE`, `STACK_RESERVE`, `FUNCS`, `HOST_REGION`).
- Fixed-width 32-bit instructions, little-endian.
- `FUNCS[0]` reserved as a NULL-fn-ptr trap.
- Format documented in [docs/format.md](docs/format.md).

#### Runtime headers (drop into user code)

- `cvm_alloc.h` — header-only first-fit free-list allocator;
  one-word block header, splits on alloc, forward-coalesces on free.
- `cvm_intrin.h` — wrappers for `MULH`/`MULHU`/`FSQRT`,
  fixed-point Q16.16 multiply, and saturating `F2I_S`/`F2I_U`
  (`cvm_f2i_sat_s`/`cvm_f2i_sat_u` — guarantees the opcode actually
  runs even when clang -O1 would fold the C cast as UB).
- `cvm_int64.h` — complete `u64`/`i64` surface as `struct {uint32_t
  lo, hi}`. Carry-propagating add/sub, full-precision mul (via
  `MUL` + `MULHU` cross-products), shifts (lowered through
  `llvm.fshl`/`fshr`), signed and unsigned comparisons.
- `cvm_float64.h` — software-emulated IEEE 754 binary64.
  Truncate rounding (within 1 ULP of IEEE), flush-to-zero
  subnormals, NaN propagation to canonical quiet NaN (payloads
  not preserved). `cvm_d_div` is a 52-iter restoring divider.

#### Host API

- `cvm_load(bytes, len, &img)` — original entry, malloc/free
  fallback for image-internal allocations.
- `cvm_load_ex(bytes, len, &img, &allocator)` — pluggable
  allocator: `cvm_allocator_t {alloc_fn, free_fn, user_data}`.
  Either function pointer NULL → fall through to stdlib; whole
  struct NULL → equivalent to `cvm_load`.
- `cvm_run` / `cvm_run_args(&img, args, n_args, &ret)`.
- `cvm_image_free`, `cvm_strerror`, `cvm_image_get_region`.
- Built-in syscalls: `cvm_sys_heap_start`, `cvm_sys_heap_size`,
  `cvm_sys_get_region`.
- `CVM_NO_STDLIB_FALLBACK` build-time gate: skip the `<stdlib.h>`
  include and turn a missing allocator hook into hard-fail
  (`alloc` returns NULL, `free` is a no-op).
- Host memory regions declared via `--region=NAME:SIZE[:DIR]` at
  translate time (DIR ∈ `r`/`w`/`rw`, default `rw`); discovered
  at run time via `cvm_sys_get_region(name_ptr)`. Up to 64 regions
  per binary, name ≤ 15 chars.
- Public version macros (`CVM_VERSION_MAJOR/MINOR/PATCH/STRING`)
  and matching runtime `cvm_version_string()` /
  `cvm_version_number()` accessors. `CVM_VERSION_STRING` and
  `CVM_VERSION_1_0` (binary-format magic) are deliberately
  distinct.

#### Translator codegen

- Calling convention: `R255` = SP, `R254` = scratch, `R0..R7`
  mirror the syscall ABI for first-8 args, additional args
  stacked. Return value in `R0`. FUNCS table indexed by stable
  per-function id (slot 0 = NULL-fn-ptr trap).
- Block-local SSA register reuse: per-block free pool;
  cross-block values, phi results, and phi inputs get
  permanent regs.
- Liveness-based spill at every `CALL` site, narrowed by an
  `ever_spilled` bitmap and packed via `slot_of[]` so the
  frame holds only regs ever live across some call.
- Post-emission branch relaxation: `BEQ`/`BNE` whose offsets
  exceed imm8 ±127 are rewritten to `BEQ +1; JMP imm24`.
- Switch lowering: chained `CMP_EQ + BNE` (sparse) or a DATA
  jump table read via `LDW + JMPR` (dense, `n_cases ≥ 4` AND
  density ≥ 0.5). Negative `iN` case constants handled
  correctly in both forms.
- `llvm.fshl.i32` / `llvm.fshr.i32` lowered to 9 opcodes with
  a `c == 0` fixup (the VM's `SHL`/`SHR` mask shift amounts to
  the low 5 bits, so naive lowering would corrupt `b >> 32`).
- Recognised intrinsics: `llvm.abs.i32`, `llvm.smax.i32`,
  `llvm.mem*`, plus `cvm_intrin_*` extern names which inline
  directly to opcodes (`MULH`, `MULHU`, `FSQRT`, `F2I_SAT_*`).
- `Trunc iN` masks to the target width; `SExt` lowered via
  `SHL + SAR`.

#### Distribution

- CMake `add_subdirectory(deps/CronoVM)` consumer pattern,
  exercised end-to-end by `examples/embedder/` (configure +
  build + run asserted by ctest).
- `cmake --install` populates a standard GNUInstallDirs
  layout; consumers `find_package(CronoVM 0.1 REQUIRED)` and
  link against `cronovm::cvm`. `cronovm::cvm-cc` and
  `cronovm::cvm-translate` (if available) are exported targets.
  Exercised end-to-end by `examples/installed_consumer/`.
- Runtime header dir is resolved by `cvm-cc` at runtime: probes
  `<exedir>/../share/cronovm/runtime/lib/`, then falls back to
  the CMake-baked build-tree path. `--runtime-dir=` overrides.

#### Tests

- 78 ctest cases covering: arithmetic, control flow, calls,
  recursion, alloca, indirect calls (function pointers + dispatch
  tables), narrow loads/stores, memory intrinsics, branch
  relaxation, switch (chain + table, negative case constants),
  multi-precision integer + float, fixed-point Q16.16,
  allocator hook end-to-end, host regions, end-to-end via
  `cvm-cc`, `find_package` round trip.
- Adversarial fixtures: `long_branch` (relaxation),
  `switch_neg_table` (negative-iN case constants),
  `spill_loop` (call-heavy loop simultaneously stressing
  spill compaction + branch relaxation).

#### Cross-compile sanity

- Toolchain file `cmake/toolchains/thumbv7m-none-eabi.cmake`
  drives a Cortex-M3-class cross build via `arm-none-eabi-gcc`.
- POST_BUILD hook runs `arm-none-eabi-nm --undefined-only`
  against `libcvm.a` and fails the build if any undefined
  symbol falls outside `cmake/cortex_m_allowed_symbols.txt`.
- The allowlist permits: the freestanding `<string.h>` subset
  (`memcpy`/`memset`/`memmove`/`memcmp`/`memchr`/`strcmp`/
  `strncmp`), `sqrtf`, `__aeabi_*`, and a handful of
  compiler-rt builtin names. Malloc/free/printf/exit are
  pointedly absent.

#### Fuzzing

- libFuzzer harness wrapping `cvm_fuzz_translate_buffer` (the
  parse + codegen pipeline on an in-memory bitcode blob).
  Gated behind `-DCVM_BUILD_FUZZER=ON`; auto-detects
  `-fsanitize=fuzzer` support via configure-time probe.
  A `cvm-translate-fuzz-replay` standalone driver works on
  any compiler/OS for corpus replay without sanitizers.
- 31-file seed corpus auto-staged from the existing fixture
  `.bc` files into `build/tools/translator/fuzz_corpus/`.
- First exploratory run (Linux clang-21, 5 minutes, 2 fork
  workers): 970 000 executions; coverage grew 1854 → 8025
  features; corpus grew 31 → 281 inputs; zero crashes rooted
  in the translator (all 1252 crash artifacts live inside
  libLLVM's bitcode parser).

#### CI

- Five GitHub Actions jobs on push-to-main / pull_request:
  `linux × {clang, gcc}` (full ctest); `linux-no-stdlib-fallback`
  (embedded gate); `linux-cortex-m-sanity` (thumbv7m cross-build
  with nm allowlist); `windows-ucrt64-clang` (msys2 ucrt64);
  `macos-clang` (brew LLVM).

#### Documentation

- [docs/isa.md](docs/isa.md) — full opcode table.
- [docs/format.md](docs/format.md) — binary format.
- [docs/translator.md](docs/translator.md) — codegen and spill
  protocol, switch lowering, branch relaxation.
- [docs/NEXT.md](docs/NEXT.md) — orientation notes for the next
  session (kept up to date as decisions land).
- [CLAUDE.md](CLAUDE.md) — design constraints and project
  character for AI coding sessions.

### Known limitations

- `cvm_d_div` is a 52-iter restoring divider with truncate
  rounding — within 1 ULP of IEEE, not bit-exact. Round-to-
  nearest is a deferred follow-up.
- f64 NaN payloads are not preserved; all NaNs collapse to
  `CVM_D_NAN`.
- `F2I_S` / `F2I_U` saturate (NaN → 0, overflow →
  `INT_MAX`/`MIN`/`UINT_MAX`). In C, casting a float to an
  integer when out of range is UB, and clang -O1 can fold
  the cast to any value (often 0) before reaching the opcode.
  Route through `cvm_f2i_sat_s` / `cvm_f2i_sat_u` from
  `cvm_intrin.h` to guarantee runtime saturating behaviour.
- Soft-float on hosts without an FPU (Cortex-M0/M0+/M3,
  RP2040) is provided by the C compiler that builds `cvm.c`
  (libgcc / compiler-rt) — ~50–100× slower than hardware,
  no feature flag.
- Little-endian only.
- Bitcode parser (`libLLVM`) is known to have crash-prone
  edge cases on adversarial input — the fuzz run above
  exclusively surfaced libLLVM crashes, not translator
  crashes. Run `cvm-translate` on untrusted `.bc` in a
  sandboxed subprocess.
- Translator's IR subset is intentionally narrow; anything
  outside scalar i1/i8/i16/i32/f32 + control flow + calls +
  GEP + the supported intrinsic set is rejected with a clear
  error.

[0.4.0]: https://github.com/cronomantic/CronoVM/releases/tag/v0.4.0
[0.1.0]: https://github.com/cronomantic/CronoVM/releases/tag/v0.1.0

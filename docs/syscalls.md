# Syscalls

CronoVM has no built-in I/O. All effects on the outside world go through
**syscalls** — host C functions registered by name and invoked from bytecode
with the `SYSCALL` opcode. Each game decides what syscalls to expose, so the
mechanism is open by design.

## End-to-end flow

```text
       user.c                     game.bin                   host program
   ┌───────────────┐          ┌──────────────┐          ┌──────────────────┐
   │ cvm_sys_print │  clang   │  IMPORTS:    │  load    │ cvm_link(img,    │
   │   _int(42);   │ ───────▶ │   "cvm_sys_  │ ───────▶ │  "cvm_sys_print  │
   └───────────────┘ + xltr   │   print_int" │          │   _int", &fn);   │
                              │  CODE:       │          │ cvm_run(img);    │
                              │   SYSCALL 0  │          └──────────────────┘
                              └──────────────┘
```

1. The user writes a C function call to `cvm_sys_<name>(...)`.
2. `clang` emits a normal `call` to `cvm_sys_<name>`.
3. The translator (forthcoming) recognises the `cvm_sys_` prefix, adds the
   symbol to the IMPORTS section, and emits `SYSCALL <id>` instead of a
   regular call.
4. The host program calls `cvm_link(img, "cvm_sys_<name>", fn, user_data)`
   to bind a C function to that import.
5. At runtime, `SYSCALL` looks up the bound handler and invokes it.

## The `cvm_sys_` prefix

Any C function whose name begins with **`cvm_sys_`** is treated as a host
import by the translator. Other functions are translated to bytecode normally.
This is a **translator-side convention**; the runtime resolves imports purely
by string match, so a hand-assembled binary may use any names it likes.

## Calling convention (v1.0)

| Slot | Carries |
| ---- | ------- |
| `R0..R7` | Arguments (left to right). At most 8 args. |
| `R0` | Return value, written by the handler. |
| `R8..` | Caller-saved scratch — handlers should not rely on these being preserved. |

Argument and return types are 32-bit. Wider types and aggregates will be
addressed when the calling convention work lands; for now, passing pointers
(addresses into the VM heap) covers strings and structs.

> Out-of-band: a handler that traps (returns non-zero) does **not** write a
> return value — `R0` is whatever it was before. `cvm_run` returns
> `CVM_E_SYSCALL_TRAP` and the host can inspect register state if it wants.

## Handler signature

```c
typedef int (*cvm_syscall_fn)(struct cvm_image *img,
                              int32_t *regs,
                              void *user_data);
```

- `img` — the running image. Use `cvm_heap_read` / `cvm_heap_write` for
  bounds-checked access to VM memory.
- `regs` — the 256-entry register file. Read args from `regs[0..N-1]`,
  write the return value to `regs[0]`.
- `user_data` — the opaque pointer passed to `cvm_link`.
- **Return**: `0` for success. Non-zero traps the VM with
  `CVM_E_SYSCALL_TRAP`.

### Heap helpers

```c
int cvm_heap_read (struct cvm_image *img, uint32_t addr, void *out, size_t n);
int cvm_heap_write(struct cvm_image *img, uint32_t addr, const void *in, size_t n);
```

Both bounds-check `addr + n <= heap_size` and return `CVM_E_BAD_ADDR` on
violation. Treat addresses coming from the VM as **untrusted** — always go
through these helpers.

## Adding a new syscall

To expose a new host capability:

1. **Pick a name** with the `cvm_sys_` prefix, e.g. `cvm_sys_play_sound`.
2. **Declare it** in a header the user code includes:
   ```c
   extern void cvm_sys_play_sound(int32_t channel, int32_t freq);
   ```
3. **Write the handler** in your host program:
   ```c
   static int my_play_sound(struct cvm_image *img, int32_t *regs, void *ud) {
       (void)img;
       audio_engine_t *eng = ud;
       audio_play(eng, regs[0], regs[1]);
       regs[0] = 0;
       return 0;
   }
   ```
4. **Bind it** before calling `cvm_run`:
   ```c
   cvm_link(&img, "cvm_sys_play_sound", my_play_sound, &my_engine);
   ```

Any syscall referenced by the binary that has no handler at run time fails
with `CVM_E_UNLINKED_SYSCALL`. There is no fallback — the host is
responsible for binding everything the binary needs. (The host can iterate
`img.import_names[]` after load and check for missing bindings up front.)

## IMPORTS section layout

```text
offset 0    u32   import_count
offset 4    u32 × import_count   name_offset[i]   (offset within section)
            u8[]                 NUL-terminated name strings
```

`SYSCALL imm16` references entry `imm16` in this table. The VM resolves it
to a function pointer at link time and stores both the pointer and
`user_data` in `image.import_fns[i]` / `image.import_userdata[i]`.

## Hello-world example

```c
/* user.c */
extern void cvm_sys_print_int(int32_t v);

int main(void) {
    cvm_sys_print_int(42);
    return 0;
}
```

```c
/* host.c */
#include "cvm.h"
#include <stdio.h>

static int print_int(struct cvm_image *img, int32_t *regs, void *ud) {
    (void)img; (void)ud;
    printf("%d\n", regs[0]);
    regs[0] = 0;
    return 0;
}

int main(void) {
    /* read game.bin into `bytes`/`len` here */
    struct cvm_image img;
    cvm_load(bytes, len, &img);
    cvm_link(&img, "cvm_sys_print_int", print_int, NULL);
    int32_t exit_code;
    cvm_run(&img, &exit_code);
    cvm_image_free(&img);
    return exit_code;
}
```

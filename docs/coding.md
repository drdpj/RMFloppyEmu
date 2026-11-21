# Coding rules

These rules apply to all code in this repository. Read them before writing code. Where a rule says "never", there are
no exceptions without the owner's agreement, and any such exception is recorded here.

## 1. Language and build

- **C11** (`-std=c11`, no GNU extensions in `src/core`). The firmware uses the Pico SDK 2.x for the RP2350.
- **No warnings.** Our files build with `-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
  -Wmissing-prototypes -Wpointer-arith -Wcast-align -Wundef -Wdouble-promotion -Wformat=2 -Werror`, in both the firmware
  build and the host test build. The list lives in `cmake/core.cmake`. Never silence a warning with a pragma or a cast
  to make it go away: fix the cause. If a cast is genuinely right, add a comment saying why.
- The SDK and `third_party/` are not held to our warnings and are never edited.
- Build with all cores: Ninja does so by default, and with make use `make -j$(nproc)`.

## 2. Layout: core and platform

```
src/core/    hardware-independent: protocol, disk images, emulated chips, the drive's logic
src/flux/    self-contained library: FM/MFM track decoding and image containers (HFE). Shared with Disk Wrangler
src/pico/    RP2350-specific: main(), UART/GPIO/IRQ, flash/SD storage, timers
tests/       host unit tests (Unity) for src/core
tools/       PC-side tools (MAME tracing, image utilities)
cmake/       build pieces shared by the firmware and the test build
```

- **`src/core` never includes a Pico SDK header** and never touches hardware. Everything it needs from the platform
  comes in through function parameters or small callback structs (`*_ops` / `*_io`). The host test build compiles
  `src/core` without the SDK, which enforces this rule.
- **`src/core` is deterministic.** It never reads a clock: time is passed in as a tick count. It never sleeps or busy-waits,
  and has no hidden global state. The same inputs always give the same outputs, which is what makes it testable and
  lets MAME traces be compared byte for byte.
- `src/pico` stays thin. If logic in it could be tested on the PC, move it to `src/core`.
- **`src/flux` is a reusable library** (read `src/flux/README.md`). It includes nothing from the rest of the project,
  not even `core/rmfe_assert.h`, and has `extern "C"` headers and its own `FLUX_*_H` guards. It is copied into Disk
  Wrangler as it stands, so it must not depend on anything else here. Everything else in this file applies to it.
- New `src/core/*.c` and `src/flux/*.c` files are added to `RMFE_CORE_SOURCES` in `cmake/core.cmake`. New
  `src/pico/*.c` files are added to `RMFE_PICO_SOURCES` in `CMakeLists.txt`.

## 3. Files and headers

- **Every source file starts with the licence header**, naming `Copyright (C) 2026 Daniel Jameson`. Use `/* */` for
  C, `#` for CMake and Python, and `--` for Lua. Copy it from an existing file.
- The header comes first, then a short comment saying what the file is for and where its facts come from
  (`docs/protocol.md` §…, or ROM addresses).
- Header guards are `RMFE_<DIR>_<FILE>_H`, for example `RMFE_CORE_BYTES_H`. No `#pragma once`.
- Includes use paths from `src/`, for example `#include "core/bytes.h"`. Order: the file's own header, then other
  project headers, then system headers, then third-party headers, with a blank line between groups.
- A header declares only the public interface. Anything private to a `.c` file is `static`.

## 4. Naming

| Thing | Style | Example |
|---|---|---|
| Functions | `module_verb_object`, snake_case, with the module prefix matching the file | `bytes_get_le16`, `link_send_status` |
| Types | snake_case with `_t` | `link_state_t`, `disk_geometry_t` |
| Struct and enum tags | the same name without `_t` | `typedef struct link_state link_state_t;` |
| Enum constants, macros, constants | UPPER_SNAKE with the module prefix | `LINK_BAUD_FAST`, `IDC_CMD_READ` |
| Variables, parameters, fields | snake_case; no Hungarian notation | `track`, `rec_count` |
| Files | snake_case, named after the module | `src/core/link.c`, `tests/test_link.c` |

- Use the 480Z and RM terms as the manuals and ROMs use them ("record" for the 128-byte CP/M unit, "sector" for the
  physical one, "drive", "side"). Never mix them: a function that takes records says so in its name.
- **Name protocol and hardware constants; never use a bare magic number.** A constant taken from the ROM or a manual
  cites its source next to it, for example `#define IDC_STATUS_NOT_READY 0x80 /* IDC3 0ec7 */`.

## 5. Formatting

- `.clang-format` is the authority: 4-space indent, 120 columns, attached braces (functions too), `char *p`.
  Run `clang-format -i` on changed files before finishing.
- Braces are required on any `if`, `for` or `while` whose body is more than one line, or that has an `else`. A one-line
  guard can stay on the same line: `if (!buf) return LINK_ERR_ARG;`.
- One statement per line. `switch` statements over an enum list every case, with no `default:`, so `-Wswitch` catches
  a case that has been forgotten.

## 6. Types and data

- Use the fixed-width types (`uint8_t`, `uint16_t`, `uint32_t`) for anything with a size on the wire, on disk or in
  the Z80. Use `size_t` for sizes and counts of buffers, and `bool` from `<stdbool.h>`.
- **Never cast a byte buffer to a struct** to parse it, because of alignment, padding and endianness. Read fields with
  `core/bytes.h`.
- Avoid relying on integer promotion: cast before shifting a byte left by 24 or more, and before mixing signed with
  unsigned.
- `const` everything that doesn't change, including pointer parameters that are only read. Large tables are
  `static const`, so they stay in flash.

## 7. Memory

- **No `malloc` or `free` in `src/core`.** State lives in structs that the caller owns (statically allocated in the
  firmware, on the stack or static in tests). Buffers are passed in with their size.
- `src/pico` may allocate only at start-up, never in steady state.
- Every function that writes to a buffer takes its capacity and checks it. A buffer overrun is a bug even if "it can't
  happen".

## 8. Errors

- Functions that can fail return a status enum (`*_status_t`) with `*_OK = 0`. Results come back through out-
  parameters. Never return a bare `-1`.
- Never ignore a returned status. If it really can be ignored, cast it to `(void)` with a comment.
- Programmer errors (a broken invariant, a NULL that can't happen) use `RMFE_ASSERT()`. It stops the tests, and in the
  firmware it logs the file and line on the debug console, then halts with the LED flashing. Errors in data from the
  480Z, a disk image or a ROM are never asserts: they are handled and reported.
- A HardFault in the firmware is reported the same way, with the fault registers (`src/pico/fault.c`). That includes
  core 0's stack overflowing its 4 KB, which the hardware stack limit turns into an immediate fault. Keep big
  buffers and state structs static, never on the stack.
- Behaviour on bad input matches the real unit where we know it, for example status `20` for a bad parameter
  (docs/protocol.md). Where we don't know, the code says so in a comment and the question goes into the open questions
  in `docs/protocol.md`.

## 9. Real-time and concurrency (`src/pico`)

- Interrupt handlers do the minimum: move bytes between hardware and a ring buffer, or set a flag. No `printf`, no
  waiting, no flash access.
- Data shared between an interrupt and the main loop, or between the two cores, is single-producer/single-consumer
  where possible. It uses the SDK's atomics or spin locks and `volatile` only where the hardware demands it. Each piece
  of shared state has a comment saying who writes it and who reads it.
- Code that must not stall during a flash write (UART interrupts, the link state machine) is placed in RAM with
  `__not_in_flash_func()`.
- If both cores are used, a comment at the top of `main.c` says which core owns what.

## 10. Comments and documentation

- Comments explain **why**, along with anything surprising about the 480Z or the unit. Don't narrate what the code
  plainly does.
- Behaviour copied from the real unit cites its evidence: `docs/protocol.md` section, IDC ROM address, MAME trace, or
  manual page. When the code and `docs/protocol.md` disagree, one of them is wrong. Find out which and fix it in the
  same change.
- Public functions have a comment in the header: what the function does, its parameters and units, what it returns,
  and who owns any buffers.

## 11. Copyright: what never goes in the repository

- **No RM material**: ROM images, bytes copied from them (including the 1049-byte boot block sent by command `FF`),
  tables extracted from them, disassembly listings, disk images, or scans of manuals.
- Code may describe RM behaviour, and cite ROM addresses as evidence, but must not reproduce it. The firmware gets the
  IDC ROM from the user at runtime.
- Test data that needs RM material is read from outside the repo (see `docs/testing.md`). Third-party code goes in
  `third_party/`, with its licence, and is listed in `THIRD-PARTY-NOTICES.md`.

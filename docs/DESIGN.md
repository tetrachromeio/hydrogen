# Design Notes

## Vision
Hydrogen is element 1: the lowest-level language in a family of languages.
Higher languages (Helium, ...) will be built on top of it.

It sits between assembly and C++. You get the control of C/C++ (raw memory,
explicit layout, inline asm, no GC) with friendlier syntax and an easy
learning curve. Think "simplified C++".

Principles:
- **Simple, readable keywords.** Declarations read like sentences:
  `new type`, `new function`, `use`.
- **One obvious way** to do common things.
- **No hidden cost.** No GC and no implicit heap allocation. Every allocation
  and every copy is visible in the code.
- **Low level is opt-in, not hidden.** Pointers, inline asm and memory layout
  are first-class, but everyday code doesn't need them.

## Decided
| Area              | Decision                                          |
|-------------------|---------------------------------------------------|
| Execution model   | Ahead-of-time native compiler, LLVM backend       |
| Implementation    | C++                                               |
| Type system       | Static, with type inference                       |
| Memory            | Manual + RAII (deterministic destruction), no GC  |
| Imports           | `use Module` brings its names into scope          |
| Types             | `new type`: value types (int, bool, string, ...)  |
| Type operators    | Types define core operators (`+ - == ...`) only    |
| Primitives        | `int`/`uint`/`float` (64-bit) are canonical; `i64`, `u64`, `usize`, `f64` alias them |
| Text              | `str` borrowed view (literals); `string` owned class with drop; `string`→`str` implicit |
| Printing          | `print<T: Printable>`; types define `new operator string` (like `__str__`) |
| Ownership         | Values with drop move (no implicit copies); `.copy()` explicit |
| Parameters        | Borrowed by default; `own` takes ownership         |
| C interop         | `extern function` (any module), `ptr<T>(x)` casts, `null` |
| Type forms        | `: base` = new distinct type; `= other` = alias    |
| Classes           | `new class`: data + methods + RAII; composition only, no inheritance |
| Heap              | Explicit: `alloc T(...)` / `free p`; stack by default |
| Functions         | `new function name() {}`                          |
| Errors            | `Result<T, E>` enum + `?` propagation; no exceptions |
| Generics          | Monomorphized; constrained by structural interfaces |
| File extension    | `.h2`                                             |
| Visibility        | Private by default; `pub` to export; `pub use` re-exports |
| Module layout     | `use A.B` → `A/B/init.h2` (project dir, deps, stdlib) |
| Multi-file modules| Root file does `include name` for sibling `name.h2`; shared namespace; `use` only in the root file, module-wide |
| Prelude           | Every module implicitly does `use Atomic`         |
| Tooling           | `hy` CLI (`new`, `check`, `build`, `run`, `deps`) driven by `hy.toml` |
| Dependencies      | Library projects named by module name; `path` deps now; git/registry later |

## Standard library
- **Atomic is fully freestanding**: no OS, no libc, no allocation. `string`, printing and OS access live in Elemental.
- **Atomic**: the freestanding core. Primitive types, memory and pointer
  ops, intrinsics, atomics. Works with no OS.
- **Elemental**: the hosted standard library. IO, strings, collections,
  math, files.

## Open
- Integer overflow behavior (wrap, trap or undefined)
- Mutating methods on `let` values
- String interpolation ("x = {x}")
- Borrow lifetimes: a str view can outlive its string in some cases (only returns are checked)

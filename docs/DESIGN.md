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
| Primitives        | Sized (`i32`, `f64`) plus aliases `int`, `float`   |
| Classes           | `new class`: data + methods + RAII; composition only, no inheritance |
| Heap              | Explicit: `alloc T(...)` / `free p`; stack by default |
| Functions         | `new function name() {}`                          |
| Errors            | `Result<T, E>` enum + `?` propagation; no exceptions |
| Generics          | Monomorphized; constrained by structural interfaces |
| File extension    | `.h2`                                             |
| Visibility        | Private by default; `pub` to export; `pub use` re-exports |
| Module layout     | `use A.B` → `A/B/init.h2` (project dir, deps, stdlib) |
| Multi-file modules| Root file does `include name` for sibling `name.h2`; shared namespace, per-file `use` |
| Prelude           | Every module implicitly does `use Atomic`         |
| Tooling           | `hy` CLI (`new`, `check`, `build`, `run`, `deps`) driven by `hy.toml` |
| Dependencies      | Library projects named by module name; `path` deps now; git/registry later |

## Standard library
- **Atomic**: the freestanding core. Primitive types, memory and pointer
  ops, intrinsics, atomics. Works with no OS.
- **Elemental**: the hosted standard library. IO, strings, collections,
  math, files.

## Open
- C interop / ABI and `extern` declarations
- Integer overflow behavior (wrap, trap or undefined)
- String interpolation

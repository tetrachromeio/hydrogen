# Hydrogen

Element 1: the lowest-level language in a family of languages. Hydrogen sits
between assembly and C++, with friendlier syntax. Higher-level languages
(Helium, ...) will be built on top of it.

```
use Elemental

new type Meters : float {
    new operator + (a: Meters, b: Meters) -> Meters { return Meters(float(a) + float(b)) }
    new operator * (a: Meters, b: Meters) -> Meters { return Meters(float(a) * float(b)) }
}

new class Point {
    x: Meters
    y: Meters
    new function length() -> Meters { return Meters(sqrt(float(self.x * self.x + self.y * self.y))) }
}

new function main() -> int {
    let p = Point(3.0, 4.0)          // stack, cleaned up automatically
    let q = alloc Point(6.0, 8.0)    // heap, explicit
    print("Hello, Hydrogen")
    free q
    return 0
}
```

**New here?** Start with the [cheat sheet](docs/CHEATSHEET.md): every piece of syntax and
every system (modules, ownership, memory, the standard library) on one page.
The full rules are in [spec/syntax.md](spec/syntax.md).

## Layout
- `compiler/`: `hy` (project tool) and `h2c` (compiler driver), in C++20
- `lib/`: the standard library. `Atomic` is the freestanding core, `Elemental` the hosted library
- `spec/`: language specification
- `docs/`: design notes and decisions
- `examples/`: sample programs
- `tests/`: golden tests for the lexer, parser and diagnostics, plus
  programs that are compiled, run and checked for leaks (`tests/run/`)

## Projects: `hy`
`hy` is the project tool. A project is a directory with a `hy.toml`:

```toml
[project]
name = "hello"
version = "0.1.0"
type = "app"                 # or "lib"

[dependencies]
Geometry = { path = "../Geometry" }
```

```bash
hy new hello          # create an app (hy new Geometry --lib for a library)
cd hello
hy check              # check every module for errors
hy run                # build and run
hy build              # build to build/hello
hy deps               # list resolved dependencies
```

A dependency is another `hy` project with `type = "lib"`. Its name is its
module name: `use Geometry` loads `../Geometry/init.h2`, and `use Geometry.Shapes`
loads `../Geometry/Shapes/init.h2`. Dependencies are resolved transitively.
For now, only `path` dependencies are supported.

`hy build` compiles to native code through LLVM: Hydrogen generates LLVM IR
and uses `clang` (set `H2_CC` to use a different one) to optimize and link it
with a small C runtime (`lib/runtime/h2rt.c`). `--release` builds with `-O2`.

## Editor support
`hy lsp` is a Language Server Protocol server: live errors as you type (syntax,
names, types, ownership), go to definition, and hover with types. The VS Code
extension in [`editors/vscode`](editors/vscode) adds syntax highlighting and
connects to it; other LSP editors (Neovim, Helix, Zed, ...) can run `hy lsp`
directly. See [editors/vscode/README.md](editors/vscode/README.md).

## Build the toolchain
```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

```bash
./build/compiler/hy help                          # project tool
./build/compiler/h2c --ast examples/hello.h2     # dump the AST
./build/compiler/h2c --tokens examples/hello.h2  # dump tokens
./build/compiler/h2c --check examples/hello.h2   # full check: names and types
./build/compiler/h2c --types examples/hello.h2   # print the inferred type of each local
./build/compiler/h2c --emit-llvm examples/hello.h2   # print the generated LLVM IR
```

To regenerate golden files after an intentional change:
```bash
python3 tests/run_tests.py build/compiler/h2c . --update
```

## Status
- [x] Lexer
- [x] Parser and AST, with error recovery
- [x] Modules, `pub` visibility and name resolution
- [x] `hy` project tool: `hy.toml`, path dependencies, `hy new` / `check` / `deps`
- [x] Language server (`hy lsp`) and a VS Code extension
- [x] Type checking and inference
- [x] Ownership: move semantics, borrowed parameters, `own`
- [x] Generic monomorphization
- [x] LLVM IR codegen: generics, classes, enums/match, `?`, strings, automatic drops
- [ ] Atomic / Elemental standard libraries

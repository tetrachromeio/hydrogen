# Hydrogen

Element 1: the lowest-level language in a family of languages. Hydrogen sits
between assembly and C++, with friendlier syntax. Higher-level languages
(Helium, ...) will be built on top of it.

```
use Elemental

new type Meters : f64 {
    new operator + (a: Meters, b: Meters) -> Meters { return Meters(f64(a) + f64(b)) }
}

new class Point {
    x: Meters
    y: Meters
    new function length() -> Meters { return sqrt(self.x * self.x + self.y * self.y) }
}

new function main() -> int {
    let p = Point(3.0, 4.0)          // stack, cleaned up automatically
    let q = alloc Point(6.0, 8.0)    // heap, explicit
    print(p.length())
    free q
    return 0
}
```

## Layout
- `compiler/`: `hy` (project tool) and `h2c` (compiler driver), in C++20
- `lib/`: the standard library. `Atomic` is the freestanding core, `Elemental` the hosted library
- `spec/`: language specification
- `docs/`: design notes and decisions
- `examples/`: sample programs
- `tests/`: golden tests for the lexer, parser and diagnostics

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

> `hy build` and `hy run` currently stop after checking: native code
> generation (the LLVM backend) is the next milestone.

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
./build/compiler/h2c --check examples/hello.h2   # syntax check
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
- [ ] Type checking and inference
- [ ] Generic monomorphization
- [ ] LLVM IR codegen
- [ ] Atomic / Elemental standard libraries

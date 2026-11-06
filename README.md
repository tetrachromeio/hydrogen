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
- `compiler/`: `h2c`, the compiler, written in C++20 (LLVM backend planned)
- `lib/Atomic/`: the freestanding core library (primitive types, Result)
- `spec/`: language specification
- `docs/`: design notes and decisions
- `examples/`: sample programs
- `tests/`: golden tests for the lexer, parser and diagnostics

## Build and test
```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

```bash
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
- [ ] Name resolution and modules (`use`)
- [ ] Type checking and inference
- [ ] Generic monomorphization
- [ ] LLVM IR codegen
- [ ] Atomic / Elemental standard libraries

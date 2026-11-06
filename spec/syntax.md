# Hydrogen Syntax: Draft 3

Status: **sketch**. Every rule here is up for discussion.

## Imports
`use` brings a module's public names into scope.
```
use Elemental            // print(), sqrt(), string, ... now usable directly
use Atomic.Memory as mem // aliased: mem.alloc(...)
```
- If two `use`d modules export the same name, using that name unqualified is
  a compile error. Disambiguate with the full path: `Elemental.IO.print(...)`.
- Qualified access (`Elemental.IO.print`) always works.

## Variables
```
let x = 10          // immutable, type inferred
var y: u8 = 255     // mutable, explicit type
```

## Functions
```
new function add(a: i32, b: i32) -> i32 {
    return a + b
}

new function log(msg: string) {   // no return type means void
    print(msg)
}
```

## Types vs. classes
These are two separate concepts.

### `new type`: values
A **type** is plain data with value semantics, like `int`, `bool` or `string`.
It has no identity, no inheritance and no methods. It is copied by value and
its layout is fully known. The built-in types are themselves declared with
`new type` in Atomic.

A type is built from raw bits, from an existing type, or from a fixed layout:
```
new type bool   : bits(1)
new type i32    : bits(32) signed
new type f64    : float(64)
new type Meters : f64              // distinct; no implicit mixing with f64

new type string {
    data: ptr<u8>
    len:  usize
}
```

#### Operators
A type may define **core-level operators** and nothing else. At the bottom,
Atomic implements them with compiler intrinsics (or inline `asm`):
```
new type i32 : bits(32) signed {
    new operator + (a: i32, b: i32) -> i32  { return intrinsic.add(a, b) }
    new operator - (a: i32, b: i32) -> i32  { return intrinsic.sub(a, b) }
    new operator == (a: i32, b: i32) -> bool { return intrinsic.eq(a, b) }
}

new type Meters : f64 {
    new operator + (a: Meters, b: Meters) -> Meters { return Meters(f64(a) + f64(b)) }
    new operator == (a: Meters, b: Meters) -> bool  { return f64(a) == f64(b) }
}
```
Overloadable operators: `+ - * / % == != < <= > >= & | ^ ~ ! << >> []`. A `-` or `!` operator with one parameter is the unary form.
`!=` is derived from `==` when not defined. Assignment and `&` (address-of)
cannot be overloaded.

#### Built-in primitives
| Sized                         | Friendly alias                |
|-------------------------------|-------------------------------|
| `i8 i16 i32 i64`              | `int` = `i64`                 |
| `u8 u16 u32 u64`, `usize`     | `uint` = `u64`                |
| `f32 f64`                     | `float` = `f64`               |
| `bool`, `char`                |                               |

Both forms are always available. Use `int`/`float` for everyday code and the
sized names when layout matters.

### `new class`: objects
A **class** groups data with behavior: methods, construction (`init`) and
RAII cleanup (`drop`). Classes use **composition only**. There is no
inheritance and there are no virtual methods, so there is no hidden vtable.
```
new class Buffer {
    data: ptr<u8>
    size: usize

    new function init(size: usize) {
        self.data = mem.alloc_bytes(size)
        self.size = size
    }

    new function drop() {            // runs automatically
        mem.free_bytes(self.data)
    }

    new function get(i: usize) -> u8 {
        return self.data[i]
    }
}

new class Image {
    pixels: Buffer                   // composition: Image *has* a Buffer
    width:  int
    height: int
}
```

## Memory: stack vs. heap
```
let a = Buffer(1024)          // stack; drop() runs at end of scope
let b = alloc Buffer(1024)    // heap; b: ptr<Buffer>
b.get(0)                      // member access auto-derefs pointers
free b                        // runs drop(), then releases the memory
```
- Nothing goes on the heap unless you write `alloc`.
- `alloc` works for types too: `alloc int(0)`, `alloc [u8; 4096]`.
- `free` is explicit. Owning smart pointers (`Box`, `Shared`) can be built
  on top of this in Elemental.

## Enums
A tagged union. Variants may carry data.
```
new enum Color { Red, Green, Blue }

new enum ParseError {
    Empty
    BadDigit(char)
    Overflow
}
```

## Error handling: `Result` and `?`
Errors are ordinary values. There are no exceptions and no unwinding.
`Result` is a plain generic enum in Atomic:
```
new enum Result<T, E> { Ok(T), Err(E) }
```
`?` unwraps `Ok` or returns the `Err` from the enclosing function:
```
new function parse_digit(c: char) -> Result<int, ParseError> {
    if c < '0' || c > '9' { return Err(BadDigit(c)) }
    return Ok(int(c) - int('0'))
}

new function parse(s: string) -> Result<int, ParseError> {
    if s.len == 0 { return Err(Empty) }
    var n = 0
    for c in s {
        n = n * 10 + parse_digit(c)?
    }
    return Ok(n)
}

match parse("42") {
    Ok(v)  => print(v)
    Err(e) => print("bad input")
}
```
Enum variants are usable unqualified when unambiguous (`Ok`, `Empty`) and
qualified when not (`ParseError.Empty`).

## Generics
Generics are **monomorphized**: each instantiation compiles to its own
specialized code, with no boxing and no runtime dispatch. Constraints are
**interfaces**, which list the operators and functions a type must provide.
```
new interface Comparable {
    new operator < (a: Self, b: Self) -> bool
}

new function max<T: Comparable>(a: T, b: T) -> T {
    if a < b { return b }
    return a
}

new class List<T> {
    data: ptr<T>
    len:  usize
    cap:  usize

    new function push(item: T) { ... }
}

let xs = List<int>()     // explicit type arguments
let m  = max(3, 7)       // inferred: max<int>
```
Types satisfy interfaces **structurally**: if `int` defines `<`, it is
`Comparable`. You don't declare it. Multiple constraints: `T: Comparable + Hashable`.

## Control flow
```
if x > 5 { ... } else if x > 2 { ... } else { ... }
while i < 10 { i += 1 }
for i in 0..10 { ... }          // half-open range
break / continue
match value { pattern => expr_or_block ... }
```

## Low level
```
let p: ptr<i32> = &x
let v = *p
let arr: [u8; 16] = [0, 1, 2]
asm { "mov rax, 1" "syscall" }
```

## Lexical rules
- Files use the `.h2` extension.
- Comments: `// line` and `/* block */` (nestable).
- Numbers: `42`, `1_000`, `0xFF`, `0b1010`, `0o17`, `3.14`, `1e9`.
- No semicolons. A statement ends at the end of the line. `;` is allowed to
  put several statements on one line.
- **Line rule:** a binary operator, call `(`, index `[` or `?` must be on the
  same line as the expression before it. To continue an expression, break the
  line *after* an operator:
  ```
  let total = a +
              b          // ok
  let x = f
  (y)                    // two statements: `f`, then `(y)`
  ```
  `.member` may start a new line, for method chains.

## Operator precedence (high to low)
| Level | Operators                          |
|-------|------------------------------------|
| 1     | postfix: `()` `[]` `.` `?`         |
| 2     | prefix: `-` `!` `~` `&` `*` `alloc` |
| 3     | `*` `/` `%`                        |
| 4     | `+` `-`                            |
| 5     | `<<` `>>`                          |
| 6     | `&`                                |
| 7     | `^`                                |
| 8     | `\|`                              |
| 9     | `==` `!=` `<` `<=` `>` `>=`        |
| 10    | `&&`                               |
| 11    | `\|\|`                           |
| 12    | `..`                               |

Unlike C, the bitwise operators bind tighter than comparisons, so
`x & 1 == 0` means `(x & 1) == 0`.
Assignment (`=`, `+=`, ...) is a statement, not an expression.

## Entry point
```
use Elemental

new function main() -> int {
    print("Hello, Hydrogen")
    return 0
}
```

# Hydrogen Syntax: Draft 6

Status: **sketch**. Every rule here is up for discussion.

## Modules and imports
A module is a **directory** whose entry file is `init.h2`:
```
lib/Atomic/init.h2            -> module Atomic
lib/Atomic/Memory/init.h2     -> module Atomic.Memory
myproject/Geometry/init.h2    -> module Geometry
```
### Multi-file modules: `include`
A module's root file (`init.h2`, or an app's main file) can pull in other
files from the same folder with `include`:
```
Shapes/
  init.h2        include circle
                 include square
  circle.h2      pub new class Circle { ... }
  square.h2      pub new class Square { ... }
  Polygon/       <- a folder with its own init.h2 is a submodule: use Shapes.Polygon
    init.h2
```
- All included files form **one module** with one shared namespace. Private
  (non-`pub`) items in one file are visible in the others, but not outside the module.
- Only the root file may `include`, so `init.h2` lists every file of the module.
  Files in the folder that aren't included are not part of the module.
- Write the file name without quotes or `.h2`: `include circle`.
- **`use` is module-wide.** All `use` lines go in the root file and apply to
  every included file. Writing `use` in an included file is an error. That
  way the root file is the module's table of contents: its imports and its files.

### Finding modules
`use A.B` searches for `A/B/init.h2` in this order:
1. the directory of the program's main file (your project)
2. each dependency / `-L` library path
3. the bundled standard library

```
use Elemental                // pub names of Elemental are now in scope: print(), sqrt()
use Atomic.Memory as mem     // aliased: only mem.alloc_bytes(...), nothing unqualified
```

### Visibility: `pub`
Everything is **private to its module** unless marked `pub`:
```
pub new function distance(a: Point, b: Point) -> f64 { ... }
new function square(v: f64) -> f64 { ... }       // private helper

pub new class Point {
    pub x: f64          // visible outside the module
    cache: f64          // private field
}
```
- `pub` is allowed on top-level declarations, `use`, and type/class fields and methods.
- Operators are public whenever their type is. Enum variants are public whenever
  their enum is. Interface members are always public. Writing `pub` on these
  is an error.
- `pub use M` **re-exports** M's pub names. That's how `use Elemental` gives
  you `print`: `Elemental/init.h2` does `pub use Elemental.IO`.

### Name lookup
An unqualified name is looked up in this order:
1. local variables, parameters and generic parameters (innermost first)
2. declarations in the current module, including its enum variants
3. built-ins: `ptr`, and `intrinsic` (only inside Atomic)
4. pub names from every `use`d module

Declarations later in a module can be used earlier: order doesn't matter at
the top level. Inside a function, a local must be declared before it is used.

If two `use`d modules export the same name, using it unqualified is an error.
Write the qualified form instead: `Geometry.shared()`. A qualified name is
`<module you used>.<pub name>` or `<alias>.<pub name>`.

### The prelude
Every module except `Atomic` itself implicitly does `use Atomic`. That's
where `int`, `bool`, `f64`, `string`, `Result`, `Ok`, `Err`, `Option`, ...
come from.

## Variables
```
let x = 10          // immutable, type inferred
var y: u8 = 255     // mutable, explicit type
```
Function parameters are immutable, like `let`. Copy one into a `var` to change it.
A name can be re-declared in a nested block (shadowing), but not twice in
the same block.

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
new type Meters : float            // distinct; no implicit mixing with float
new type i64    = int              // alias: another name for the same type

new type string {
    data: ptr<u8>
    len:  uint
}
```
`:` makes a **new** type with the same representation. It inherits no
operators (define the ones you want) and converts only explicitly:
`Meters(x)`, `float(m)`. `=` makes an **alias**: the two names are the same
type, and an alias can't add operators.

#### Operators
A type may define **core-level operators** and nothing else. At the bottom,
Atomic implements them with compiler intrinsics (or inline `asm`):
```
new type i32 : bits(32) signed {
    new operator + (a: i32, b: i32) -> i32  { return intrinsic.add(a, b) }
    new operator - (a: i32, b: i32) -> i32  { return intrinsic.sub(a, b) }
    new operator == (a: i32, b: i32) -> bool { return intrinsic.eq(a, b) }
}

new type Meters : float {
    new operator + (a: Meters, b: Meters) -> Meters { return Meters(float(a) + float(b)) }
    new operator == (a: Meters, b: Meters) -> bool  { return float(a) == float(b) }
}
```
Overloadable operators: `+ - * / % == != < <= > >= & | ^ ~ ! << >> []`. A `-` or `!` operator with one parameter is the unary form.
`!=` is derived from `==` when not defined. Assignment and `&` (address-of)
cannot be overloaded.

#### Built-in primitives
| Type     | Size                 | Aliases          |
|----------|----------------------|------------------|
| `int`    | 64-bit signed        | `i64`            |
| `uint`   | 64-bit unsigned      | `u64`, `usize`   |
| `float`  | 64-bit float         | `f64`            |
| `i8 i16 i32`, `u8 u16 u32`, `f32` | sized |      |
| `bool`, `char`, `string` |      |                  |

Both forms are always available. Use `int`/`float` for everyday code and the
sized names when layout matters. Error messages use the friendly names.

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

## Type checking
- **Inference.** `let x = 10` is `int`, `let y = 1.5` is `float`. A literal
  takes its type from context when there is one: `let b: u8 = 200`, and the
  checker reports values that don't fit (`let b: u8 = 256`).
- **No implicit conversions** between named types. Convert explicitly:
  `int(c)`, `float(n)`, `Meters(x)`. Conversions work between number, bool and
  char types.
- **Operators come from the types.** `a + b` looks for `new operator +` on the
  operand types. Derived forms: `!=` from `==`; `>`, `<=` and `>=` from `<`.
- **Construction.** `Point(1.0, 2.0)` calls `init` if the class has one,
  otherwise it takes the fields in order. From another module, that needs a
  `pub init`, or all fields `pub`.
- **Generic arguments are inferred** from the arguments and the expected
  type: `max(3, 7)` is `max<int>`, and `return Err(e)` uses the return type.
  The type must structurally satisfy each constraint's interface.
- **`match` must be exhaustive**, including nested patterns
  (`Err(Empty)` plus `Err(BadDigit(c))` covers every `Err` of that enum).
- **Every path of a function with a return type must `return`.**
- **Mutability.** Assigning to a `let`, or to a field or element of one, is an error.
- `pub` is enforced on fields, methods and `init`.
- `ptr<T>`: `&x` takes an address, `*p` reads through it, `p.field` and `p[i]`
  dereference automatically. `alloc [T; N]` returns `ptr<T>`.
- `for` works over ranges (`a..b`), arrays, slices and strings (as `char`).

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

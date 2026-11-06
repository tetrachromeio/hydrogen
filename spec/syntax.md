# Hydrogen Syntax: Draft 2

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
Overloadable operators: `+ - * / % == != < <= > >= & | ^ ~ << >> [] unary-`.
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

## Control flow
```
if x > 5 { ... } else { ... }
while i < 10 { i += 1 }
for i in 0..10 { ... }
```

## Low level
```
let p: ptr<i32> = &x
asm { "mov rax, 1" }
```

## Entry point
```
use Elemental

new function main() -> i32 {
    print("Hello, Hydrogen")
    return 0
}
```

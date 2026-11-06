# Hydrogen Syntax: Draft 1

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
It has no identity, no inheritance and no hidden behavior. It is copied by
value and its layout is fully known. The built-in types are themselves
declared with `new type` in Atomic.

A type is built from raw bits or from an existing type:
```
// How Atomic defines primitives
new type bool : bits(1)
new type i32  : bits(32) signed
new type u8   : bits(8)
new type f64  : float(64)

// A distinct type built on an existing one; it does not mix with f64 implicitly
new type Meters : f64
```

Or a type can be a fixed layout of other types, like `string`:
```
new type string {
    data: ptr<u8>
    len:  usize
}
```

Built-in primitives: `i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool char usize`
(`int` = `i32`? to decide).

### `new class`: objects
A **class** groups data with behavior: methods, construction and RAII cleanup.
```
new class Buffer {
    data: ptr<u8>
    size: usize

    new function init(size: usize) {
        self.data = mem.alloc(size)
        self.size = size
    }

    new function drop() {           // runs automatically at end of scope
        mem.free(self.data)
    }

    new function get(i: usize) -> u8 {
        return self.data[i]
    }
}

let buf = Buffer(1024)
```

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

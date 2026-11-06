# Hydrogen Syntax: Draft 0

Status: **sketch**. Every rule here is up for discussion.

## Imports
```
use Elemental            // whole module
use Elemental.IO         // submodule
use Atomic.Memory as mem // alias
```

## Variables
```
let x = 10          // immutable, type inferred (i32)
var y: u8 = 255     // mutable, explicit type
```

Primitive types: `i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool char`,
`usize`, `void`.

## Functions
```
new function add(a: i32, b: i32) -> i32 {
    return a + b
}

new function log(msg: str) {   // no return type means void
    Elemental.IO.print(msg)
}
```

## Types
```
new type Vec2 {
    x: f32
    y: f32

    new function length(self) -> f32 {
        return Elemental.Math.sqrt(self.x * self.x + self.y * self.y)
    }
}

let v = Vec2 { x: 3.0, y: 4.0 }
let n = v.length()
```

### RAII
A type can define `drop`, which runs automatically when the value goes out
of scope:
```
new type Buffer {
    data: ptr<u8>
    size: usize

    new function drop(self) {
        Atomic.Memory.free(self.data)
    }
}
```

## Control flow
```
if x > 5 { ... } else { ... }
while i < 10 { i += 1 }
for i in 0..10 { ... }
```

## Low level
```
let p: ptr<i32> = &x      // raw pointer
let v = p.read()          // or *p, to be decided
asm { "mov rax, 1" }      // inline assembly
```

## Entry point
```
use Elemental

new function main() -> i32 {
    Elemental.IO.print("Hello, Hydrogen")
    return 0
}
```

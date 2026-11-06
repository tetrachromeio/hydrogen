# Hydrogen Cheat Sheet

Hydrogen is element 1: a systems language between assembly and C++, with
friendlier syntax. Source files end in `.h2`. Everything here works today;
anything not yet supported is listed at the end.

---

## 1. Tools

| Command | What it does |
|---|---|
| `hy new <name>` | Create an app in `./<name>` (`--lib` for a library) |
| `hy check` | Check the whole project: syntax, names, types, ownership |
| `hy run` | Build and run (`hy run -- args` passes arguments) |
| `hy build` | Build to `build/<name>` (`--release` for `-O2`) |
| `hy deps` | List resolved dependencies |
| `h2c --types file.h2` | Print the inferred type of every local |
| `h2c --emit-llvm file.h2` | Print the generated LLVM IR |
| `h2c --ast file.h2` | Print the syntax tree |

`hy` finds the project by walking up from the current folder to `hy.toml`.
`hy build` needs `clang` (or set `H2_CC`).

### `hy.toml`
```toml
[project]
name = "hello"            # also the module name, for libraries
version = "0.1.0"
type = "app"              # or "lib"
entry = "main.h2"         # default: main.h2 (app) / init.h2 (lib)

[dependencies]
Geometry = { path = "../Geometry" }   # another hy project with type = "lib"
```

---

## 2. Modules

```
myapp/
  hy.toml
  main.h2              <- root file of the app
  helpers.h2           <- part of the app if main.h2 says: include helpers
  Shapes/
    init.h2            <- module Shapes        (use Shapes)
    circle.h2          <- part of Shapes if Shapes/init.h2 says: include circle
    Polygon/
      init.h2          <- module Shapes.Polygon (use Shapes.Polygon)
```

| Syntax | Meaning |
|---|---|
| `use Elemental` | Bring a module's `pub` names into scope (`print(...)`) |
| `use Atomic.Memory as mem` | Aliased: only `mem.alloc_bytes(...)` |
| `pub use Elemental.IO` | Re-export: users of this module get IO's names |
| `include circle` | Add `circle.h2` (same folder) to this module |
| `pub new function f() {}` | Visible to other modules (everything is private by default) |

- `use` and `include` go **only in the root file** (`init.h2` / `main.h2`);
  `use` applies to every file of the module.
- Every module implicitly does `use Atomic` (int, string, Result, ...).
- `use A.B` looks for `A/B/init.h2` in: your project, dependencies, the standard library.
- Private items are shared across a module's files, but not outside it.
- Top-level order doesn't matter; locals must be declared before use.

---

## 3. Basics

```
// line comment        /* block comment, /* nestable */ */

let x = 10              // immutable, type inferred (int)
var y: u8 = 255         // mutable, explicit type
y = 7
x = 3                   // error: 'x' is immutable
```

- One statement per line. `;` is optional, for several statements on one line.
- **Line rule:** to continue an expression, break the line *after* an operator.
  A line starting with `(`, `[`, `?` or a binary operator starts a new statement.
  A line starting with `.member` continues a method chain.

### Literals
| Kind | Examples |
|---|---|
| Integers | `42`, `1_000_000`, `0xFF`, `0b1010`, `0o17` |
| Floats | `3.14`, `1e9`, `2.5e-3` |
| Bool | `true`, `false` |
| Char | `'a'`, `'\n'`, `'\x41'`, `'\''` |
| Text (`str`) | `"hi\tthere\n"` |
| Array | `[1, 2, 3]` |
| Range | `0..10` (excludes 10) |

Escapes: `\n \t \r \0 \\ \' \" \xHH`

### Primitive types
| Type | Meaning | Aliases |
|---|---|---|
| `int` | 64-bit signed | `i64` |
| `uint` | 64-bit unsigned | `u64`, `usize` |
| `float` | 64-bit float | `f64` |
| `i8 i16 i32`, `u8 u16 u32`, `f32` | sized | |
| `bool`, `char` | char is a 32-bit Unicode code point | |
| `str` | borrowed text (pointer + length) | |
| `string` | owned, growable text | |
| `ptr<T>`, `[T; N]`, `[T]` | pointer, fixed array, slice | |

- Literals take their type from context: `let b: u8 = 200` works,
  `let b: u8 = 256` is an error. With no context they're `int` / `float`.
- **No implicit conversions.** Convert explicitly: `int(c)`, `float(n)`,
  `u8(x)`, `Meters(x)`.

---

## 4. Operators

Precedence, high to low:

| Level | Operators |
|---|---|
| postfix | `f()` `a[i]` `a.b` `x?` |
| prefix | `-x` `!x` `~x` `&x` `*p` `alloc` |
| multiply | `*` `/` `%` |
| add | `+` `-` |
| shift | `<<` `>>` |
| bitwise | `&` then `^` then `\|` |
| compare | `==` `!=` `<` `<=` `>` `>=` |
| logic | `&&` then `\|\|` |
| range | `..` |

- Bitwise binds tighter than comparison: `x & 1 == 0` is `(x & 1) == 0`.
- Assignment is a statement: `=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`.
- Every operator is a declaration on a type (see §6). Derived for free:
  `!=` from `==`; `>`, `<=`, `>=` from `<`.

---

## 5. Functions

```
new function add(a: int, b: int) -> int {
    return a + b
}

new function log(msg: str) {          // no "->" means no return value
    print(msg)
}
```

- **Parameters are borrowed:** the function can read them; the caller keeps them.
- **`own`** takes ownership (needed to store or keep the value):
  `new function keep(item: own string) { ... }`
- Parameters are immutable. Copy into a `var` to change one.
- Every path of a function with a return type must `return`.
- The program starts at `new function main() -> int` (0 = success).

---

## 6. Types: `new type`

A **type** is plain data with value semantics: like `int`, `bool` or `str`.
No methods. It can define **operators** only.

```
new type Flags : bits(8)               // raw bits (add `signed` for signed)
new type Half  : float(32)             // a float
new type Meters : float                // NEW type, float's representation
new type Count = int                   // ALIAS: just another name for int

new type Pair { first: int, second: int }   // a fixed layout of fields

new type Meters2 : float {
    new operator + (a: Meters2, b: Meters2) -> Meters2 { return Meters2(float(a) + float(b)) }
    new operator < (a: Meters2, b: Meters2) -> bool { return float(a) < float(b) }
    new operator string (m: Meters2) -> string { return string(float(m)) + " m" }
}
```

- `:` makes a distinct type: no inherited operators, explicit conversions only.
- `=` makes an alias, which can't add operators.
- Overloadable operators: `+ - * / % == < & | ^ ~ ! << >> []`, plus unary `-`/`!`/`~` (one parameter).
- `operator string` is how a type prints (like Python's `__str__`).
- Layout types are built field by field, in order: `Pair(1, 2)`.

---

## 7. Classes: `new class`

A **class** is data plus behavior: fields, methods, `init`, `drop`.
Composition only: no inheritance, no virtual methods.

```
pub new class Account {
    pub owner: string           // pub: readable/writable from other modules
    balance: int                // private to this module

    pub new function init(owner: str) {   // called by Account("ada")
        self.owner = string(owner)
        self.balance = 0
    }

    pub new function deposit(amount: int) { self.balance += amount }
    pub new function total() -> int { return self.balance }

    new function drop() {                 // runs automatically at end of scope
        print("closing " + self.owner)
    }

    new operator string (a: Account) -> string { return a.owner + ": " + string(a.balance) }
}

let acct = Account("ada")
acct.deposit(50)
print(acct.total())
print(acct)
```

- Without `init`, construction takes the fields in order: `Point(1.0, 2.0)`.
  From another module that requires every field to be `pub` (or a `pub init`).
- `self` is available in methods (not in operators: those take explicit parameters).
- An object is zero-filled before `init` runs.
- `Self` inside a type/class/interface means "this type".

---

## 8. Enums and `match`

```
new enum Shape {
    Circle(float)
    Rect(float, float)
    Empty
}

new function area(s: Shape) -> float {
    match s {
        Circle(r) => { return 3.14159 * r * r }
        Rect(w, h) => { return w * h }
        Empty => { return 0.0 }
    }
}

let label = match area(Rect(2.0, 3.0)) {
    0.0 => "nothing"
    _ => "something"
}
```

- Variants can be used bare (`Circle(1.0)`) or qualified (`Shape.Circle(1.0)`).
- Patterns: variants, nested variants, literals, `_`, and names that bind values.
- `match` must cover every case (the compiler names what's missing).
- `match` can produce a value. Arms are `pattern => expr` or `pattern => { block }`.
- Match bindings are borrowed views of the matched value.

---

## 9. Errors: `Result`, `Option`, `?`

```
new enum ParseError { Empty, BadChar(char) }

new function digit(c: char) -> Result<int, ParseError> {
    if c < '0' || c > '9' { return Err(BadChar(c)) }
    return Ok(int(c) - int('0'))
}

new function parse(s: str) -> Result<int, ParseError> {
    if s.len == 0 { return Err(Empty) }
    var n = 0
    for c in s {
        n = n * 10 + digit(c)?    // ? : unwrap Ok, or return the Err right away
    }
    return Ok(n)
}
```

- `Result<T, E>` = `Ok(T) | Err(E)`, `Option<T>` = `Some(T) | None` (both in Atomic).
- `?` needs the function to return the same kind (`Result<_, E>` with the same `E`, or `Option`).
- No exceptions; errors are values.

---

## 10. Generics and interfaces

```
new interface Ordered {
    new operator < (a: Self, b: Self) -> bool
}

new function biggest<T: Ordered>(a: T, b: T) -> T {
    if a < b { return b }
    return a
}

new class Stack<T> {
    items: ptr<T>
    count: uint
    new function push(item: own T) {
        self.items[self.count] = item
        self.count += 1
    }
}

let m = biggest(3, 9)            // T inferred as int
let s = Stack<int>(alloc [int; 8], 0)
```

- Generics are compiled per concrete type (monomorphized): no runtime cost.
- Constraints are **structural**: any type with the interface's operators/methods
  satisfies it. Combine with `+`: `<T: Ordered + Printable>`.
- Explicit type arguments: `biggest<u8>(1, 2)`, `Stack<int>(...)`.

---

## 11. Control flow

```
if x > 5 { ... } else if x > 2 { ... } else { ... }

while i < 10 { i += 1 }
while true { if done { break } }

for i in 0..10 { ... }          // range (end excluded)
for x in [1, 2, 3] { ... }      // array or slice elements
for c in "text" { ... }         // characters of a str/string (bytes for now)

break      continue      return value
```

Conditions must be `bool` (no "truthy" numbers).

---

## 12. Memory and pointers

```
let p = Point(1.0, 2.0)          // stack: cleaned up at end of scope
let h = alloc Point(3.0, 4.0)    // heap: h is ptr<Point>
print(h.x)                        // . and [] go through pointers automatically
free h                            // runs drop, then releases the memory

let buf = alloc [u8; 4096]        // heap array: ptr<u8>, zero-filled
buf[0] = 65
free buf

var n = 5
let pn = &n                       // address of: ptr<int>
let v = *pn                       // read through a pointer

let arr: [u8; 16] = [1, 2, 3]     // fixed array (rest zero-filled)
let view: [u8] = arr              // slice of it
print(arr.len)                    // arrays and slices have .len
```

- Nothing goes on the heap unless you write `alloc`.
- `free` is explicit for anything you `alloc`.
- Writing through a raw pointer (`p[i] = v`, `*p = v`) doesn't drop the old value.

---

## 13. Ownership

A type **needs cleanup** if it is a class with `drop` (like `string`) or contains one.
Such values have exactly one owner and are **moved**, never silently copied.

```
let a = string("hi")
let b = a              // a moves into b
print(a)               // error: 'a' was moved
let c = b.copy()       // explicit copy
```

| Moves the value | Borrows the value |
|---|---|
| `let x = v`, `x = v` | passing to a normal parameter |
| `return v` | `match v`, `for x in v` |
| `Some(v)`, `Point(v, ...)` | operators (`a == b`) |
| passing to an `own` parameter | method calls (`v.len()`) |

The compiler checks the rules, following every branch and loop:
- Using a value after a move is an error. So is using one that "may have been moved" on some path.
- Assigning a new value to a `var` makes it usable again.
- A value moved inside a loop must be replaced before the next iteration.
- You can't move out of borrowed things (parameters, `self`, match/for bindings) or out of fields.
- Returning a `str` view of a local `string` is an error.
- Generic code is checked for every concrete type it's used with.

**Cleanup order:** reverse order of declaration at scope end. `return`, `break` and
`continue` clean up what they leave. Temporaries are dropped at the end of the statement.
Reassigning drops the old value.

---

## 14. Text

| | `str` | `string` |
|---|---|---|
| What | borrowed view (pointer + length) | owned, growable, freed automatically |
| From | string literals, `s.view()` | `string("...")`, `string(42)`, `a + b` |
| Kind | `type` | `class` |

```
let name: str = "Ada"
let owned = string(name)              // copy into an owned string (allocates)
let line = "x = " + string(42)        // + builds a new string
let v: str = owned                    // string -> str is automatic (free)
owned.len()   owned.view()   owned.copy()   owned.append("!")
s.len   s.data   s[0]                 // str fields / bytes
print(42)   print(3.5)   print(true)   print("hi")   print(line)
```

- `str` -> `string` is always explicit (it allocates). `string` -> `str` is automatic.
- Functions that only read text should take `str`.
- `print(x)` works for any type with `operator string`.

---

## 15. Standard library

Always available (`Atomic`, the prelude):

| Name | What |
|---|---|
| `int uint float`, sized ints/floats, `bool char str string` | types, with their operators and `operator string` |
| `Result<T, E>` (`Ok`, `Err`), `Option<T>` (`Some`, `None`) | error handling |
| `Printable` | interface: has `operator string` |
| `string`: `init(str)` `len()` `view()` `copy()` `append(str)` `+` `==` `[]` | owned text |

| Module | Functions |
|---|---|
| `use Elemental` | everything below |
| `Elemental.IO` | `print(x)`, `eprint(x)` (stderr) |
| `Elemental.Math` | `sqrt` `floor` `ceil` `abs` (float); `max` `min` (any `Ordered`); `Ordered` |
| `Atomic.Memory` | `alloc_bytes(n)` `free_bytes(p)` `copy(dst, src, n)` `fill(dst, byte, n)` |
| `Atomic.System` | `write(fd, data, len)` `exit(code)` |
| `Atomic.Math` | `sqrt` `floor` `ceil` |

Inside Atomic only: `intrinsic.add(a, b)` and friends, which map directly to machine
operations (`add sub mul sdiv srem eq slt shl ...`, `malloc free memcpy`, ...).

---

## 16. Not supported yet

- `null` pointers
- Reading input: keyboard, files, command-line arguments
- Built-in growable `List<T>` / `Map<K, V>`
- String interpolation (`"x = {x}"`); write `"x = " + string(x)`
- `if` as an expression / ternary (use `match`)
- Functions as values, closures
- Inline `asm` (parses, but the code generator rejects it)
- UTF-8 decoding in `for c in text` (iterates bytes)
- Full borrow checking of `str` views (only returning one is caught)
- Random numbers, time, threads

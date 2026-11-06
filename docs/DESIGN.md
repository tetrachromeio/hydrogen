# Design Notes

## Decided
| Area              | Decision                                         |
|-------------------|--------------------------------------------------|
| Execution model   | Ahead-of-time native compiler, LLVM backend      |
| Implementation    | C++                                              |
| Type system       | Static, with type inference                      |
| Domain / memory   | Systems language; manual memory + RAII, no GC    |

## Open
- Syntax style
- Generics / polymorphism model
- Error handling (exceptions vs. result types)
- Module / package system
- C interop
- File extension

# munx — Compiler, Bytecode Tools & VM

Front-end for **munx**, a systems language where concurrency and I/O primitives
appear as first-class syntax. Grammar is derived from the programs in
`sample/`. A full PDF language specification lives at
[`munx lang spec/munx-language-specification.pdf`](munx%20lang%20spec/munx-language-specification.pdf)
(regenerate with `python3 "munx lang spec/generate_spec.py"`).

This repo compiles munx source into versioned `.mxb` stack-machine bytecode,
validates and disassembles those images, and **executes them** on a stack VM
with threads, pipes, locks, sockets, and file I/O.

See [`BYTECODE_OPCODE_REFERENCE.md`](BYTECODE_OPCODE_REFERENCE.md) for opcode
numbers, operand encodings, stack effects, container compatibility rules, and
the reference interpreter's semantics.

---

## Build & run

### Linux / macOS

```bash
./build.sh              # delegates to compile.sh (release, -fno-exceptions)
./compile.sh            # explicit release build with extensive warning/opt flags
BUILD_TYPE=debug ./compile.sh
BUILD_TYPE=sanitize ./compile.sh

./munxc sample/logscope/main.mx                      # emit main.mxb
./munxc --run sample/logscope/main.mx access.log     # compile, then execute (JIT default)
./munxc --run --interp sample/logscope/main.mx access.log  # force interpreter
MUNX_VM_JIT=0 ./munxc --run sample/logscope/main.mxb access.log
./munxc --decompile sample/logscope/main.mxb         # reconstruct pseudo-Munx
./munxc --decode sample/logscope/main.mxb            # low-level disassembly
./munxc --ast sample/logscope/main.mx                # print AST
./munxc --tokens sample/logscope/main.mx             # print tokens
./munxc --files sample/io.mx sample/process_.mx      # compile several files
```

**macOS notes**

- Requires Xcode Command Line Tools (`xcode-select --install`) or Homebrew LLVM/Clang.
- **Apple Silicon (arm64):** builds and runs natively; SIMD bytecode ops need AVX2 and fail at runtime with a clear error (use scalar code paths on arm64).
- **Intel Mac (x86_64):** add `-mavx2 -mfma` for SIMD performance, or use `./build.sh` which adds them automatically on x86_64.
- **Named pipes / pipe hub** work on macOS (Unix domain sockets + `posix_spawn`).

Or with CMake:

```bash
cmake -S . -B build -G Ninja -DMUNX_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/munxc --run tests/programs/hello.mx
```

### Tests

GoogleTest drives `munxc` as a subprocess to verify compile + run:

```bash
cmake -S . -B build -G Ninja -DMUNX_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Runnable fixtures live in `tests/programs/`. CI is defined in
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

### Windows

Build with **MSVC** (Winsock is linked automatically):

```bat
build.bat
```

Or with CMake:

```bat
cmake -S . -B build
cmake --build build --config Release
build\Release\munxc.exe --run sample\valid\01_package_imports.mx
```

Cross-platform notes:

- **Sockets** (`open(io_type::socket, …)`, `bind`, `listen`, `accept`) work on Windows (Winsock), Linux, and macOS.
- **Named pipes / pipe hub** (`pipe(name, in|out|subscribe)`) work on Linux, macOS, and Windows. Linux/macOS can fall back to POSIX FIFOs when `$MUNX_PIPE_HUB=0`; on Windows the hub is required (Named Pipes at `\\.\pipe\munx-hub-<hash>`).
- **`process.id()`** returns the OS process ID on all platforms.
- **SIMD** requires AVX2 (x86_64 only); Apple Silicon builds without AVX and reports a runtime error if SIMD ops are used.

All of these must parse cleanly:

```bash
for f in sample/*.mx; do ./munxc "$f" >/dev/null && echo OK $f; done
```

---

## Running programs

`--run` executes a program on the VM in `include/vm.hpp`. A `.mx` argument is
compiled to `.mxb` first; every argument after the program path becomes `argv`.
Imported packages run their initializers before the entry package, and the
process exits with 1 after an uncaught runtime error.

By default the VM uses a **threaded JIT**: bytecode is optimized (constant
folding, dead `POP` removal, jump threading), compiled into a direct dispatch
table, and cached per function body. Use `--interp` or `MUNX_VM_JIT=0` to force
the reference switch-dispatch interpreter in `include/vm.hpp`.

The JIT uses dense handler indexing and fall-through chaining so sequential
bytecode avoids hash-map lookups between instructions. Conditional jumps
(`JMP_IF_*`) use a hybrid branch predictor (global-history perceptron + bimodal
counter + dedicated 256-entry loop/back-edge table keyed by `branch_pc >> 4`)
in `include/jit/branch_predictor.hpp` to prefetch the next handler.

**Profile-guided optimization (runtime PGO)** is always on unless disabled:

| Variable | Default | Effect |
|---|---|---|
| `MUNX_VM_PGO` | on | Specialize strongly biased branches; re-JIT when runtime profile matures |
| `MUNX_VM_JIT_WARMUP` | `0` | Interpret N invocations per function before JIT (seeds profile + predictor) |

The interpreter records branch/block profiles into `include/jit/execution_profile.hpp`.
On JIT compile, static back-edge seeding plus profile data warm-start the
predictor; mature profiles drive bytecode specialization in
`include/jit/pgo_optimizer.hpp` (remove never-taken branches, fold always-taken
into unconditional jumps). During JIT execution, per-unit `runtime_profile`
counters trigger recompilation when a function is invoked again after enough
branch samples.

```bash
# Branch microbenchmarks (disable pipe hub for clean timing)
MUNX_PIPE_HUB=0 ./tools/bench_branch.sh
MUNX_PIPE_HUB=0 ./munxc --run sample/bench/branch_count.mx
MUNX_PIPE_HUB=0 ./munxc --run sample/bench/branch_nested.mx
MUNX_VM_JIT_WARMUP=1 ./munxc --run sample/bench/branch_mixed.mx
```

```bash
./munxc --run sample/logscope/main.mx access.log --output report.txt
./munxc --run sample/chatrelay/main.mx --log chatrelay.log
```

### Prelude

Builtins are ordinary globals, so a program may shadow them (as `sample/io.mx`
does with `print = fix(open(io_type::tty, out))`; an `out` handle is callable
and writes its arguments).

| Group | Names |
|-------|-------|
| Console | `print`, `println`, `readln`, `fail`, `fix` |
| Strings | `concat`, `trim`, `split`, `has_substring_regex`, `len` |
| Collections | `queue`, `append`, `push`, `pop`, `remove_at` |
| I/O | `open`, `read`, `write`, `close`, `bind`, `listen`, `accept` |
| Concurrency | `thread`, `join`, `pipe`, `sleep` |
| Environment | `argv`, `process.id()`, `in`, `out`, `subscribe` |

`pipe(name, in|out|subscribe)` opens a named pipe through the munx pipe hub
(default directory: `$MUNX_PIPE_DIR` or `<tmpdir>/munx-pipes/`). The hub starts
automatically on the first VM that uses pipes and exits when the last VM
disconnects. `pipe(name, in)` is a competing-consumer queue; `pipe(name,
subscribe)` delivers a copy of every message to each subscriber; `pipe(name,
out)` publishes. Values are serialized so separate VM processes can exchange
data. Set `$MUNX_PIPE_HUB=0` to use direct POSIX FIFOs on Linux/macOS (not supported on Windows).

See `sample/pipehub/` for a multi-process pub/sub demo.

`open` selects the handle type from its `io_type` enum member:
`io_type::tty` with `in` / `out` yields a `term`, `io_type::file` with a path
and a mode yields a `file`, and `io_type::socket` with `af_type` / `sock_type`
members yields a `socket`.

### Runtime model

- **Values** — `int`, `float`, `bool`, `character`, `string`, regex, enum
  members, arrays, tuples, objects, buffers, functions, handles, pipes, locks,
  and threads. Arrays, tuples, and objects are reference values.
- **Scope** — functions read and write package globals directly; a name that is
  new inside a function stays local to that call.
- **Concurrency** — `thread` maps to an OS thread over shared globals. Pipes
  route through a singleton pipe hub across VM processes (`pipe(name, in)` for
  work queues, `pipe(name, subscribe)` for pub/sub, `pipe(name, out)` for
  writers). Values are serialized (`null` is the conventional sentinel). Locks
  are non-reentrant and releasable by any thread.
- **Errors** — `fail(...)`, bad casts, bad indexes, division by zero, and I/O
  failures raise trappable errors that unwind to the nearest `monitor` / `trap`
  in the current frame, then to the caller.

---

## Pipeline

```
.mx source  →  lexer  →  tokens  →  parser  →  ast::program
                 │                      │
           include/lexer.hpp      include/parser.hpp
           include/token.hpp      include/ast.hpp
```

| Piece | File |
|-------|------|
| Keywords | `include/keywords.hpp` |
| Tokens | `include/token.hpp` |
| Lexer | `include/lexer.hpp` |
| AST | `include/ast.hpp` |
| Parser | `include/parser.hpp` |
| Debug printer | `include/ast_printer.hpp` |
| Bytecode compiler | `include/bytecode_compiler.hpp` |
| Opcodes | `include/Opcode.hpp` |
| Disassembler / decompiler | `include/bytecode_decoder.hpp`, `include/bytecode_decompiler.hpp` |
| VM values | `include/vm_value.hpp` |
| VM loader, interpreter, prelude | `include/vm.hpp` |
| JIT optimizer & threaded dispatch | `include/jit/` |
| CLI | `src/main.cpp` |

---

## Sample-driven grammar (what the samples actually use)

### Module

```
package <name>
load_package <name>                 # single import
load_packages {name, name, ...}     # braced multi-import
```

Imports form a header after `package`: both forms may repeat and interleave,
but only before the first non-import statement. Later or nested imports are
a compile error.

### Declarations

```
func name(param: type, ...): type { ... }
enum Name { Member, Member }
object Name { field: type, ... }
lock name;
```

### Control flow

```
if expr { ... } else if expr { ... } else { ... }
loop { ... }
loop expr { ... }
break
return
return expr
match expr { case Enum::Member { ... } }
monitor { ... } trap(name: type) { ... }
```

### Assignments & destructuring

```
x = expr
x += expr
a, b = expr
{a, b} = expr
{a, _} = expr
```

### Expressions (from samples)

| Construct | Example |
|-----------|---------|
| Literals | `42`, `15.45`, `"hi"`, `'a'`, `true`, `false`, `null`, `r"..."` |
| Calls | `print(x)`, `Person(...)`, `open(...)` |
| Member / index | `argv.len`, `array[i]`, `process.id` |
| Enum access | `Gender::Male`, `io_type::socket` |
| Arithmetic / compare | `+ - * / %`, `== != < > <= >=`, `!` |
| Arrays / tuples | `[1,2,3]`, `[int][1,2,3,4]`, `{a, b}` |
| Lambdas | `lambda (): void => { ... }`, IIFE `lambda ... => { ... }()` |
| Pipes | `"hi" -> in_pipe`, `<- out_pipe` |
| Memory | `alloc [100] [1,2,3]`, `delete buff` |
| Cast | `cast[int](str)` |
| Join | `join [t1, t2]` |

### Structural keywords

Only these are tokenized as `KEYWORD` (so builtins like `print` / `in` / `out`
remain assignable identifiers):

```
package load_package load_packages func lambda return if else loop break
match case enum object tuple alloc delete free
lock acquire release join monitor trap cast fail
```

Builtins (`print`, `open`, `thread`, `pipe`, `concat`, …) are ordinary
`SYMBOL`s that your VM / prelude binds by name.

---

## AST summary (`munx::ast`)

Nodes use an explicit discriminator enum plus a `std::variant` payload:

```cpp
struct expr_node {
    expr_type type;
    std::variant<int_literal, float_literal, /* … */, lambda_expr> value;
};

struct stmt_node {
    stmt_type type;
    std::variant<assignment_stmt, expr_stmt, /* … */, load_package_stmt> value;
};

struct type_node {
    type_kind type;
    std::variant<primitive_type, named_type, array_type, tuple_type> value;
};
```

Helpers: `make_expr_ptr(...)`, `make_stmt_ptr(...)`, `as<T>(expr)`, `as_stmt<T>(stmt)`.
`type` is kept in sync with `value.index()`.

### Root

```cpp
struct program {
    std::string package_name;
    std::vector<load_package_stmt> imports;
    std::vector<std::unique_ptr<stmt_node>> statements;
};
```

### Types (`type_kind`)

| Kind | Meaning |
|------|---------|
| `Primitive` | built-in scalar / I/O / control types (see below) |
| `Named` | user types (`Person`, `Gender`, …) |
| `Array` | `[T]` |
| `Tuple` | `tuple[T1, T2, …]` |

#### Primitive types (`primitive_kind`)

Each primitive is stored as `ast::primitive_type{kind}` inside a `type_node`.
In source they appear in parameter, return, field, cast, and trap annotations.

| Kind | Source name | Role |
|------|-------------|------|
| `Int` | `int` | Signed 64-bit integer. Literals like `42`; arithmetic and indexing use this. |
| `Float` | `float` | Floating-point number. Literals like `15.45`; result of float arithmetic / casts. |
| `Bool` | `bool` | Boolean. Literals `true` / `false`; conditions for `if`, `loop`, and `&&` / `\|\|` / `!`. |
| `String` | `string` | Text. Literals `"..."`, including escapes (`\n`, `\t`, `\a`, `\\`, `\"`, `\'`). |
| `Character` | `character` | Single character. Literals `'a'`, `'\n'`, etc. |
| `Void` | `void` | No value. Function / lambda return type when nothing is returned. |
| `Socket` | `socket` | Network socket handle from `open(io_type::socket, ...)`. Used with `bind`, `listen`, `accept`, `read`, `write`, `close`. Appears in signatures such as `client_addr: socket`. |
| `File` | `file` | Filesystem handle from `open(io_type::file, path, in\|out)`. Supports `read` / `write` / `close` according to open mode (`write` fails on read-only opens). |
| `Term` | `term` | Terminal / TTY handle from `open(io_type::tty, ...)`. Globally fixed handles such as `print`, `println`, and `readln` are `term` objects (often wrapped in `fix(...)` so they cannot be reassigned). |
| `Exception` | `exception` | Error object delivered to a `monitor` / `trap` handler, e.g. `trap(error_object: exception)`. |

`Socket`, `File`, and `Term` are the three I/O object types produced by `open`.
The `io_type` enum member selects which one:

| `open` kind | Result type |
|-------------|-------------|
| `io_type::socket` | `socket` |
| `io_type::file` | `file` |
| `io_type::tty` | `term` |

Enum members such as `io_type::file` remain ordinary enum access expressions;
only type *positions* (`param: file`, `cast[file](...)`, object fields, etc.)
resolve to the primitive.

Casts (`cast[T](expr)`) are only defined for primitive types; other conversions
must be written by the user.

### Expressions (`expr_type`)

`IntLiteral` · `FloatLiteral` · `StringLiteral` · `CharLiteral` · `BoolLiteral` ·
`NullLiteral` · `RegexLiteral` · `Identifier` · `Binary` · `Unary` · `Call` ·
`Member` · `EnumAccess` · `Index` · `ArrayLiteral` · `TypedArrayLiteral` ·
`TupleLiteral` · `PipeInsert` · `PipeExtract` · `Cast` · `Alloc` · `Free` ·
`Lambda`

### Statements (`stmt_type`)

`Assignment` · `Expr` · `Return` · `Break` · `Block` · `If` · `Loop` · `Match` ·
`FuncDecl` · `EnumDecl` · `ObjectDecl` · `Monitor` · `Lock` · `Acquire` ·
`Release` · `LoadPackage`

### Operator precedence (high → low)

1. Postfix: `.` `()` `[]`
2. Unary: `!` `~` `-` `<-`
3. `*` `/` `%`
4. `+` `-`
5. `<` `>` `<=` `>=`
6. `==` `!=`
7. `&&`
8. `||`
9. `->` (pipe insert)
10. `=` `+=` (statement level)

---

## Example AST

Source (`process_.mx`):

```munx
package process_
in_pipe = pipe("pipe_id", in)
self_pid = process.id()
"Hello there" -> in_pipe
```

Printed:

```
(program
  (package process_)
  (assign =
    (targets (target in_pipe))
    (call (ident pipe) (string "pipe_id") (ident in)))
  (assign =
    (targets (target self_pid))
    (call (member id (ident process))))
  (expr
    (pipe-insert in_pipe (string "Hello there"))))
```

---

## How the front end lowers to bytecode

1. **No types on expr nodes** — types are only used where the source spells
   them out (casts, trap handlers, signatures).
2. **Object construction** is a `Call` to an `Identifier` (e.g. `Person(...)`);
   `DEFINE_OBJECT` registers the constructor at runtime.
3. **`process.id()`** is `Call(Member(Identifier("process"), "id"))`.
4. **Concurrency / I/O** (`thread`, `open`, `read`, …) are ordinary calls
   resolved by name against the prelude. The result type of `open` is
   `socket`, `file`, or `term` depending on the `io_type` argument.
5. **`join [a, b]`** lowers to `Call(Identifier("join"), [a, b])`.
6. Control flow lowers to stack ops + `JMP` / conditional jumps; `&&` and `||`
   short-circuit through conditional jumps.
7. The instruction set is `include/Opcode.hpp`, documented in
   [`BYTECODE_OPCODE_REFERENCE.md`](BYTECODE_OPCODE_REFERENCE.md).

---

## Layout

```
include/               headers (lexer, parser, AST, bytecode tools, VM)
sample/                .mx programs that define the accepted grammar
sample/valid/          exhaustive happy-path parse tests
sample/invalid/        one-file-per-error compile-fail tests
sample/chatrelay/      multi-file real-world project (TCP chat relay)
sample/logscope/       multi-file real-world project (concurrent log analyzer)
sample/shipyard/       multi-file order-fulfillment pipeline (verbose logging)
src/                   CLI driver
```

Compile and run the two multi-file projects with:

```bash
./munxc --files sample/chatrelay/*.mx
./munxc --run sample/logscope/main.mx sample/logscope/access.log
./munxc --run sample/chatrelay/main.mx --log chatrelay.log   # serves :7070
./munxc --run sample/shipyard/main.mx sample/shipyard/orders.csv --verbose --trace
```
# munchkinglang

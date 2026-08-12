# Munx Bytecode Opcode Reference

This document describes the instruction set emitted by
`munx::bytecode_compiler` for **MX bytecode format version 8**. The canonical
opcode declarations are in [`include/Opcode.hpp`](include/Opcode.hpp).

## Encoding conventions

Each instruction begins with a one-byte opcode (`uint8_t`), followed
immediately by its operands.

| Operand | Size | Meaning |
|---|---:|---|
| `u8` | 1 byte | Unsigned integer |
| `u32` | 4 bytes | Unsigned integer |
| `i64` | 8 bytes | Signed integer |
| `f64` | 8 bytes | IEEE-754 double |
| `str` | 8 bytes | `u32 offset` followed by `u32 length` |
| `type` | variable | Recursive type encoding described below |

A `str` offset is relative to the beginning of the image's string table. String
data is raw UTF-8 and is not null-terminated.

The current encoder writes scalar values with `memcpy`. Supported VM targets
are therefore expected to use little-endian scalar representation, matching
the compiler's current targets.

### Stack notation

Stack effects are written as:

```text
before -- after
```

The rightmost value is the top of the stack. For example:

```text
..., left, right -- ..., result
```

means the VM pops `right`, then `left`, and pushes `result`.

### Jump targets

Jump operands are `u32` absolute byte offsets within the current code blob.
They are not file offsets:

- a function jump is relative to the first byte of that function's bytecode;
- a package-initializer jump is relative to the first byte of that
  initializer.

The target must point to the beginning of an instruction.

### Type operands

`CAST` uses a recursive type operand. It starts with a `u8` `type_kind`:

| Tag | Kind | Following payload |
|---:|---|---|
| `0` | Primitive | `u8 primitive_kind` |
| `1` | Named | `str type_name` |
| `2` | Array | One nested `type` |
| `3` | Tuple | `u32 count`, followed by `count` nested `type` values |

Primitive tags follow `ast::primitive_kind`:

| Tag | Type |
|---:|---|
| `0` | `int` |
| `1` | `float` |
| `2` | `bool` |
| `3` | `string` |
| `4` | `character` |
| `5` | `void` |
| `6` | `socket` |
| `7` | `file` |
| `8` | `term` |
| `9` | `exception` |

## Constants

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x00` | `PUSH_INT` | `i64 value` | `... -- ..., value` | Push a signed integer literal. |
| `0x01` | `PUSH_FLOAT` | `f64 value` | `... -- ..., value` | Push a floating-point literal. Munx `long double` literals are narrowed to `double`. |
| `0x02` | `PUSH_STRING` | `str value` | `... -- ..., value` | Push a string literal. |
| `0x03` | `PUSH_CHAR` | `u8 value` | `... -- ..., value` | Push a character byte. |
| `0x04` | `PUSH_BOOL` | `u8 value` | `... -- ..., bool` | Push `false` for zero or `true` for one. |
| `0x05` | `PUSH_NULL` | — | `... -- ..., null` | Push the null value. |
| `0x06` | `PUSH_REGEX` | `str pattern` | `... -- ..., regex` | Push a regex value from its pattern text. |
| `0x07` | `PUSH_ENUM` | `str enum_name`, `str member_name` | `... -- ..., member` | Resolve and push an enum member. |
| `0x08` | `PUSH_FUNC` | `str function_name` | `... -- ..., function` | Push a function value. Currently emitted for compiled lambdas. |

## Stack operations

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x09` | `POP` | — | `..., value -- ...` | Discard the top value. |
| `0x0A` | `DUP` | — | `..., value -- ..., value, value` | Duplicate the top value. |
| `0x0B` | `SWAP` | — | `..., a, b -- ..., b, a` | Exchange the top two values. |

Stack underflow is a VM runtime error.

## Variables and symbols

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x0C` | `LOAD` | `str name` | `... -- ..., value` | Load a variable, declared function, or runtime builtin by name. |
| `0x0D` | `STORE` | `str name` | `..., value -- ...` | Store the top value under `name`. Function prologues emit `STORE` instructions for parameters in reverse declaration order. |

Name lookup and scope ownership are VM responsibilities.

## Arithmetic

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x0E` | `ADD` | — | `..., left, right -- ..., left + right` | Addition; also used for `+=`. |
| `0x0F` | `SUB` | — | `..., left, right -- ..., left - right` | Subtraction. |
| `0x10` | `MUL` | — | `..., left, right -- ..., left * right` | Multiplication; also repeats an array when the left operand is an array and the right is an integral count, and performs element-wise SIMD multiplication when both operands are SIMD vectors. |
| `0x11` | `DIV` | — | `..., left, right -- ..., left / right` | Division. |
| `0x12` | `MOD` | — | `..., left, right -- ..., left % right` | Remainder. |
| `0x13` | `NEG` | — | `..., value -- ..., -value` | Arithmetic negation. |

Numeric promotion, overflow, division-by-zero behavior, and supported
overloads are defined by the VM. Integer overflow and division by zero raise
typed `exception` values with `.kind` of `overflow` or `division_by_zero`.
Uncaught runtime errors print a stack trace with source locations when debug
maps are present in the bytecode image (format version 5+). Map and `Lambda[…]`
types are encoded in CAST operands (tags 4 and 5) since version 6.

## Comparison

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x14` | `EQ` | — | `..., left, right -- ..., bool` | Equal. |
| `0x15` | `NE` | — | `..., left, right -- ..., bool` | Not equal. |
| `0x16` | `LT` | — | `..., left, right -- ..., bool` | Less than. |
| `0x17` | `GT` | — | `..., left, right -- ..., bool` | Greater than. |
| `0x18` | `LE` | — | `..., left, right -- ..., bool` | Less than or equal. |
| `0x19` | `GE` | — | `..., left, right -- ..., bool` | Greater than or equal. |

## Logical and bitwise operations

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x1A` | `NOT` | — | `..., value -- ..., bool` | Logical negation. |
| `0x1B` | `BITWISE_AND` | — | `..., left, right -- ..., result` | Bitwise AND. |
| `0x1C` | `BITWISE_OR` | — | `..., left, right -- ..., result` | Bitwise OR. |
| `0x1D` | `BITWISE_XOR` | — | `..., left, right -- ..., result` | Bitwise XOR. |
| `0x1E` | `BITWISE_NOT` | — | `..., value -- ..., result` | Bitwise complement. |

Munx logical `&&` and `||` do not have dedicated opcodes. The compiler lowers
them to `DUP`, `JMP_IF_FALSE` or `JMP_IF_TRUE`, `POP`, and the right-hand
expression so evaluation short-circuits.

## Control flow and calls

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x1F` | `JMP` | `u32 target` | `... -- ...` | Jump unconditionally. |
| `0x20` | `JMP_IF_FALSE` | `u32 target` | `..., condition -- ...` | Pop the condition and jump when it is false. |
| `0x21` | `JMP_IF_TRUE` | `u32 target` | `..., condition -- ...` | Pop the condition and jump when it is true. |
| `0x3C` | `HINT_BRANCH` | `u8 expected_taken` | `... -- ...` | Seed the branch predictor for the immediately following `JMP_IF_*` (`1` = jump likely, `0` = fallthrough likely). Emitted for `likely` / `unlikely` on `if` conditions. |
| `0x22` | `CALL` | `u8 argc` | `..., callee, arg0, ..., argN-1 -- ..., result` | Invoke `callee` with `argc` arguments. A void call pushes `null`. |
| `0x23` | `RET` | — | `..., result -- ...` | Return the top value to the caller. |
| `0x24` | `HALT` | — | `... -- ...` | Finish a package initializer. |

The compiler emits an implicit `PUSH_NULL; RET` at the end of every function,
so `RET` always has a value available.

`if`, `loop`, `break`, and `match` are lowered into these jump instructions.
There is no separate block or loop opcode.

## Aggregates

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x25` | `MAKE_ARRAY` | `u32 count` | `..., element0, ..., elementN-1 -- ..., array` | Construct an array from `count` values. |
| `0x26` | `MAKE_TUPLE` | `u32 count` | `..., element0, ..., elementN-1 -- ..., tuple` | Construct a tuple from `count` values. |
| `0x27` | `INDEX_GET` | — | `..., object, index -- ..., value` | Read `object[index]`. |
| `0x28` | `MEMBER_GET` | `str member` | `..., object -- ..., value` | Read a named member. |
| `0x29` | `UNPACK` | `u8 count` | `..., aggregate -- ..., elementN-1, ..., element0` | Unpack an aggregate. Element zero must be left on top so following `STORE` instructions bind targets in source order. |

Typed and untyped array literals share `MAKE_ARRAY`; the original element-type
annotation is not retained in bytecode.

## Casts and memory

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x2A` | `CAST` | `type target` | `..., value -- ..., converted` | Convert a value to the encoded target type. |
| `0x2B` | `ALLOC` | `u32 count` | `..., capacity, init0, ..., initN-1 -- ..., buffer` | Allocate a buffer with `capacity` and `count` initializer values. |
| `0x2C` | `FREE` | `str buffer_name` | `... -- ..., null` | Release the named buffer and push `null`. |
| `0x2D` | `CLONE_OBJECT` | — | `..., value -- ..., copy` | Deep-copy user objects for by-value returns; pass all other values through unchanged. |
| `0x2E` | `MAKE_SIMD` | — | `..., array -- ..., simd` | Wrap a homogeneous primitive array as an AVX2-backed SIMD vector. |
| `0x2F` | `SIMD_TO_ARRAY` | — | `..., simd -- ..., array` | Materialize SIMD lanes back into a munx array. |
| `0x30` | `MAKE_MAP` | `u32 count` | `..., k0, v0, …, kN-1, vN-1 -- ..., map` | Build a map from `count` key/value pairs (any value type as key; value on top). |

Allocation failure, invalid casts, and double-free behavior are VM runtime
errors.

## Pipes

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x31` | `PIPE_INSERT` | `str pipe_name` | `..., value -- ..., null` | Write `value` to the named pipe and push `null`. |
| `0x32` | `PIPE_EXTRACT` | `str pipe_name` | `... -- ..., value` | Block until a value can be read from the named pipe. |

Each message is a little-endian `u32` payload length followed by a tagged
value encoding (scalars, strings, arrays, tuples, and objects). By default the
runtime routes pipes through a singleton hub in `$MUNX_PIPE_DIR` or
`<tmpdir>/munx-pipes/` (`hub.sock`). `pipe(name, in)` competes for queue
delivery; `pipe(name, subscribe)` receives broadcast copies; `pipe(name, out)`
publishes. The hub starts with the first VM client and exits when the last
disconnects. Set `$MUNX_PIPE_HUB=0` for legacy direct FIFO mode.
Handles, functions, locks, and threads cannot be sent through a pipe.

## Runtime type declarations

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x33` | `DEFINE_ENUM` | `str name`, `u32 count`, `count × str member` | `... -- ...` | Register an enum and its ordered member names. |
| `0x34` | `DEFINE_OBJECT` | `str name`, `u32 count`, `count × str field` | `... -- ...` | Register an object and its ordered field names. |

Field type annotations are not currently encoded by `DEFINE_OBJECT`.
Constructing an object is an ordinary `LOAD` plus `CALL`.

## Locks

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x35` | `LOCK_CREATE` | `str name` | `... -- ...` | Create a named lock. |
| `0x36` | `LOCK_ACQUIRE` | `str name` | `... -- ...` | Block until the named lock is acquired. |
| `0x37` | `LOCK_RELEASE` | `str name` | `... -- ...` | Release the named lock. |

Acquiring an unknown lock, releasing a lock not owned by the current thread,
and recursive acquisition behavior are VM-defined errors.

## Monitor and trap

| Code | Opcode | Operands | Stack effect | Description |
|---:|---|---|---|---|
| `0x38` | `MONITOR_ENTER` | `u32 handler` | `... -- ...` | Begin a protected region and register its trap handler offset. |
| `0x39` | `MONITOR_EXIT` | — | `... -- ...` | Leave the protected region normally. |

If an exception is raised after `MONITOR_ENTER`, the VM unwinds the protected
region, pushes the exception object, and jumps to `handler`. The first
instruction at the compiler-generated handler is normally `STORE trap_name`.
On normal completion, `MONITOR_EXIT` is followed by a `JMP` over the handler.

## `.mxb` container summary

The opcode streams are contained in a packed **version 8** `.mxb` image:

```text
mx_program_header
mx_package_descriptor[import_count]
for each bundled package, then the entry package:
    mx_function_descriptor[function_count]
    function bytecode blobs
    per-function debug map blobs (v5)
    package initializer bytecode
    package init debug map blob (v5)
string table
```

Each debug map blob (format version 5+) is:

```text
u32 entry_count
entry_count × (u32 pc, u32 line, u32 column, u32 file_offset, u32 file_length)
```

File offsets in debug entries are relative to the string table. The VM resolves
`pc → source location` via binary search on the highest `pc` not greater than
the fault PC.

All descriptor offsets are absolute file offsets. Name offsets are relative to
the string table. Descriptor structures are packed with
`__attribute__((packed))`.

The descriptors currently use `size_t`; therefore their width remains tied to
the compiler/VM architecture even though padding is removed. A VM must reject
an unsupported bytecode version before reading later fields.

## Compatibility rules

The bytecode version must be incremented when any of the following changes:

- an opcode is inserted, removed, reordered, or assigned a different value;
- an instruction operand layout or stack effect changes;
- a type or primitive tag changes;
- a packed descriptor layout changes; or
- string-table or jump-offset semantics change.

Readers should verify the `MX` signature and version before decoding
descriptors or instructions. The repository's `--decode` command additionally
checks all section bounds, string references, operands, and opcode values:

```bash
./munxc --decode program.mxb
```

## Reference interpreter

`include/vm.hpp` executes these images (`./munxc --run program.mxb`). Where the
tables above leave a behavior to the VM, the reference interpreter resolves it
as follows.

| Area | Reference behavior |
|---|---|
| Load order | Bundled packages run their initializers first, in dependency order, then the entry package. Every function of every package is published by name before any initializer runs. |
| Name lookup | `LOAD` searches the current frame's locals, then the shared package globals; an unresolved name is a runtime error. |
| Name binding | `STORE` writes an existing local, otherwise an existing global, otherwise creates a local. Initializer frames always write globals, so a function can update package state without redeclaring it. |
| Parameters | The leading `STORE` run of a function body is treated as its prologue, so parameters are pre-bound as locals and never shadow a same-named global. |
| Arithmetic | Two integral operands (`int`, `bool`, `character`) stay integral; any float operand promotes both to `float`. `ADD` concatenates when either side is a string and appends when the left side is an array. `MUL` repeats an array when the left side is an array and the right is integral, and multiplies SIMD vectors element-wise when both sides are SIMD values (AVX2 required). Division or modulo by zero raises a trappable error. |
| Comparison | `EQ` / `NE` compare numbers by value, strings, enum members, and modes structurally, and arrays, tuples, objects, and handles by identity. Ordering comparisons accept numbers or strings. |
| Truthiness | `false`, `0`, `0.0`, `""`, `'\0'`, and `null` are false; everything else is true. |
| `CAST` | Failures (`cast[int]("abc")`, mismatched tuple widths, unrelated named types) raise trappable errors. `cast[string]` uses the same rendering as `print`. |
| `ALLOC` / `FREE` | `FREE` releases the buffer bound to the named variable; releasing twice is an error, and freeing a non-buffer is a no-op that still pushes `null`. |
| Pipes | Hub-backed named pipes under `$MUNX_PIPE_DIR` or `<tmpdir>/munx-pipes/`. `pipe(name, out)` publishes; `pipe(name, in)` opens a queue reader; `pipe(name, subscribe)` opens a broadcast subscriber. Values are length-prefixed and serialized for cross-process exchange. The hub auto-starts on the first VM and exits when the last client disconnects. `PIPE_INSERT` blocks until a reader is connected; `PIPE_EXTRACT` blocks until a message arrives. EOF returns `null`. `$MUNX_PIPE_HUB=0` restores direct FIFO mode. |
| Locks | `LOCK_CREATE` binds a new lock to the named variable. Locks are not reentrant and are not owned by the acquiring thread, so `acquire` and `release` may live in different functions. |
| `MONITOR_ENTER` / `MONITOR_EXIT` | Handlers form a per-frame stack. A trap truncates the operand stack to the depth recorded at entry, pushes the error, and jumps to the handler. An error with no handler in the current frame propagates to the caller. |
| Threads | `thread(fn, {args})` runs `fn` on an OS thread over the same globals. An uncaught error in a thread is reported and fails the run without stopping other threads. The program joins every unjoined thread before exiting. |

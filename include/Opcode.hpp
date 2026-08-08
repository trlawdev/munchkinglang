#pragma once

#include <cstdint>

/// munx stack VM instruction set, as emitted by `munx::bytecode_compiler`.
///
/// Encoding: a 1-byte opcode followed by the little-endian operands listed
/// beside each instruction:
///   - `i64`  8-byte signed integer
///   - `f64`  8-byte IEEE-754 double
///   - `u8`   1 byte
///   - `u32`  4-byte unsigned integer
///   - `str`  u32 byte offset into the module string table + u32 byte length
///
/// Jump targets are absolute byte offsets within the current code blob
/// (one function body, or one package's top-level bytecode).
///
/// Type operands (CAST) are encoded recursively as a u8 `ast::type_kind`
/// tag (0 primitive, 1 named, 2 array, 3 tuple) followed by:
///   - primitive: u8 `ast::primitive_kind`
///   - named:     str type name
///   - array:     the encoded element type
///   - tuple:     u32 n, then n encoded element types
///   - map:       encoded key type, encoded value type
///   - lambda:    u32 n, n encoded param types, encoded return type
///
/// Every value-producing instruction pushes exactly one value, so an
/// expression statement always discards its result with a single POP.
enum class Opcode : uint8_t
{
    // ---- constants --------------------------------------------------------
    PUSH_INT,    ///< i64 — push an integer literal.
    PUSH_FLOAT,  ///< f64 — push a float literal.
    PUSH_STRING, ///< str — push a string literal.
    PUSH_CHAR,   ///< u8 — push a character literal.
    PUSH_BOOL,   ///< u8 (0 or 1) — push a boolean literal.
    PUSH_NULL,   ///< push null.
    PUSH_REGEX,  ///< str — push a regex literal (raw pattern text).
    PUSH_ENUM,   ///< str str — push enum member (enum name, member name).
    PUSH_FUNC,   ///< str — push a function value by name (used for lambdas).

    // ---- stack ------------------------------------------------------------
    POP,         ///< pop and discard the top of the stack.
    DUP,         ///< duplicate the top of the stack.
    SWAP,        ///< swap the two topmost stack values.

    // ---- variables --------------------------------------------------------
    LOAD,        ///< str — push the value of the named variable / function / builtin.
    STORE,       ///< str — pop the top of the stack into the named variable.

    // ---- arithmetic (pop right, pop left, push result) ---------------------
    ADD,         ///< push left + right.
    SUB,         ///< push left - right.
    MUL,         ///< push left * right.
    DIV,         ///< push left / right.
    MOD,         ///< push left % right.
    NEG,         ///< pop one value, push its arithmetic negation.

    // ---- comparison (pop right, pop left, push bool) ------------------------
    EQ, NE, LT, GT, LE, GE,

    // ---- logical / bitwise --------------------------------------------------
    NOT,         ///< pop one value, push logical not.
    BITWISE_AND, ///< pop two values, push bitwise and.
    BITWISE_OR,  ///< pop two values, push bitwise or.
    BITWISE_XOR, ///< pop two values, push bitwise xor.
    BITWISE_NOT, ///< pop one value, push bitwise complement.

    // ---- control flow --------------------------------------------------------
    JMP,          ///< u32 — unconditional jump.
    JMP_IF_FALSE, ///< u32 — pop condition, jump when false.
    JMP_IF_TRUE,  ///< u32 — pop condition, jump when true.
    CALL,         ///< u8 argc — stack: callee, arg0 … argN-1 → result (null for void).
    RET,          ///< return from function; top of stack is the return value.
    HALT,         ///< end of package top-level / entry-point bytecode.

    // ---- aggregates -----------------------------------------------------------
    MAKE_ARRAY,   ///< u32 n — pop n elements (last on top), push array.
    MAKE_TUPLE,   ///< u32 n — pop n elements (last on top), push tuple.
    INDEX_GET,    ///< pop index, pop object, push object[index].
    MEMBER_GET,   ///< str — pop object, push the named member.
    UNPACK,       ///< u8 n — pop aggregate, push its n elements (element 0 on top).

    // ---- casts / memory ---------------------------------------------------------
    CAST,         ///< type (see encoding above) — pop value, push converted value.
    ALLOC,        ///< u32 n — pop n initialisers (last on top) then capacity, push buffer.
    FREE,         ///< str — release the named buffer (`delete` / `free`), push null.

    CLONE_OBJECT, ///< pop value; deep-copy user objects, pass everything else through.

    MAKE_SIMD,       ///< pop array; push SIMD vector (homogeneous primitives only).
    SIMD_TO_ARRAY,   ///< pop simd; push array (materialize SIMD lanes).

    MAKE_MAP,     ///< u32 n — pop n key/value pairs (value keys, value on top), push map.

    // ---- pipes / channels ---------------------------------------------------------
    PIPE_INSERT,     ///< str pipe name — pop value, write it into the pipe, push null.
    PIPE_EXTRACT,    ///< str pipe name — blocking read, push the extracted value.
    CHANNEL_INSERT,  ///< str channel var — pop value, channel-send (`:=>`), push null.
    CHANNEL_EXTRACT, ///< str channel var — blocking channel-recv (`<=:`), push value.

    // ---- runtime type metadata (emitted for enum / object declarations) -------------
    DEFINE_ENUM,   ///< str name, u32 n, then n × str member names.
    DEFINE_OBJECT, ///< str name, u32 n, then n × str field names.

    // ---- locks -----------------------------------------------------------------------
    LOCK_CREATE,  ///< str — create the named lock (`lock name`).
    LOCK_ACQUIRE, ///< str — enter critical section (`acquire name`).
    LOCK_RELEASE, ///< str — leave critical section (`release name`).

    // ---- monitor / trap ------------------------------------------------------------------
    MONITOR_ENTER, ///< u32 handler — on trap the VM jumps there with the exception pushed.
    MONITOR_EXIT,  ///< leave the protected region (no trap occurred).
};

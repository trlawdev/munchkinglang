#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace munx::native::mir
{

enum class type
{
    i64,
    f64,
    b1,
    value, // runtime MunxValue
    void_,
};

enum class opcode
{
    // constants
    const_i64,
    const_f64,
    const_bool,
    const_null,
    const_str, // operand: string table index

    // locals (slot in operand[0])
    load_local,
    store_local,

    // arithmetic / logic on values (via runtime or i64)
    add,
    sub,
    mul,
    div,
    mod,
    neg,
    not_,
    eq,
    ne,
    lt,
    gt,
    le,
    ge,

    // runtime print
    print,
    println,

    // calls: callee name in string_operand; args are preceding values on stack model — use arg list
    call,
    ret,

    // control
    br,    // target block
    cbr,   // cond, true_block, false_block
    label, // block entry marker (block id)
};

struct instr
{
    opcode op{opcode::const_null};
    type ty{type::value};
    int64_t i64{0};
    double f64{0};
    bool b{false};
    uint32_t local{0};
    uint32_t str_index{0};
    uint32_t block_target{0};
    uint32_t block_target_false{0};
    std::string callee;
    std::vector<uint32_t> args; // value ids (SSA-lite: result index = position in instrs for producers)
    uint32_t result{std::numeric_limits<uint32_t>::max()};
};

struct function
{
    std::string name;
    bool is_entry_init{false};
    uint32_t param_count{0};
    uint32_t local_count{0};
    std::vector<std::string> local_names;
    std::vector<instr> code;
};

struct module
{
    std::string entry_package;
    std::vector<std::string> strings;
    std::vector<function> functions;

    uint32_t intern(const std::string &s)
    {
        for (uint32_t i = 0; i < strings.size(); ++i)
        {
            if (strings[i] == s)
            {
                return i;
            }
        }
        strings.push_back(s);
        return static_cast<uint32_t>(strings.size() - 1);
    }
};

} // namespace munx::native::mir

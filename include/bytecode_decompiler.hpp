#pragma once

#include "bytecode_decoder.hpp"
#include <algorithm>
#include <span>

namespace munx
{

/// Reconstructs source-like Munx from a validated `.mxb` image.16
///
/// The current format does not preserve function/field types, lexical block
/// boundaries, typed-array annotations, or the original import graph.
/// Consequently this emits readable pseudo-Munx: unknown function types are
/// written as `any`, object fields are annotated `any`, and lowered control
/// flow is represented by `goto @bytecode_offset`.
class bytecode_decompiler
{
    std::span<const std::byte> image_;
    mx_program_header header_{};
    std::ostream &out_;

    struct reader
    {
        std::span<const std::byte> code;
        size_t cursor{0};

        bool done() const noexcept { return cursor == code.size(); }
        size_t position() const noexcept { return cursor; }

        uint8_t u8()
        {
            return std::to_integer<uint8_t>(code[cursor++]);
        }

        template <typename T>
        T scalar()
        {
            T value{};
            std::memcpy(&value, code.data() + cursor, sizeof(T));
            cursor += sizeof(T);
            return value;
        }

        uint32_t u32() { return scalar<uint32_t>(); }
        int64_t i64() { return scalar<int64_t>(); }
        double f64() { return scalar<double>(); }

        string_ref string_operand()
        {
            return {u32(), u32()};
        }
    };

public:
    /// Validate and decompile @p image to @p out.
    explicit bytecode_decompiler(std::span<const std::byte> image,
                                 std::ostream &out = std::cout)
        : image_(image), out_(out)
    {
        // Reuse the strict decoder as the validation pass. No decompiler read
        // occurs until all descriptors, strings, instructions, and operands
        // have been bounds checked.
        std::ostringstream discarded;
        bytecode_decoder validator{image, discarded};
        validator.decode();
        std::memcpy(&header_, image_.data(), sizeof(header_));
    }

    void decompile()
    {
        out_ << "// Decompiled from MX bytecode version "
             << header_.mx_bytecode_version << ".\n";
        out_ << "// Types and structured control-flow metadata are not present "
                "in this bytecode format.\n\n";

        for (size_t index = 0; index < header_.num_package_import_list; ++index)
        {
            decompile_package(read_struct<mx_package_descriptor>(
                header_.package_import_array_offset +
                index * sizeof(mx_package_descriptor)), false);
        }
        decompile_package(header_.entry_point_package_descriptor, true);
    }

private:
    template <typename T>
    T read_struct(size_t offset) const
    {
        T value{};
        std::memcpy(&value, image_.data() + offset, sizeof(T));
        return value;
    }

    std::string_view string_at(size_t offset, size_t length) const
    {
        const auto *text = reinterpret_cast<const char *>(
            image_.data() + header_.string_table_offset + offset);
        return {text, length};
    }

    std::string string_at(string_ref ref) const
    {
        return std::string{string_at(ref.offset, ref.length)};
    }

    std::string source_string(std::string_view value) const
    {
        std::string result{"\""};
        for (const char ch : value)
        {
            switch (ch)
            {
            case '\n': result += "\\n"; break;
            case '\t': result += "\\t"; break;
            case '\a': result += "\\a"; break;
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            default: result.push_back(ch); break;
            }
        }
        result.push_back('"');
        return result;
    }

    static std::string source_character(char value)
    {
        switch (value)
        {
        case '\n': return "'\\n'";
        case '\t': return "'\\t'";
        case '\a': return "'\\a'";
        case '\\': return "'\\\\'";
        case '\'': return "'\\''";
        default: return std::string{"'"} + value + '\'';
        }
    }

    static std::string identifier(std::string_view value)
    {
        std::string result;
        result.reserve(value.size() + 1);
        if (value.empty() ||
            !(std::isalpha(static_cast<unsigned char>(value.front())) != 0 ||
              value.front() == '_'))
        {
            result.push_back('_');
        }
        for (const char ch : value)
        {
            const unsigned char byte = static_cast<unsigned char>(ch);
            result.push_back(std::isalnum(byte) != 0 || ch == '_' ? ch : '_');
        }
        return result;
    }

    std::span<const std::byte> code_at(size_t offset, size_t length) const
    {
        return image_.subspan(offset, length);
    }

    void decompile_package(const mx_package_descriptor &package, bool entry)
    {
        const std::string package_name{
            string_at(package.package_name_offset, package.package_name_length)};
        out_ << "// " << (entry ? "entry" : "bundled dependency")
             << " package\n";
        out_ << "package " << identifier(package_name) << "\n\n";

        for (size_t index = 0; index < package.num_function_descriptors; ++index)
        {
            decompile_function(read_struct<mx_function_descriptor>(
                package.function_descriptor_array_offset +
                index * sizeof(mx_function_descriptor)));
            out_ << '\n';
        }

        out_ << "// Package initializer\n";
        decompile_code(code_at(package.package_bytecode_offset,
                               package.package_bytecode_length),
                       0, "");
        out_ << "\n// End package " << identifier(package_name)
             << "\n\n";
    }

    struct function_prefix
    {
        std::vector<std::string> parameters;
        size_t body_offset{0};
    };

    function_prefix infer_parameters(std::span<const std::byte> code) const
    {
        reader input{code};
        function_prefix result;
        while (!input.done())
        {
            const size_t start = input.position();
            const Opcode opcode = static_cast<Opcode>(input.u8());
            if (opcode != Opcode::STORE)
            {
                result.body_offset = start;
                break;
            }
            result.parameters.push_back(string_at(input.string_operand()));
            result.body_offset = input.position();
        }
        // Function entry stores arguments from last to first.
        std::reverse(result.parameters.begin(), result.parameters.end());
        return result;
    }

    void decompile_function(const mx_function_descriptor &function)
    {
        const std::string raw_name{
            string_at(function.function_name_offset,
                      function.function_name_length)};
        const auto code = code_at(function.function_content_offset,
                                  function.function_content_length);
        const function_prefix prefix = infer_parameters(code);

        if (raw_name.starts_with("<lambda#"))
        {
            out_ << "// Originally a lambda; its capture site is not retained.\n";
        }
        out_ << "func " << identifier(raw_name) << '(';
        for (size_t index = 0; index < prefix.parameters.size(); ++index)
        {
            if (index != 0)
            {
                out_ << ", ";
            }
            out_ << identifier(prefix.parameters[index]) << ": any";
        }
        out_ << "): any {\n";
        decompile_code(code, prefix.body_offset, "    ");
        out_ << "}\n";
    }

    static std::string pop(std::vector<std::string> &stack)
    {
        if (stack.empty())
        {
            return "<stack-value>";
        }
        std::string value = std::move(stack.back());
        stack.pop_back();
        return value;
    }

    static std::vector<std::string>
    pop_arguments(std::vector<std::string> &stack, uint32_t count)
    {
        std::vector<std::string> values;
        values.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            values.push_back(pop(stack));
        }
        std::reverse(values.begin(), values.end());
        return values;
    }

    static std::string join(const std::vector<std::string> &values,
                            std::string_view separator)
    {
        std::ostringstream result;
        for (size_t index = 0; index < values.size(); ++index)
        {
            if (index != 0)
            {
                result << separator;
            }
            result << values[index];
        }
        return result.str();
    }

    static const char *binary_symbol(Opcode opcode)
    {
        switch (opcode)
        {
        case Opcode::ADD: return "+";
        case Opcode::SUB: return "-";
        case Opcode::MUL: return "*";
        case Opcode::DIV: return "/";
        case Opcode::MOD: return "%";
        case Opcode::EQ: return "==";
        case Opcode::NE: return "!=";
        case Opcode::LT: return "<";
        case Opcode::GT: return ">";
        case Opcode::LE: return "<=";
        case Opcode::GE: return ">=";
        case Opcode::BITWISE_AND: return "&";
        case Opcode::BITWISE_OR: return "|";
        case Opcode::BITWISE_XOR: return "^";
        default: return nullptr;
        }
    }

    static const char *primitive_name(uint8_t encoded)
    {
        switch (static_cast<ast::primitive_kind>(encoded))
        {
        case ast::primitive_kind::Int: return "int";
        case ast::primitive_kind::Float: return "float";
        case ast::primitive_kind::Bool: return "bool";
        case ast::primitive_kind::String: return "string";
        case ast::primitive_kind::Character: return "character";
        case ast::primitive_kind::Void: return "void";
        case ast::primitive_kind::Socket: return "socket";
        case ast::primitive_kind::File: return "file";
        case ast::primitive_kind::Term: return "term";
        case ast::primitive_kind::Exception: return "exception";
        }
        return "unknown";
    }

    std::string decompile_type(reader &input)
    {
        const auto type = static_cast<ast::type_kind>(input.u8());
        switch (type)
        {
        case ast::type_kind::Primitive:
            return primitive_name(input.u8());
        case ast::type_kind::Named:
            return identifier(string_at(input.string_operand()));
        case ast::type_kind::Array:
            return '[' + decompile_type(input) + ']';
        case ast::type_kind::Tuple:
        {
            const uint32_t count = input.u32();
            std::vector<std::string> elements;
            elements.reserve(count);
            for (uint32_t index = 0; index < count; ++index)
            {
                elements.push_back(decompile_type(input));
            }
            return "tuple[" + join(elements, ", ") + ']';
        }
        case ast::type_kind::Map:
            return "map[" + decompile_type(input) + ", " + decompile_type(input) +
                   ']';
        case ast::type_kind::Lambda:
        {
            const uint32_t count = input.u32();
            std::vector<std::string> params;
            params.reserve(count);
            for (uint32_t index = 0; index < count; ++index)
            {
                params.push_back(decompile_type(input));
            }
            return "lambda(" + join(params, ", ") + ") -> " + decompile_type(input);
        }
        }
        return "unknown";
    }

    void statement(std::string_view indent, size_t offset,
                   const std::string &text)
    {
        out_ << indent << text << "  // @" << offset << '\n';
    }

    void decompile_code(std::span<const std::byte> code, size_t start,
                        std::string_view indent)
    {
        reader input{code, start};
        std::vector<std::string> stack;
        while (!input.done())
        {
            const size_t offset = input.position();
            const Opcode opcode = static_cast<Opcode>(input.u8());
            if (const char *symbol = binary_symbol(opcode))
            {
                const std::string right = pop(stack);
                const std::string left = pop(stack);
                stack.push_back('(' + left + ' ' + symbol + ' ' + right + ')');
                continue;
            }

            switch (opcode)
            {
            case Opcode::PUSH_INT:
                stack.push_back(std::to_string(input.i64()));
                break;
            case Opcode::PUSH_FLOAT:
            {
                std::ostringstream value;
                value << input.f64();
                stack.push_back(value.str());
                break;
            }
            case Opcode::PUSH_STRING:
                stack.push_back(source_string(string_at(input.string_operand())));
                break;
            case Opcode::PUSH_CHAR:
                stack.push_back(source_character(
                    static_cast<char>(input.u8())));
                break;
            case Opcode::PUSH_BOOL:
                stack.push_back(input.u8() == 0 ? "false" : "true");
                break;
            case Opcode::PUSH_NULL:
                stack.push_back("null");
                break;
            case Opcode::PUSH_REGEX:
                stack.push_back("r" +
                    source_string(string_at(input.string_operand())));
                break;
            case Opcode::PUSH_ENUM:
            {
                const std::string enum_name =
                    identifier(string_at(input.string_operand()));
                const std::string member =
                    identifier(string_at(input.string_operand()));
                stack.push_back(enum_name + "::" + member);
                break;
            }
            case Opcode::PUSH_FUNC:
                stack.push_back(identifier(string_at(input.string_operand())));
                break;
            case Opcode::POP:
                statement(indent, offset, pop(stack));
                break;
            case Opcode::DUP:
                stack.push_back(stack.empty() ? "<stack-value>" : stack.back());
                break;
            case Opcode::SWAP:
                if (stack.size() >= 2)
                {
                    std::iter_swap(stack.end() - 1, stack.end() - 2);
                }
                break;
            case Opcode::LOAD:
                stack.push_back(identifier(string_at(input.string_operand())));
                break;
            case Opcode::STORE:
            {
                const std::string name =
                    identifier(string_at(input.string_operand()));
                statement(indent, offset, name + " = " + pop(stack));
                break;
            }
            case Opcode::NEG:
                stack.push_back("(-" + pop(stack) + ')');
                break;
            case Opcode::NOT:
                stack.push_back("(!" + pop(stack) + ')');
                break;
            case Opcode::BITWISE_NOT:
                stack.push_back("(~" + pop(stack) + ')');
                break;
            case Opcode::JMP:
                statement(indent, offset,
                          "goto @" + std::to_string(input.u32()));
                break;
            case Opcode::JMP_IF_FALSE:
                statement(indent, offset,
                          "if !" + pop(stack) + " goto @" +
                          std::to_string(input.u32()));
                break;
            case Opcode::JMP_IF_TRUE:
                statement(indent, offset,
                          "if " + pop(stack) + " goto @" +
                          std::to_string(input.u32()));
                break;
            case Opcode::CALL:
            {
                const uint8_t count = input.u8();
                const auto arguments = pop_arguments(stack, count);
                const std::string callee = pop(stack);
                stack.push_back(callee + '(' + join(arguments, ", ") + ')');
                break;
            }
            case Opcode::RET:
                statement(indent, offset, "return " + pop(stack));
                break;
            case Opcode::HALT:
                statement(indent, offset, "// end initializer");
                break;
            case Opcode::MAKE_ARRAY:
            {
                const auto values = pop_arguments(stack, input.u32());
                stack.push_back('[' + join(values, ", ") + ']');
                break;
            }
            case Opcode::MAKE_TUPLE:
            {
                const auto values = pop_arguments(stack, input.u32());
                stack.push_back('{' + join(values, ", ") + '}');
                break;
            }
            case Opcode::INDEX_GET:
            {
                const std::string index = pop(stack);
                const std::string object = pop(stack);
                stack.push_back(object + '[' + index + ']');
                break;
            }
            case Opcode::MEMBER_GET:
            {
                const std::string object = pop(stack);
                stack.push_back(object + '.' +
                    identifier(string_at(input.string_operand())));
                break;
            }
            case Opcode::UNPACK:
            {
                const uint8_t count = input.u8();
                const std::string aggregate = pop(stack);
                for (uint8_t index = count; index > 0; --index)
                {
                    stack.push_back(aggregate + '[' +
                                    std::to_string(index - 1) + ']');
                }
                break;
            }
            case Opcode::CAST:
            {
                const std::string value = pop(stack);
                stack.push_back("cast[" + decompile_type(input) + "](" +
                                value + ')');
                break;
            }
            case Opcode::ALLOC:
            {
                const auto values = pop_arguments(stack, input.u32());
                const std::string capacity = pop(stack);
                stack.push_back("alloc [" + capacity + "] [" +
                                join(values, ", ") + ']');
                break;
            }
            case Opcode::FREE:
                stack.push_back("free " +
                    identifier(string_at(input.string_operand())));
                break;
            case Opcode::CLONE_OBJECT:
                break;
            case Opcode::MAKE_SIMD:
            {
                const std::string array = pop(stack);
                stack.push_back("simd(" + array + ')');
                break;
            }
            case Opcode::SIMD_TO_ARRAY:
            {
                const std::string simd = pop(stack);
                stack.push_back("simd_to_array(" + simd + ')');
                break;
            }
            case Opcode::PIPE_INSERT:
            {
                const std::string value = pop(stack);
                stack.push_back(value + " -> " +
                    identifier(string_at(input.string_operand())));
                break;
            }
            case Opcode::PIPE_EXTRACT:
                stack.push_back("<- " +
                    identifier(string_at(input.string_operand())));
                break;
            case Opcode::CHANNEL_INSERT:
            {
                const std::string value = pop(stack);
                stack.push_back(value + " :=> " +
                    identifier(string_at(input.string_operand())));
                break;
            }
            case Opcode::CHANNEL_EXTRACT:
                stack.push_back("<=: " +
                    identifier(string_at(input.string_operand())));
                break;
            case Opcode::DEFINE_ENUM:
            {
                const std::string name =
                    identifier(string_at(input.string_operand()));
                const uint32_t count = input.u32();
                std::vector<std::string> members;
                for (uint32_t index = 0; index < count; ++index)
                {
                    members.push_back(
                        identifier(string_at(input.string_operand())));
                }
                statement(indent, offset,
                          "enum " + name + " { " +
                          join(members, ", ") + " }");
                break;
            }
            case Opcode::DEFINE_OBJECT:
            {
                const std::string name =
                    identifier(string_at(input.string_operand()));
                const uint32_t count = input.u32();
                std::vector<std::string> fields;
                for (uint32_t index = 0; index < count; ++index)
                {
                    fields.push_back(
                        identifier(string_at(input.string_operand())) +
                        ": any");
                }
                statement(indent, offset,
                          "object " + name + " { " +
                          join(fields, ", ") + " }");
                break;
            }
            case Opcode::LOCK_CREATE:
                statement(indent, offset, "lock " +
                    identifier(string_at(input.string_operand())));
                break;
            case Opcode::LOCK_ACQUIRE:
                statement(indent, offset, "acquire " +
                    identifier(string_at(input.string_operand())));
                break;
            case Opcode::LOCK_RELEASE:
                statement(indent, offset, "release " +
                    identifier(string_at(input.string_operand())));
                break;
            case Opcode::MONITOR_ENTER:
                statement(indent, offset,
                          "monitor_begin trap @" +
                          std::to_string(input.u32()));
                break;
            case Opcode::MONITOR_EXIT:
                statement(indent, offset, "monitor_end");
                break;
            default:
                // Binary operations are handled before the switch. Validation
                // guarantees no unknown opcode can reach this point.
                break;
            }
        }

        for (const std::string &value : stack)
        {
            statement(indent, code.size(),
                      "// residual stack value: " + value);
        }
    }
};

/// Read, validate, and decompile one `.mxb` file.
inline void decompile_bytecode_file(const std::filesystem::path &path,
                                    std::ostream &out = std::cout)
{
    std::ifstream input{path, std::ios::binary};
    if (!input)
    {
        fail_compile("could not open bytecode file: " +
                                path.string());
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0)
    {
        fail_compile("could not determine bytecode file size: " +
                                path.string());
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> image(static_cast<size_t>(end));
    input.read(reinterpret_cast<char *>(image.data()),
               static_cast<std::streamsize>(image.size()));
    if (!input && !image.empty())
    {
        fail_compile("failed to read bytecode file: " +
                                path.string());
    }
    bytecode_decompiler decompiler{image, out};
    decompiler.decompile();
}

} // namespace munx

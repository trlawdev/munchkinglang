#pragma once

#include "bytecode_compiler.hpp"
#include <cctype>
#include <iomanip>
#include <limits>
#include <span>

namespace munx
{

/// Bounds-checked reader and human-readable disassembler for `.mxb` images.
class bytecode_decoder
{
    std::span<const std::byte> image_;
    mx_program_header header_{};
    std::ostream &out_;

public:
    /// Decode @p image to @p out.
    /// @throws compilation_error if the image is malformed or unsupported.
    explicit bytecode_decoder(std::span<const std::byte> image,
                              std::ostream &out = std::cout)
        : image_(image), out_(out)
    {
        header_ = read_struct<mx_program_header>(0, "program header");
        if (header_.mx_signature[0] != std::byte{'M'} ||
            header_.mx_signature[1] != std::byte{'X'})
        {
            fail("invalid bytecode signature (expected MX)");
        }
        if (header_.mx_bytecode_version != current_mx_bytecode_version)
        {
            fail("unsupported bytecode version " +
                 std::to_string(header_.mx_bytecode_version) +
                 " (decoder supports " +
                 std::to_string(current_mx_bytecode_version) + ")");
        }
        require_range(header_.string_table_offset, header_.string_table_length,
                      "string table");
    }

    /// Validate and print the complete bytecode image.
    void decode()
    {
        out_ << "MX bytecode version " << header_.mx_bytecode_version << '\n';
        out_ << "package " << quoted_string(header_.package_name_offset,
                                             header_.package_name_length)
             << '\n';
        out_ << "imports " << header_.num_package_import_list << '\n';
        out_ << "string table @" << header_.string_table_offset
             << " (" << header_.string_table_length << " bytes)\n";

        const size_t import_bytes =
            checked_product(header_.num_package_import_list,
                            sizeof(mx_package_descriptor),
                            "package descriptor array");
        require_range(header_.package_import_array_offset, import_bytes,
                      "package descriptor array");

        for (size_t index = 0; index < header_.num_package_import_list; ++index)
        {
            const size_t offset =
                header_.package_import_array_offset +
                index * sizeof(mx_package_descriptor);
            decode_package(read_struct<mx_package_descriptor>(
                               offset, "package descriptor"),
                           "import", index);
        }

        validate_entry_point();
        decode_package(header_.entry_point_package_descriptor, "entry", 0);
    }

private:
    static void fail(const std::string &message)
    {
        fail_compile("invalid .mxb: " + message);
    }

    void require_range(size_t offset, size_t length,
                       std::string_view section) const
    {
        if (offset > image_.size() || length > image_.size() - offset)
        {
            fail(std::string{section} + " is outside the file");
        }
    }

    static size_t checked_product(size_t count, size_t item_size,
                                  std::string_view section)
    {
        if (item_size != 0 &&
            count > std::numeric_limits<size_t>::max() / item_size)
        {
            fail(std::string{section} + " size overflows");
        }
        return count * item_size;
    }

    template <typename T>
    T read_struct(size_t offset, std::string_view section) const
    {
        require_range(offset, sizeof(T), section);
        T value{};
        std::memcpy(&value, image_.data() + offset, sizeof(T));
        return value;
    }

    std::string_view string_at(uint32_t offset, uint32_t length) const
    {
        if (offset > header_.string_table_length ||
            length > header_.string_table_length - offset)
        {
            fail("string reference is outside the string table");
        }
        const auto *data = reinterpret_cast<const char *>(
            image_.data() + header_.string_table_offset + offset);
        return {data, length};
    }

    std::string quoted_string(size_t offset, size_t length) const
    {
        if (offset > std::numeric_limits<uint32_t>::max() ||
            length > std::numeric_limits<uint32_t>::max())
        {
            fail("string reference exceeds the bytecode operand width");
        }
        std::ostringstream text;
        text << std::quoted(std::string{
            string_at(static_cast<uint32_t>(offset),
                      static_cast<uint32_t>(length))});
        return text.str();
    }

    void validate_entry_point() const
    {
        const auto &entry = header_.entry_point_package_descriptor;
        if (header_.entry_point_bytecode_offset !=
                entry.package_bytecode_offset ||
            header_.entry_point_bytecode_length !=
                entry.package_bytecode_length)
        {
            fail("entry-point offsets disagree with its package descriptor");
        }
        if (header_.package_name_offset != entry.package_name_offset ||
            header_.package_name_length != entry.package_name_length)
        {
            fail("entry-point package name disagrees with the header");
        }
    }

    void decode_package(const mx_package_descriptor &package,
                        std::string_view kind, size_t index)
    {
        const std::string name =
            quoted_string(package.package_name_offset,
                          package.package_name_length);
        out_ << "\n[" << kind;
        if (kind == "import")
        {
            out_ << ' ' << index;
        }
        out_ << "] package " << name << '\n';

        const size_t descriptor_bytes =
            checked_product(package.num_function_descriptors,
                            sizeof(mx_function_descriptor),
                            "function descriptor array");
        require_range(package.function_descriptor_array_offset,
                      descriptor_bytes, "function descriptor array");

        out_ << "  functions " << package.num_function_descriptors << '\n';
        for (size_t function_index = 0;
             function_index < package.num_function_descriptors;
             ++function_index)
        {
            const size_t descriptor_offset =
                package.function_descriptor_array_offset +
                function_index * sizeof(mx_function_descriptor);
            const auto function = read_struct<mx_function_descriptor>(
                descriptor_offset, "function descriptor");
            require_range(function.function_content_offset,
                          function.function_content_length,
                          "function bytecode");
            if (function.debug_map_length > 0)
            {
                require_range(function.debug_map_offset, function.debug_map_length,
                              "function debug map");
            }
            out_ << "\n  function "
                 << quoted_string(function.function_name_offset,
                                  function.function_name_length)
                 << " @" << function.function_content_offset
                 << " (" << function.function_content_length << " bytes)\n";
            decode_code(function.function_content_offset,
                        function.function_content_length, "    ");
        }

        require_range(package.package_bytecode_offset,
                      package.package_bytecode_length,
                      "package initializer bytecode");
        if (package.init_debug_map_length > 0)
        {
            require_range(package.init_debug_map_offset, package.init_debug_map_length,
                          "package initializer debug map");
        }
        out_ << "\n  initializer @" << package.package_bytecode_offset
             << " (" << package.package_bytecode_length << " bytes)\n";
        decode_code(package.package_bytecode_offset,
                    package.package_bytecode_length, "    ");
    }

    class instruction_reader
    {
        size_t file_offset_;
        std::span<const std::byte> code_;
        size_t cursor_{0};

    public:
        instruction_reader(const bytecode_decoder &owner, size_t file_offset,
                           size_t length)
            : file_offset_(file_offset),
              code_(owner.image_.subspan(file_offset, length))
        {}

        bool done() const noexcept { return cursor_ == code_.size(); }
        size_t position() const noexcept { return cursor_; }

        uint8_t u8(std::string_view operand)
        {
            require(1, operand);
            return std::to_integer<uint8_t>(code_[cursor_++]);
        }

        uint32_t u32(std::string_view operand)
        {
            return scalar<uint32_t>(operand);
        }

        int64_t i64(std::string_view operand)
        {
            return scalar<int64_t>(operand);
        }

        double f64(std::string_view operand)
        {
            return scalar<double>(operand);
        }

        string_ref string_ref_operand(std::string_view operand)
        {
            return {u32(operand), u32(operand)};
        }

    private:
        void require(size_t length, std::string_view operand) const
        {
            if (cursor_ > code_.size() ||
                length > code_.size() - cursor_)
            {
                fail("truncated " + std::string{operand} +
                     " at file offset " +
                     std::to_string(file_offset_ + cursor_));
            }
        }

        template <typename T>
        T scalar(std::string_view operand)
        {
            require(sizeof(T), operand);
            T value{};
            std::memcpy(&value, code_.data() + cursor_, sizeof(T));
            cursor_ += sizeof(T);
            return value;
        }
    };

    static const char *opcode_name(Opcode opcode)
    {
        switch (opcode)
        {
#define MUNX_OPCODE_NAME(name) case Opcode::name: return #name
        MUNX_OPCODE_NAME(PUSH_INT);
        MUNX_OPCODE_NAME(PUSH_FLOAT);
        MUNX_OPCODE_NAME(PUSH_STRING);
        MUNX_OPCODE_NAME(PUSH_CHAR);
        MUNX_OPCODE_NAME(PUSH_BOOL);
        MUNX_OPCODE_NAME(PUSH_NULL);
        MUNX_OPCODE_NAME(PUSH_REGEX);
        MUNX_OPCODE_NAME(PUSH_ENUM);
        MUNX_OPCODE_NAME(PUSH_FUNC);
        MUNX_OPCODE_NAME(POP);
        MUNX_OPCODE_NAME(DUP);
        MUNX_OPCODE_NAME(SWAP);
        MUNX_OPCODE_NAME(LOAD);
        MUNX_OPCODE_NAME(STORE);
        MUNX_OPCODE_NAME(ADD);
        MUNX_OPCODE_NAME(SUB);
        MUNX_OPCODE_NAME(MUL);
        MUNX_OPCODE_NAME(DIV);
        MUNX_OPCODE_NAME(MOD);
        MUNX_OPCODE_NAME(NEG);
        MUNX_OPCODE_NAME(EQ);
        MUNX_OPCODE_NAME(NE);
        MUNX_OPCODE_NAME(LT);
        MUNX_OPCODE_NAME(GT);
        MUNX_OPCODE_NAME(LE);
        MUNX_OPCODE_NAME(GE);
        MUNX_OPCODE_NAME(NOT);
        MUNX_OPCODE_NAME(BITWISE_AND);
        MUNX_OPCODE_NAME(BITWISE_OR);
        MUNX_OPCODE_NAME(BITWISE_XOR);
        MUNX_OPCODE_NAME(BITWISE_NOT);
        MUNX_OPCODE_NAME(JMP);
        MUNX_OPCODE_NAME(JMP_IF_FALSE);
        MUNX_OPCODE_NAME(JMP_IF_TRUE);
        MUNX_OPCODE_NAME(CALL);
        MUNX_OPCODE_NAME(RET);
        MUNX_OPCODE_NAME(HALT);
        MUNX_OPCODE_NAME(MAKE_ARRAY);
        MUNX_OPCODE_NAME(MAKE_TUPLE);
        MUNX_OPCODE_NAME(INDEX_GET);
        MUNX_OPCODE_NAME(MEMBER_GET);
        MUNX_OPCODE_NAME(UNPACK);
        MUNX_OPCODE_NAME(CAST);
        MUNX_OPCODE_NAME(ALLOC);
        MUNX_OPCODE_NAME(CLONE_OBJECT);
        MUNX_OPCODE_NAME(MAKE_SIMD);
        MUNX_OPCODE_NAME(SIMD_TO_ARRAY);
        MUNX_OPCODE_NAME(MAKE_MAP);
        MUNX_OPCODE_NAME(FREE);
        MUNX_OPCODE_NAME(PIPE_INSERT);
        MUNX_OPCODE_NAME(PIPE_EXTRACT);
        MUNX_OPCODE_NAME(CHANNEL_INSERT);
        MUNX_OPCODE_NAME(CHANNEL_EXTRACT);
        MUNX_OPCODE_NAME(DEFINE_ENUM);
        MUNX_OPCODE_NAME(DEFINE_OBJECT);
        MUNX_OPCODE_NAME(LOCK_CREATE);
        MUNX_OPCODE_NAME(LOCK_ACQUIRE);
        MUNX_OPCODE_NAME(LOCK_RELEASE);
        MUNX_OPCODE_NAME(MONITOR_ENTER);
        MUNX_OPCODE_NAME(MONITOR_EXIT);
#undef MUNX_OPCODE_NAME
        }
        return nullptr;
    }

    void decode_code(size_t file_offset, size_t length,
                     std::string_view indent)
    {
        instruction_reader reader{*this, file_offset, length};
        while (!reader.done())
        {
            const size_t instruction_offset = reader.position();
            const uint8_t encoded = reader.u8("opcode");
            const Opcode opcode = static_cast<Opcode>(encoded);
            const char *name = opcode_name(opcode);
            if (name == nullptr)
            {
                fail("unknown opcode " + std::to_string(encoded) +
                     " at file offset " +
                     std::to_string(file_offset + instruction_offset));
            }

            out_ << indent << std::setw(6) << instruction_offset
                 << "  " << std::left << std::setw(18) << name
                 << std::right;
            decode_operands(reader, opcode);
            out_ << '\n';
        }
    }

    void print_string_operand(instruction_reader &reader,
                              std::string_view label = {})
    {
        const string_ref ref = reader.string_ref_operand("string operand");
        if (!label.empty())
        {
            out_ << label << '=';
        }
        out_ << quoted_string(ref.offset, ref.length);
    }

    void decode_operands(instruction_reader &reader, Opcode opcode)
    {
        switch (opcode)
        {
        case Opcode::PUSH_INT:
            out_ << reader.i64("integer operand");
            break;
        case Opcode::PUSH_FLOAT:
            out_ << reader.f64("float operand");
            break;
        case Opcode::PUSH_CHAR:
        {
            const uint8_t value = reader.u8("character operand");
            if (std::isprint(value) != 0)
            {
                out_ << '\'' << static_cast<char>(value) << "' ";
            }
            out_ << static_cast<unsigned>(value);
            break;
        }
        case Opcode::PUSH_BOOL:
            out_ << (reader.u8("boolean operand") == 0 ? "false" : "true");
            break;
        case Opcode::PUSH_STRING:
        case Opcode::PUSH_REGEX:
        case Opcode::PUSH_FUNC:
        case Opcode::LOAD:
        case Opcode::STORE:
        case Opcode::MEMBER_GET:
        case Opcode::FREE:
        case Opcode::PIPE_INSERT:
        case Opcode::PIPE_EXTRACT:
        case Opcode::CHANNEL_INSERT:
        case Opcode::CHANNEL_EXTRACT:
        case Opcode::LOCK_CREATE:
        case Opcode::LOCK_ACQUIRE:
        case Opcode::LOCK_RELEASE:
            print_string_operand(reader);
            break;
        case Opcode::PUSH_ENUM:
            print_string_operand(reader);
            out_ << "::";
            print_string_operand(reader);
            break;
        case Opcode::JMP:
        case Opcode::JMP_IF_FALSE:
        case Opcode::JMP_IF_TRUE:
        case Opcode::MONITOR_ENTER:
            out_ << reader.u32("jump target");
            break;
        case Opcode::CALL:
            out_ << "argc=" << static_cast<unsigned>(
                reader.u8("argument count"));
            break;
        case Opcode::UNPACK:
            out_ << "count=" << static_cast<unsigned>(
                reader.u8("unpack count"));
            break;
        case Opcode::MAKE_ARRAY:
        case Opcode::MAKE_TUPLE:
        case Opcode::MAKE_MAP:
        case Opcode::ALLOC:
            out_ << "count=" << reader.u32("element count");
            break;
        case Opcode::CAST:
            decode_type(reader);
            break;
        case Opcode::DEFINE_ENUM:
        case Opcode::DEFINE_OBJECT:
        {
            print_string_operand(reader, "name");
            const uint32_t count = reader.u32("member count");
            out_ << " members=[";
            for (uint32_t index = 0; index < count; ++index)
            {
                if (index != 0)
                {
                    out_ << ", ";
                }
                print_string_operand(reader);
            }
            out_ << ']';
            break;
        }
        default:
            break;
        }
    }

    void decode_type(instruction_reader &reader)
    {
        const uint8_t encoded = reader.u8("type tag");
        const auto type = static_cast<ast::type_kind>(encoded);
        switch (type)
        {
        case ast::type_kind::Primitive:
            out_ << "primitive(" << static_cast<unsigned>(
                reader.u8("primitive type")) << ')';
            break;
        case ast::type_kind::Named:
            out_ << "named(";
            print_string_operand(reader);
            out_ << ')';
            break;
        case ast::type_kind::Array:
            out_ << "array(";
            decode_type(reader);
            out_ << ')';
            break;
        case ast::type_kind::Tuple:
        {
            const uint32_t count = reader.u32("tuple type count");
            out_ << "tuple(";
            for (uint32_t index = 0; index < count; ++index)
            {
                if (index != 0)
                {
                    out_ << ", ";
                }
                decode_type(reader);
            }
            out_ << ')';
            break;
        }
        case ast::type_kind::Map:
            out_ << "map(";
            decode_type(reader);
            out_ << " => ";
            decode_type(reader);
            out_ << ')';
            break;
        case ast::type_kind::Lambda:
        {
            const uint32_t count = reader.u32("lambda param count");
            out_ << "Lambda({";
            for (uint32_t index = 0; index < count; ++index)
            {
                if (index != 0)
                {
                    out_ << ", ";
                }
                decode_type(reader);
            }
            out_ << "} => ";
            decode_type(reader);
            out_ << ')';
            break;
        }
        }
    }
};

/// Read and decode one `.mxb` file.
inline void decode_bytecode_file(const std::filesystem::path &path,
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
    bytecode_decoder decoder{image, out};
    decoder.decode();
}

} // namespace munx

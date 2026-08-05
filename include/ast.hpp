#pragma once

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace munx::ast
{

    // -------------------------------------------------------------------------
    // Source location
    // -------------------------------------------------------------------------

    /// File path and 1-based line/column of a construct's first token.
    struct source_loc
    {
        std::string file{};   ///< Source file path (as given to the parser).
        long long line{0};    ///< 1-based line number; 0 means unset.
        long long column{0};  ///< 1-based column number; 0 means unset.
    };

    /// Stream @p loc as `file:line:column` (compiler diagnostic style).
    inline std::ostream &operator<<(std::ostream &os, const source_loc &loc)
    {
        return os << loc.file << ':' << loc.line << ':' << loc.column;
    }

    // -------------------------------------------------------------------------
    // Forward declarations
    // -------------------------------------------------------------------------

    struct type_node;
    struct expr_node;
    struct stmt_node;
    struct block_stmt;
    struct parameter;

    // -------------------------------------------------------------------------
    // Types
    // -------------------------------------------------------------------------

    /// Built-in scalar and I/O primitive kinds.
    enum class primitive_kind
    {
        Int,       ///< Signed 64-bit integer.
        Float,     ///< Floating-point number.
        Bool,      ///< Boolean.
        String,    ///< Text string.
        Character, ///< Single character.
        Void,      ///< No value (return type only).
        Socket,    ///< Network socket from `open(io_type::socket, …)`.
        File,      ///< Filesystem handle from `open(io_type::file, …)`.
        Term,      ///< Terminal/TTY handle from `open(io_type::tty, …)`.
        Exception, ///< Error object for `monitor` / `trap`.
    };

    /// User-defined type referenced by name (enum or object).
    struct named_type
    {
        std::string name; ///< Type identifier.
    };

    /// Array type `[T]`.
    struct array_type
    {
        std::unique_ptr<type_node> element; ///< Element type `T`.
    };

    /// Tuple type `tuple[T1, T2, …]` (may be empty).
    struct tuple_type
    {
        std::vector<std::unique_ptr<type_node>> elements; ///< Component types.
    };

    /// Map type `map[K => V]`.
    struct map_type
    {
        std::unique_ptr<type_node> key;   ///< Key type.
        std::unique_ptr<type_node> value; ///< Value type.
    };

    /// Callable lambda type `Lambda[{T1, T2, …} => R]`.
    struct lambda_type
    {
        std::vector<std::unique_ptr<type_node>> params; ///< Parameter types.
        std::unique_ptr<type_node> ret;                 ///< Return type.
    };

    /// Wrapper holding a @ref primitive_kind.
    struct primitive_type
    {
        primitive_kind kind; ///< Which primitive.
    };

    /// Discriminator for @ref type_node::value.
    enum class type_kind
    {
        Primitive, ///< @ref primitive_type
        Named,     ///< @ref named_type
        Array,     ///< @ref array_type
        Tuple,     ///< @ref tuple_type
        Map,       ///< @ref map_type
        Lambda,    ///< @ref lambda_type
    };

    /// Type annotation AST node (parameters, returns, fields, casts, traps).
    struct type_node
    {
        source_loc loc{}; ///< Location of the type's first token.
        type_kind type{type_kind::Primitive}; ///< Active alternative in @ref value.
        std::variant<primitive_type, named_type, array_type, tuple_type, map_type,
                     lambda_type>
            value;

        /// Build a primitive type node at optional @p loc.
        static type_node make_primitive(primitive_kind p, source_loc loc = {})
        {
            type_node node{loc, type_kind::Primitive, primitive_type{p}};
            return node;
        }

        /// Build a named type node for @p name at optional @p loc.
        static type_node make_named(std::string name, source_loc loc = {})
        {
            type_node node{loc, type_kind::Named, named_type{std::move(name)}};
            return node;
        }
    };

    /// Function or lambda parameter `name: type`.
    struct parameter
    {
        source_loc loc{};                 ///< Location of the parameter name.
        std::string name;                 ///< Parameter identifier.
        std::unique_ptr<type_node> type;  ///< Declared type.
    };

    // -------------------------------------------------------------------------
    // Expressions
    // -------------------------------------------------------------------------

    /// Binary operators (including reserved bitwise ops not yet parsed).
    enum class binary_op
    {
        Add,
        Sub,
        Mul,
        Div,
        Mod,
        Eq,
        Ne,
        Lt,
        Gt,
        Le,
        Ge,
        And,
        Or,
        BitwiseAnd,
        BitwiseOr,
        BitwiseXor,
    };

    /// Unary operators.
    enum class unary_op
    {
        Not,        ///< `!`
        BitwiseNot, ///< `~`
        Neg,        ///< unary `-`
    };

    /// Assignment operators at statement level.
    enum class assign_op
    {
        Assign,    ///< `=`
        AddAssign, ///< `+=`
    };

    /// Integer literal expression.
    struct int_literal
    {
        long long value; ///< Parsed integer value.
    };

    /// Floating-point literal expression.
    struct float_literal
    {
        long double value; ///< Parsed float value.
    };

    /// String literal expression (escapes already decoded).
    struct string_literal
    {
        std::string value; ///< Decoded string contents.
    };

    /// Character literal expression.
    struct char_literal
    {
        char value; ///< Decoded character.
    };

    /// Boolean literal `true` / `false`.
    struct bool_literal
    {
        bool value; ///< Boolean payload.
    };

    /// Null literal `null`.
    struct null_literal
    {
    };

    /// Regex literal `r"..."`.
    struct regex_literal
    {
        std::string pattern; ///< Raw pattern text (no escape processing).
    };

    /// Bare identifier expression.
    struct identifier
    {
        std::string name; ///< Identifier spelling.
    };

    /// Binary expression `left op right`.
    struct binary_expr
    {
        binary_op op;                     ///< Operator.
        std::unique_ptr<expr_node> left;  ///< Left operand.
        std::unique_ptr<expr_node> right; ///< Right operand.
    };

    /// Unary expression `op operand`.
    struct unary_expr
    {
        unary_op op;                         ///< Operator.
        std::unique_ptr<expr_node> operand;  ///< Operand.
    };

    /// Call expression `callee(arguments…)`.
    struct call_expr
    {
        std::unique_ptr<expr_node> callee;                 ///< Callee expression.
        std::vector<std::unique_ptr<expr_node>> arguments; ///< Argument list.
    };

    /// Member access `object.member`.
    struct member_expr
    {
        std::unique_ptr<expr_node> object; ///< Base expression.
        std::string member;                ///< Member name.
    };

    /// Enum member access `Enum::Member`.
    struct enum_access_expr
    {
        std::string enum_name; ///< Enum type name.
        std::string member;    ///< Enumerant name.
    };

    /// Index expression `object[index]`.
    struct index_expr
    {
        std::unique_ptr<expr_node> object; ///< Indexed expression.
        std::unique_ptr<expr_node> index;  ///< Index expression.
    };

    /// Untyped array literal `[e, …]`.
    struct array_literal
    {
        std::vector<std::unique_ptr<expr_node>> elements; ///< Elements.
    };

    /// Typed array literal `[T][e, …]`.
    struct typed_array_literal
    {
        std::unique_ptr<type_node> element_type;           ///< Element type `T`.
        std::vector<std::unique_ptr<expr_node>> elements;  ///< Elements.
    };

    /// Tuple literal `{e, …}`.
    struct tuple_literal
    {
        std::vector<std::unique_ptr<expr_node>> elements; ///< Elements.
    };

    /// Pipe insert `value -> pipe_name`.
    struct pipe_insert_expr
    {
        std::unique_ptr<expr_node> value; ///< Value written into the pipe.
        std::string pipe_name;            ///< Destination pipe identifier.
    };

    /// Pipe extract `<- pipe_name` (blocking read).
    struct pipe_extract_expr
    {
        std::string pipe_name; ///< Source pipe identifier.
    };

    /// Cast expression `cast[T](operand)`.
    struct cast_expr
    {
        std::unique_ptr<type_node> target_type; ///< Target type `T`.
        std::unique_ptr<expr_node> operand;     ///< Value being cast.
    };

    /// Allocation `alloc [capacity] [initial…]`.
    struct alloc_expr
    {
        std::unique_ptr<expr_node> capacity;                    ///< Buffer capacity.
        std::vector<std::unique_ptr<expr_node>> initial_values; ///< Initializers.
    };

    /// Free / delete expression `delete name` or `free name`.
    struct free_expr
    {
        std::string buffer_name; ///< Name of the buffer to release.
    };

    /// SIMD expression `simd(operand)` — wraps a homogeneous primitive array.
    struct simd_expr
    {
        std::unique_ptr<expr_node> operand; ///< Array (or array-producing) value.
    };

    /// Lambda expression `lambda (params): Ret => { … }`.
    struct lambda_expr
    {
        std::vector<parameter> parameters;       ///< Parameter list.
        std::unique_ptr<type_node> return_type;  ///< Declared return type.
        std::unique_ptr<block_stmt> body;        ///< Body block.
    };

    /// One entry in a map literal.
    struct map_entry
    {
        std::unique_ptr<expr_node> key;   ///< Key expression (usually a string literal).
        std::unique_ptr<expr_node> value; ///< Value expression.
    };

    /// Map literal `map[K => V]{ key: value, … }`.
    struct map_literal
    {
        std::unique_ptr<type_node> key_type;    ///< Declared key type.
        std::unique_ptr<type_node> value_type;  ///< Declared value type.
        std::vector<map_entry> entries;         ///< Initial entries.
    };

    /// Braced map entries `{ key: value, … }` (no type prefix).
    struct map_entries_literal
    {
        std::vector<map_entry> entries;
    };

    /// Discriminator for @ref expr_node::value (must match variant order).
    enum class expr_type
    {
        IntLiteral,
        FloatLiteral,
        StringLiteral,
        CharLiteral,
        BoolLiteral,
        NullLiteral,
        RegexLiteral,
        Identifier,
        Binary,
        Unary,
        Call,
        Member,
        EnumAccess,
        Index,
        ArrayLiteral,
        TypedArrayLiteral,
        TupleLiteral,
        PipeInsert,
        PipeExtract,
        Cast,
        Alloc,
        Free,
        Simd,
        Lambda,
        MapLiteral,
        MapEntriesLiteral,
    };

    /// Variant payload for all expression kinds.
    using expr_value = std::variant<
        int_literal,
        float_literal,
        string_literal,
        char_literal,
        bool_literal,
        null_literal,
        regex_literal,
        identifier,
        binary_expr,
        unary_expr,
        call_expr,
        member_expr,
        enum_access_expr,
        index_expr,
        array_literal,
        typed_array_literal,
        tuple_literal,
        pipe_insert_expr,
        pipe_extract_expr,
        cast_expr,
        alloc_expr,
        free_expr,
        simd_expr,
        lambda_expr,
        map_literal,
        map_entries_literal>;

    /// Expression AST node: location + discriminator + payload.
    struct expr_node
    {
        source_loc loc{};                           ///< Start of this expression.
        expr_type type{expr_type::NullLiteral};     ///< Active alternative.
        expr_value value{null_literal{}};           ///< Typed payload.
    };

    /// Build an @ref expr_node from @p payload, setting @ref expr_type from the variant index.
    template <typename T>
    inline expr_node make_expr(T payload, source_loc loc = {})
    {
        expr_node node{};
        node.loc = loc;
        node.value = std::move(payload);
        node.type = static_cast<expr_type>(node.value.index());
        return node;
    }

    /// Heap-allocate an expression node (see @ref make_expr).
    template <typename T>
    inline std::unique_ptr<expr_node> make_expr_ptr(T payload, source_loc loc = {})
    {
        return std::make_unique<expr_node>(make_expr(std::move(payload), loc));
    }

    /// @return Mutable reference to the @p T alternative of @p node.
    template <typename T>
    inline T &as(expr_node &node)
    {
        return std::get<T>(node.value);
    }

    /// @return Const reference to the @p T alternative of @p node.
    template <typename T>
    inline const T &as(const expr_node &node)
    {
        return std::get<T>(node.value);
    }

    // -------------------------------------------------------------------------
    // Statements & declarations
    // -------------------------------------------------------------------------

    /// Left-hand side of an assignment / destructure (`name` or `_`).
    struct bind_target
    {
        source_loc loc{};      ///< Location of the target token.
        bool is_discard{false}; ///< True when the target is `_`.
        std::string name{};     ///< Bound name (empty when discarded).
    };

    /// Assignment statement `targets = value` or `+=`.
    struct assignment_stmt
    {
        std::vector<bind_target> targets;    ///< One or more bind targets.
        assign_op op;                        ///< `=` or `+=`.
        std::unique_ptr<expr_node> value;    ///< Right-hand side.
    };

    /// Expression used as a statement.
    struct expr_stmt
    {
        std::unique_ptr<expr_node> expression; ///< Evaluated for side effects.
    };

    /// `return` or `return expr`.
    struct return_stmt
    {
        std::optional<std::unique_ptr<expr_node>> value; ///< Absent for bare `return`.
    };

    /// `break` inside a loop.
    struct break_stmt
    {
    };

    /// Brace-delimited statement list.
    struct block_stmt
    {
        source_loc loc{}; ///< Location of the opening `{`.
        std::vector<std::unique_ptr<stmt_node>> statements; ///< Body statements.
    };

    /// One `if` / `else if` arm.
    struct if_branch
    {
        std::unique_ptr<expr_node> condition; ///< Branch condition.
        std::unique_ptr<block_stmt> body;     ///< Branch body.
    };

    /// `if` / `else if` / `else` chain.
    struct if_stmt
    {
        std::unique_ptr<if_branch> then_branch;                       ///< First arm.
        std::vector<std::unique_ptr<if_branch>> else_if_branches;     ///< Extra arms.
        std::optional<std::unique_ptr<block_stmt>> else_branch;       ///< Optional else.
    };

    /// `loop` or `loop cond { … }`.
    struct loop_stmt
    {
        std::optional<std::unique_ptr<expr_node>> condition; ///< Absent ⇒ infinite loop.
        std::unique_ptr<block_stmt> body;                   ///< Loop body.
    };

    /// One `case Enum::Member { … }` arm.
    struct match_case
    {
        source_loc loc{};                  ///< Location of the `case` keyword.
        std::string enum_name;             ///< Enum type name.
        std::string member;                ///< Enumerant name.
        std::unique_ptr<block_stmt> body;  ///< Case body.
    };

    /// `match` statement over an enum scrutinee.
    struct match_stmt
    {
        std::unique_ptr<expr_node> scrutinee; ///< Value being matched.
        std::vector<match_case> cases;       ///< Case arms (may be empty).
    };

    /// Function declaration.
    struct func_decl
    {
        std::string name;                         ///< Function name.
        std::vector<parameter> parameters;        ///< Parameters.
        std::unique_ptr<type_node> return_type;   ///< Return type.
        std::unique_ptr<block_stmt> body;         ///< Function body.
    };

    /// Enum declaration.
    struct enum_decl
    {
        std::string name;                 ///< Enum name.
        std::vector<std::string> members; ///< Enumerant names.
    };

    /// One field of an object declaration.
    struct object_field
    {
        source_loc loc{};                ///< Location of the field name.
        std::string name;                ///< Field name.
        std::unique_ptr<type_node> type; ///< Field type.
    };

    /// Object (struct) declaration.
    struct object_decl
    {
        std::string name;                  ///< Object type name.
        std::vector<object_field> fields;  ///< Fields.
    };

    /// `monitor { … } trap(name: type) { … }`.
    struct monitor_stmt
    {
        std::unique_ptr<block_stmt> protected_block; ///< Guarded block.
        std::string trap_name;                       ///< Trap parameter name.
        std::unique_ptr<type_node> trap_type;        ///< Trap parameter type.
        std::unique_ptr<block_stmt> handler;         ///< Handler block.
    };

    /// `lock name` declaration.
    struct lock_stmt
    {
        std::string lock_name; ///< Lock identifier.
    };

    /// `acquire name` — enter a critical section.
    struct acquire_stmt
    {
        std::string lock_name; ///< Lock identifier.
    };

    /// `release name` — leave a critical section.
    struct release_stmt
    {
        std::string lock_name; ///< Lock identifier.
    };

    /// `load_package name` (also used for each entry of `load_packages {…}`).
    struct load_package_stmt
    {
        source_loc loc{};    ///< Location of the import keyword or package name.
        std::string package; ///< Imported package identifier.
    };

    /// `receiver insert(map, map_literal)` — merge entries into a map.
    struct insert_stmt
    {
        std::string receiver;                  ///< Map variable name (must match first arg).
        std::unique_ptr<expr_node> map_expr;   ///< Map to mutate.
        std::unique_ptr<expr_node> entries;    ///< Map literal with new entries.
    };

    /// Discriminator for @ref stmt_node::value (must match variant order).
    enum class stmt_type
    {
        Assignment,
        Expr,
        Return,
        Break,
        Block,
        If,
        Loop,
        Match,
        FuncDecl,
        EnumDecl,
        ObjectDecl,
        Monitor,
        Lock,
        Acquire,
        Release,
        LoadPackage,
        Insert,
    };

    /// Variant payload for all statement kinds.
    using stmt_value = std::variant<
        assignment_stmt,
        expr_stmt,
        return_stmt,
        break_stmt,
        block_stmt,
        if_stmt,
        loop_stmt,
        match_stmt,
        func_decl,
        enum_decl,
        object_decl,
        monitor_stmt,
        lock_stmt,
        acquire_stmt,
        release_stmt,
        load_package_stmt,
        insert_stmt>;

    /// Statement AST node: location + discriminator + payload.
    struct stmt_node
    {
        source_loc loc{};                 ///< Start of this statement.
        stmt_type type{stmt_type::Break}; ///< Active alternative.
        stmt_value value{break_stmt{}};   ///< Typed payload.
    };

    /// Build a @ref stmt_node from @p payload, setting @ref stmt_type from the variant index.
    template <typename T>
    inline stmt_node make_stmt(T payload, source_loc loc = {})
    {
        stmt_node node{};
        node.loc = loc;
        node.value = std::move(payload);
        node.type = static_cast<stmt_type>(node.value.index());
        return node;
    }

    /// Heap-allocate a statement node (see @ref make_stmt).
    template <typename T>
    inline std::unique_ptr<stmt_node> make_stmt_ptr(T payload, source_loc loc = {})
    {
        return std::make_unique<stmt_node>(make_stmt(std::move(payload), loc));
    }

    /// @return Mutable reference to the @p T alternative of @p node.
    template <typename T>
    inline T &as_stmt(stmt_node &node)
    {
        return std::get<T>(node.value);
    }

    /// @return Const reference to the @p T alternative of @p node.
    template <typename T>
    inline const T &as_stmt(const stmt_node &node)
    {
        return std::get<T>(node.value);
    }

    /// Root of a compiled munx translation unit.
    struct program
    {
        source_loc package_loc{};                           ///< Location of the `package` keyword.
        std::string package_name;                           ///< Package identifier.
        std::vector<load_package_stmt> imports;             ///< Import header entries.
        std::vector<std::unique_ptr<stmt_node>> statements; ///< Top-level statements.
    };

} // namespace munx::ast

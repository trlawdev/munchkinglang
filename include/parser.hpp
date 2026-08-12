#pragma once

#include "ast.hpp"
#include "errors.hpp"
#include "keywords.hpp"
#include "token.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace munx
{

    /// Recursive-descent parser: token stream → @ref ast::program.
    class parser
    {
        std::vector<token> tokens_;       ///< Token stream (includes trailing END).
        std::size_t token_pos_{0};        ///< Index of the current lookahead token.
        std::filesystem::path path_;      ///< Source path for diagnostics / locations.

    public:
        /// Take ownership of @p tokens for the source file at @p path.
        parser(std::vector<token> tokens, std::filesystem::path path)
            : tokens_(std::move(tokens)), path_(std::move(path))
        {
        }

        /// Parse a full translation unit (`package`, imports, statements).
        ast::program parse_program()
        {
            ast::program prog;
            expect_kw("package");
            prog.package_loc = loc_of(prev());
            prog.package_name = expect_name();

            // Import header: both forms may repeat and interleave, but only
            // until the first non-import statement (see parse_statement).
            while (true)
            {
                if (match_kw("load_package"))
                {
                    prog.imports.emplace_back(parse_load_package());
                }
                else if (match_kw("load_packages"))
                {
                    parse_load_packages(prog.imports);
                }
                else
                {
                    break;
                }
            }

            while (!at_end())
            {
                prog.statements.emplace_back(parse_statement());
                match(token_type::SEMICOLON);
            }
            return prog;
        }

    private:
        // ---- token helpers --------------------------------------------------

        /// @return The current lookahead token.
        const token &cur() const { return tokens_[token_pos_]; }
        /// @return The most recently consumed token.
        const token &prev() const { return tokens_[token_pos_ - 1]; }
        /// @return True when the current token is @c END.
        bool at_end() const { return cur().type == token_type::END; }

        // Returns a view into the token's own storage; valid as long as
        // tokens_ is alive (it is never mutated after construction).
        /// @return String payload of @p t, or empty if the payload is numeric.
        static const std::string &text(const token &t)
        {
            static const std::string empty{};
            const std::string *s = std::get_if<std::string>(&t.value);
            return s ? *s : empty;
        }

        /// Build a @ref ast::source_loc for @p t using this parser's path.
        ast::source_loc loc_of(const token &t) const
        {
            return ast::source_loc{path_.string(), t.line, t.column};
        }

        /// @return Location of the current lookahead token.
        ast::source_loc here() const { return loc_of(cur()); }

        /// @return True if the current token has kind @p type.
        bool check(token_type type) const
        {
            return !at_end() && cur().type == type;
        }

        /// @return True if the current token is keyword @p kw.
        bool check_kw(std::string_view kw) const
        {
            return check(token_type::KEYWORD) && text(cur()) == kw;
        }

        /// @return True if the current token can serve as an identifier.
        bool check_name() const
        {
            return check(token_type::SYMBOL) || check(token_type::KEYWORD);
        }

        /// Consume the current token and return the previous one.
        const token &advance()
        {
            if (!at_end())
            {
                ++token_pos_;
            }
            return prev();
        }

        /// Consume the current token if it has kind @p type.
        bool match(token_type type)
        {
            if (!check(type))
            {
                return false;
            }
            advance();
            return true;
        }

        /// Consume the current token if it is keyword @p kw.
        bool match_kw(std::string_view kw)
        {
            if (!check_kw(kw))
            {
                return false;
            }
            advance();
            return true;
        }

        /// Record a compile error at @p t with message @p msg.
        void error(const token &t, const std::string &msg)
        {
            fail_compile(path_.string() + ':' + std::to_string(t.line) +
                                    ':' + std::to_string(t.column) + ": error: " + msg);
        }

        /// Record a compile error at the current token.
        void error_here(const std::string &msg) { error(cur(), msg); }

        /// Require kind @p type or throw with @p msg; return the consumed token.
        const token &expect(token_type type, const char *msg)
        {
            if (check(type))
            {
                return advance();
            }
            error_here(msg);
            return cur();
        }

        /// Require keyword @p kw or throw.
        void expect_kw(std::string_view kw)
        {
            if (!match_kw(kw))
            {
                error_here("expected `" + std::string{kw} + "`");
            }
        }

        /// Require an identifier-like token and return its text.
        std::string expect_name()
        {
            if (check_name())
            {
                return text(advance());
            }
            error_here("expected identifier");
            return {};
        }

        // ---- types ----------------------------------------------------------

        /// Parse a type annotation (`tuple[…]`, `map[…]`, `Lambda[…]`, `[T]`, primitive, or named).
        std::unique_ptr<ast::type_node> parse_type()
        {
            if (check_kw("tuple"))
            {
                const auto loc = here();
                advance();
                expect(token_type::LBRACKET, "expected `[` after `tuple`");
                auto node = std::make_unique<ast::type_node>();
                node->loc = loc;
                node->type = ast::type_kind::Tuple;
                node->value = ast::tuple_type{};
                if (!check(token_type::RBRACKET))
                {
                    do
                    {
                        std::get<ast::tuple_type>(node->value).elements.push_back(parse_type());
                    } while (match(token_type::COMMA));
                }
                expect(token_type::RBRACKET, "expected `]` after tuple type");
                return node;
            }

            if (check_kw("map"))
            {
                const auto loc = here();
                advance();
                expect(token_type::LBRACKET, "expected `[` after `map`");
                auto key = parse_type();
                expect(token_type::ARROW, "expected `=>` in map type");
                auto val = parse_type();
                expect(token_type::RBRACKET, "expected `]` after map type");
                auto node = std::make_unique<ast::type_node>();
                node->loc = loc;
                node->type = ast::type_kind::Map;
                node->value = ast::map_type{std::move(key), std::move(val)};
                return node;
            }

            if (check(token_type::LBRACKET))
            {
                const auto loc = here();
                advance();
                auto element = parse_type();
                expect(token_type::RBRACKET, "expected `]` after array type");
                auto node = std::make_unique<ast::type_node>();
                node->loc = loc;
                node->type = ast::type_kind::Array;
                node->value = ast::array_type{std::move(element)};
                return node;
            }

            const auto loc = here();
            const std::string name = expect_name();
            if (name == "Lambda")
            {
                expect(token_type::LBRACKET, "expected `[` after `Lambda`");
                expect(token_type::LBRACE, "expected `{` after `Lambda[`");
                ast::lambda_type lambda{};
                if (!check(token_type::RBRACE))
                {
                    do
                    {
                        lambda.params.push_back(parse_type());
                    } while (match(token_type::COMMA));
                }
                expect(token_type::RBRACE, "expected `}` in Lambda parameter list");
                expect(token_type::ARROW, "expected `=>` in Lambda type");
                lambda.ret = parse_type();
                expect(token_type::RBRACKET, "expected `]` after Lambda type");
                auto node = std::make_unique<ast::type_node>();
                node->loc = loc;
                node->type = ast::type_kind::Lambda;
                node->value = std::move(lambda);
                return node;
            }
            auto node = std::make_unique<ast::type_node>();
            if (name == "int")
            {
                *node = ast::type_node::make_primitive(ast::primitive_kind::Int, loc);
            }
            else if (name == "float")
            {
                *node = ast::type_node::make_primitive(ast::primitive_kind::Float, loc);
            }
            else if (name == "bool")
            {
                *node = ast::type_node::make_primitive(ast::primitive_kind::Bool, loc);
            }
            else if (name == "string")
            {
                *node = ast::type_node::make_primitive(ast::primitive_kind::String, loc);
            }
            else if (name == "character")
            {
                *node = ast::type_node::make_primitive(ast::primitive_kind::Character, loc);
            }
            else if (name == "void")
            {
                *node = ast::type_node::make_primitive(ast::primitive_kind::Void, loc);
            }
            else if (name == "socket")
            {
                *node = ast::type_node::make_primitive(ast::primitive_kind::Socket, loc);
            }
            else if (name == "file")
            {
                *node = ast::type_node::make_primitive(ast::primitive_kind::File, loc);
            }
            else if (name == "term")
            {
                *node = ast::type_node::make_primitive(ast::primitive_kind::Term, loc);
            }
            else if (name == "exception")
            {
                *node = ast::type_node::make_primitive(ast::primitive_kind::Exception, loc);
            }
            else
            {
                *node = ast::type_node::make_named(name, loc);
            }
            return node;
        }

        /// Parse a comma-separated parameter list (no surrounding parentheses).
        std::vector<ast::parameter> parse_params()
        {
            std::vector<ast::parameter> params;
            if (check(token_type::RPAREN))
            {
                return params;
            }
            do
            {
                ast::parameter p;
                p.loc = here();
                p.name = expect_name();
                expect(token_type::COLON, "expected `:` after parameter name");
                p.type = parse_type();
                params.push_back(std::move(p));
            } while (match(token_type::COMMA));
            return params;
        }

        // ---- expressions (Pratt / precedence climbing) ----------------------

        /// Parse a full expression (entry point for expression grammar).
        std::unique_ptr<ast::expr_node> parse_expression() { return parse_pipe(); }

        /// Parse pipe/channel-insert expressions (`value -> name`, `value :=> name`).
        std::unique_ptr<ast::expr_node> parse_pipe()
        {
            auto left = parse_or();
            if (match(token_type::PIPE_INSERT))
            {
                const auto loc = left->loc;
                return ast::make_expr_ptr(
                    ast::pipe_insert_expr{std::move(left), expect_name()}, loc);
            }
            if (match(token_type::CHANNEL_INSERT))
            {
                const auto loc = left->loc;
                return ast::make_expr_ptr(
                    ast::channel_insert_expr{std::move(left), expect_name()}, loc);
            }
            return left;
        }

        /// Parse `||` expressions.
        std::unique_ptr<ast::expr_node> parse_or()
        {
            auto left = parse_and();
            while (match(token_type::OR_OR))
            {
                left = make_binary(ast::binary_op::Or, std::move(left), parse_and());
            }
            return left;
        }

        /// Parse `&&` expressions.
        std::unique_ptr<ast::expr_node> parse_and()
        {
            auto left = parse_equality();
            while (match(token_type::AND_AND))
            {
                left = make_binary(ast::binary_op::And, std::move(left), parse_equality());
            }
            return left;
        }

        /// Parse `==` / `!=` expressions.
        std::unique_ptr<ast::expr_node> parse_equality()
        {
            auto left = parse_relational();
            while (true)
            {
                if (match(token_type::EQ))
                {
                    left = make_binary(ast::binary_op::Eq, std::move(left), parse_relational());
                }
                else if (match(token_type::NE))
                {
                    left = make_binary(ast::binary_op::Ne, std::move(left), parse_relational());
                }
                else
                {
                    break;
                }
            }
            return left;
        }

        /// Parse `<` / `>` / `<=` / `>=` expressions.
        std::unique_ptr<ast::expr_node> parse_relational()
        {
            auto left = parse_additive();
            while (true)
            {
                if (match(token_type::LT))
                {
                    left = make_binary(ast::binary_op::Lt, std::move(left), parse_additive());
                }
                else if (match(token_type::GT))
                {
                    left = make_binary(ast::binary_op::Gt, std::move(left), parse_additive());
                }
                else if (match(token_type::LE))
                {
                    left = make_binary(ast::binary_op::Le, std::move(left), parse_additive());
                }
                else if (match(token_type::GE))
                {
                    left = make_binary(ast::binary_op::Ge, std::move(left), parse_additive());
                }
                else
                {
                    break;
                }
            }
            return left;
        }

        /// Parse `+` / `-` expressions.
        std::unique_ptr<ast::expr_node> parse_additive()
        {
            auto left = parse_multiplicative();
            while (true)
            {
                if (match(token_type::PLUS))
                {
                    left = make_binary(ast::binary_op::Add, std::move(left), parse_multiplicative());
                }
                else if (match(token_type::MINUS))
                {
                    left = make_binary(ast::binary_op::Sub, std::move(left), parse_multiplicative());
                }
                else
                {
                    break;
                }
            }
            return left;
        }

        /// Parse `*` / `/` / `%` expressions.
        std::unique_ptr<ast::expr_node> parse_multiplicative()
        {
            auto left = parse_unary();
            while (true)
            {
                if (match(token_type::STAR))
                {
                    left = make_binary(ast::binary_op::Mul, std::move(left), parse_unary());
                }
                else if (match(token_type::SLASH))
                {
                    left = make_binary(ast::binary_op::Div, std::move(left), parse_unary());
                }
                else if (match(token_type::PERCENT))
                {
                    left = make_binary(ast::binary_op::Mod, std::move(left), parse_unary());
                }
                else
                {
                    break;
                }
            }
            return left;
        }

        /// Parse unary `!` / `~` / `-` / pipe-extract / channel-extract.
        std::unique_ptr<ast::expr_node> parse_unary()
        {
            if (match(token_type::PIPE_EXTRACT))
            {
                const auto loc = loc_of(prev());
                return ast::make_expr_ptr(ast::pipe_extract_expr{expect_name()}, loc);
            }
            if (match(token_type::CHANNEL_EXTRACT))
            {
                const auto loc = loc_of(prev());
                return ast::make_expr_ptr(
                    ast::channel_extract_expr{expect_name()}, loc);
            }
            if (match(token_type::BANG))
            {
                const auto loc = loc_of(prev());
                return make_unary(ast::unary_op::Not, parse_unary(), loc);
            }
            if (match(token_type::TILDE))
            {
                const auto loc = loc_of(prev());
                return make_unary(ast::unary_op::BitwiseNot, parse_unary(), loc);
            }
            if (match(token_type::MINUS))
            {
                const auto loc = loc_of(prev());
                return make_unary(ast::unary_op::Neg, parse_unary(), loc);
            }
            return parse_postfix();
        }

        /// Parse postfix `.` / call / index chains.
        std::unique_ptr<ast::expr_node> parse_postfix()
        {
            auto expr = parse_primary();
            while (true)
            {
                if (match(token_type::DOT))
                {
                    const auto loc = expr->loc;
                    expr = ast::make_expr_ptr(
                        ast::member_expr{std::move(expr), expect_name()}, loc);
                }
                else if (check(token_type::LT) && looks_like_call_type_args())
                {
                    const auto loc = expr->loc;
                    advance(); // `<`
                    ast::call_expr call;
                    call.callee = std::move(expr);
                    if (!check(token_type::GT))
                    {
                        do
                        {
                            call.type_arguments.push_back(parse_type());
                        } while (match(token_type::COMMA));
                    }
                    expect(token_type::GT, "expected `>` after type arguments");
                    expect(token_type::LPAREN, "expected `(` after type arguments");
                    if (!check(token_type::RPAREN))
                    {
                        do
                        {
                            call.arguments.push_back(parse_expression());
                        } while (match(token_type::COMMA));
                    }
                    expect(token_type::RPAREN, "expected `)` after arguments");
                    expr = ast::make_expr_ptr(std::move(call), loc);
                }
                else if (match(token_type::LPAREN))
                {
                    const auto loc = expr->loc;
                    ast::call_expr call;
                    call.callee = std::move(expr);
                    if (!check(token_type::RPAREN))
                    {
                        do
                        {
                            call.arguments.push_back(parse_expression());
                        } while (match(token_type::COMMA));
                    }
                    expect(token_type::RPAREN, "expected `)` after arguments");
                    expr = ast::make_expr_ptr(std::move(call), loc);
                }
                else if (match(token_type::LBRACKET))
                {
                    const auto loc = expr->loc;
                    auto index = parse_expression();
                    expect(token_type::RBRACKET, "expected `]` after index");
                    expr = ast::make_expr_ptr(
                        ast::index_expr{std::move(expr), std::move(index)}, loc);
                }
                else
                {
                    break;
                }
            }
            return expr;
        }

        /// Parse literals, identifiers, casts, alloc, lambda, arrays, tuples, groups.
        std::unique_ptr<ast::expr_node> parse_primary()
        {
            if (match(token_type::INT_LITERAL))
            {
                return ast::make_expr_ptr(
                    ast::int_literal{std::get<long long>(prev().value)},
                    loc_of(prev()));
            }
            if (match(token_type::FLOAT_LITERAL))
            {
                return ast::make_expr_ptr(
                    ast::float_literal{std::get<long double>(prev().value)},
                    loc_of(prev()));
            }
            if (match(token_type::STRING_LITERAL))
            {
                return ast::make_expr_ptr(ast::string_literal{text(prev())},
                                          loc_of(prev()));
            }
            if (match(token_type::CHAR_LITERAL))
            {
                const auto loc = loc_of(prev());
                const std::string s = text(prev());
                return ast::make_expr_ptr(
                    ast::char_literal{s.empty() ? '\0' : s[0]}, loc);
            }
            if (match(token_type::BOOL_LITERAL))
            {
                return ast::make_expr_ptr(
                    ast::bool_literal{text(prev()) == "true"}, loc_of(prev()));
            }
            if (match(token_type::NULL_LITERAL))
            {
                return ast::make_expr_ptr(ast::null_literal{}, loc_of(prev()));
            }
            if (match(token_type::REGEX_LITERAL))
            {
                return ast::make_expr_ptr(ast::regex_literal{text(prev())},
                                          loc_of(prev()));
            }

            if (check_kw("cast"))
            {
                const auto loc = here();
                advance();
                expect(token_type::LBRACKET, "expected `[` after `cast`");
                ast::cast_expr cast;
                cast.target_type = parse_type();
                expect(token_type::RBRACKET, "expected `]` after cast type");
                expect(token_type::LPAREN, "expected `(` after cast type");
                cast.operand = parse_expression();
                expect(token_type::RPAREN, "expected `)` after cast operand");
                return ast::make_expr_ptr(std::move(cast), loc);
            }

            if (check_kw("alloc"))
            {
                const auto loc = here();
                advance();
                ast::alloc_expr alloc;
                expect(token_type::LBRACKET, "expected `[` after `alloc`");
                alloc.capacity = parse_expression();
                expect(token_type::RBRACKET, "expected `]` after alloc size");
                expect(token_type::LBRACKET, "expected `[` for alloc initializers");
                if (!check(token_type::RBRACKET))
                {
                    do
                    {
                        alloc.initial_values.push_back(parse_expression());
                    } while (match(token_type::COMMA));
                }
                expect(token_type::RBRACKET, "expected `]` after alloc initializers");
                return ast::make_expr_ptr(std::move(alloc), loc);
            }

            if (check_kw("delete") || check_kw("free"))
            {
                const auto loc = here();
                advance();
                return ast::make_expr_ptr(ast::free_expr{expect_name()}, loc);
            }

            if (check_kw("simd"))
            {
                const auto loc = here();
                advance();
                expect(token_type::LPAREN, "expected `(` after `simd`");
                ast::simd_expr simd;
                simd.operand = parse_expression();
                expect(token_type::RPAREN, "expected `)` after simd operand");
                return ast::make_expr_ptr(std::move(simd), loc);
            }

            // `likely` / `unlikely` as expression wrappers (also peeled from `if`).
            if (check_kw("likely") || check_kw("unlikely"))
            {
                const auto loc = here();
                const std::string name = text(advance());
                expect(token_type::LPAREN, "expected `(` after branch hint");
                ast::call_expr call;
                call.callee = ast::make_expr_ptr(ast::identifier{name}, loc);
                call.arguments.push_back(parse_expression());
                expect(token_type::RPAREN, "expected `)` after branch hint");
                return ast::make_expr_ptr(std::move(call), loc);
            }

            if (check_kw("this_package"))
            {
                const auto loc = here();
                advance();
                return ast::make_expr_ptr(ast::identifier{"this_package"}, loc);
            }

            if (check_kw("lambda"))
            {
                return parse_lambda();
            }

            if (check_kw("map"))
            {
                return parse_map_literal();
            }

            if (check(token_type::SCOPE))
            {
                return parse_compiler_call_expr();
            }

            if (check_name())
            {
                const auto loc = here();
                const std::string name = text(advance());
                if (match(token_type::SCOPE))
                {
                    return ast::make_expr_ptr(
                        ast::enum_access_expr{name, expect_name()}, loc);
                }
                return ast::make_expr_ptr(ast::identifier{name}, loc);
            }

            if (check(token_type::LBRACKET))
            {
                return parse_array_or_typed_array();
            }

            if (check(token_type::LBRACE))
            {
                const auto loc = here();
                advance();
                if (check(token_type::RBRACE))
                {
                    advance();
                    return ast::make_expr_ptr(ast::tuple_literal{}, loc);
                }
                auto first = parse_expression();
                if (match(token_type::COLON))
                {
                    ast::map_entries_literal entries;
                    ast::map_entry entry;
                    entry.key = std::move(first);
                    entry.value = parse_expression();
                    entries.entries.push_back(std::move(entry));
                    while (match(token_type::COMMA))
                    {
                        ast::map_entry next;
                        next.key = parse_expression();
                        expect(token_type::COLON, "expected `:` after map key");
                        next.value = parse_expression();
                        entries.entries.push_back(std::move(next));
                    }
                    expect(token_type::RBRACE, "expected `}` after map entries");
                    return ast::make_expr_ptr(std::move(entries), loc);
                }
                ast::tuple_literal tuple;
                tuple.elements.push_back(std::move(first));
                while (match(token_type::COMMA))
                {
                    tuple.elements.push_back(parse_expression());
                }
                expect(token_type::RBRACE, "expected `}` after tuple literal");
                return ast::make_expr_ptr(std::move(tuple), loc);
            }

            if (match(token_type::LPAREN))
            {
                auto expr = parse_expression();
                expect(token_type::RPAREN, "expected `)` after expression");
                return expr;
            }

            error_here("expected expression");
            return nullptr;
        }

        /// Parse `[…]` or typed `[T][…]` array literals.
        std::unique_ptr<ast::expr_node> parse_array_or_typed_array()
        {
            // Lookahead: [Type][elems...] vs [elems...]
            const auto loc = here();
            const std::size_t saved = token_pos_;
            advance(); // [
            if (check_name())
            {
                advance();
                if (match(token_type::RBRACKET) && check(token_type::LBRACKET))
                {
                    // typed array: rewind and parse properly
                    token_pos_ = saved;
                    expect(token_type::LBRACKET, "expected `[`");
                    ast::typed_array_literal typed;
                    typed.element_type = parse_type();
                    expect(token_type::RBRACKET, "expected `]` after element type");
                    expect(token_type::LBRACKET, "expected `[` for typed array values");
                    if (!check(token_type::RBRACKET))
                    {
                        do
                        {
                            typed.elements.push_back(parse_expression());
                        } while (match(token_type::COMMA));
                    }
                    expect(token_type::RBRACKET, "expected `]` after typed array values");
                    return ast::make_expr_ptr(std::move(typed), loc);
                }
            }
            token_pos_ = saved;
            expect(token_type::LBRACKET, "expected `[`");
            ast::array_literal arr;
            if (!check(token_type::RBRACKET))
            {
                do
                {
                    arr.elements.push_back(parse_expression());
                } while (match(token_type::COMMA));
            }
            expect(token_type::RBRACKET, "expected `]` after array literal");
            return ast::make_expr_ptr(std::move(arr), loc);
        }

        /// Parse `map[K => V]{ key: value, … }`.
        std::unique_ptr<ast::expr_node> parse_map_literal()
        {
            const auto loc = here();
            advance(); // map
            expect(token_type::LBRACKET, "expected `[` after `map`");
            ast::map_literal literal;
            literal.key_type = parse_type();
            expect(token_type::ARROW, "expected `=>` in map literal");
            literal.value_type = parse_type();
            expect(token_type::RBRACKET, "expected `]` after map type in map literal");
            expect(token_type::LBRACE, "expected `{` after map type");
            if (!check(token_type::RBRACE))
            {
                do
                {
                    ast::map_entry entry;
                    entry.key = parse_expression();
                    expect(token_type::COLON, "expected `:` after map key");
                    entry.value = parse_expression();
                    literal.entries.push_back(std::move(entry));
                } while (match(token_type::COMMA));
            }
            expect(token_type::RBRACE, "expected `}` after map entries");
            return ast::make_expr_ptr(std::move(literal), loc);
        }

        /// Parse a `lambda (params): Ret => { … }` expression.
        std::unique_ptr<ast::expr_node> parse_lambda()
        {
            const auto loc = here();
            expect_kw("lambda");
            expect(token_type::LPAREN, "expected `(` after `lambda`");
            ast::lambda_expr lambda;
            lambda.parameters = parse_params();
            expect(token_type::RPAREN, "expected `)` after lambda parameters");
            expect(token_type::COLON, "expected `:` after lambda parameters");
            lambda.return_type = parse_type();
            expect(token_type::ARROW, "expected `=>` after lambda return type");
            lambda.body = parse_block();
            return ast::make_expr_ptr(std::move(lambda), loc);
        }

        /// Build a binary expression node from @p left, @p op, and @p right.
        static std::unique_ptr<ast::expr_node>
        make_binary(ast::binary_op op, std::unique_ptr<ast::expr_node> left,
                    std::unique_ptr<ast::expr_node> right)
        {
            const auto loc = left->loc;
            return ast::make_expr_ptr(
                ast::binary_expr{op, std::move(left), std::move(right)}, loc);
        }

        /// Build a unary expression node from @p op and @p operand.
        static std::unique_ptr<ast::expr_node>
        make_unary(ast::unary_op op, std::unique_ptr<ast::expr_node> operand,
                   const ast::source_loc &loc)
        {
            return ast::make_expr_ptr(ast::unary_expr{op, std::move(operand)}, loc);
        }

        // ---- statements -----------------------------------------------------

        /// Parse a `{ … }` statement block.
        std::unique_ptr<ast::block_stmt> parse_block()
        {
            const auto loc = here();
            expect(token_type::LBRACE, "expected `{`");
            auto block = std::make_unique<ast::block_stmt>();
            block->loc = loc;
            while (!check(token_type::RBRACE) && !at_end())
            {
                block->statements.push_back(parse_statement());
                match(token_type::SEMICOLON);
            }
            expect(token_type::RBRACE, "expected `}`");
            return block;
        }

        /// Parse one top-level or block-level statement.
        std::unique_ptr<ast::stmt_node> parse_statement()
        {
            // Imports form a header section terminated by the first statement.
            // Reaching one here means it appears too late or inside a scope;
            // without this check it would silently parse as an identifier,
            // because a keyword is accepted wherever a name is expected.
            if (check_kw("load_package") || check_kw("load_packages"))
            {
                error_here('`' + text(cur()) +
                           "` is only allowed at the top of the file, "
                           "before any other statement");
            }

            if (check(token_type::SCOPE))
            {
                return parse_compiler_stmt();
            }
            if (check_kw("func"))
            {
                const auto loc = here();
                advance();
                return parse_func(loc);
            }
            if (check_kw("enum"))
            {
                const auto loc = here();
                advance();
                return parse_enum(loc);
            }
            if (check_kw("object"))
            {
                const auto loc = here();
                advance();
                return parse_object(loc);
            }
            if (check_kw("if"))
            {
                const auto loc = here();
                advance();
                return parse_if(loc);
            }
            if (check_kw("loop"))
            {
                const auto loc = here();
                advance();
                return parse_loop(loc);
            }
            if (check_kw("match"))
            {
                const auto loc = here();
                advance();
                return parse_match(loc);
            }
            if (check_kw("monitor"))
            {
                const auto loc = here();
                advance();
                return parse_monitor(loc);
            }
            if (match_kw("lock"))
            {
                const auto loc = loc_of(prev());
                auto name = expect_name();
                match(token_type::SEMICOLON);
                return ast::make_stmt_ptr(ast::lock_stmt{std::move(name)}, loc);
            }
            if (match_kw("acquire"))
            {
                const auto loc = loc_of(prev());
                return ast::make_stmt_ptr(ast::acquire_stmt{expect_name()}, loc);
            }
            if (match_kw("release"))
            {
                const auto loc = loc_of(prev());
                return ast::make_stmt_ptr(ast::release_stmt{expect_name()}, loc);
            }
            if (match_kw("return"))
            {
                const auto loc = loc_of(prev());
                ast::return_stmt ret;
                if (!check(token_type::RBRACE) && !at_end() && !check(token_type::SEMICOLON))
                {
                    ret.value = parse_expression();
                }
                return ast::make_stmt_ptr(std::move(ret), loc);
            }
            if (match_kw("break"))
            {
                return ast::make_stmt_ptr(ast::break_stmt{}, loc_of(prev()));
            }
            if (check_kw("join"))
            {
                const auto loc = here();
                advance();
                return parse_join(loc);
            }

            // Braced destructure `{a, b} = ...` vs bare block `{ ... }`
            if (check_name())
            {
                const std::size_t saved = token_pos_;
                const auto loc = here();
                const std::string receiver = expect_name();
                if (match_kw("insert"))
                {
                    expect(token_type::LPAREN, "expected `(` after `insert`");
                    ast::insert_stmt insert;
                    insert.receiver = receiver;
                    insert.map_expr = parse_expression();
                    expect(token_type::COMMA, "expected `,` after map in insert");
                    insert.entries = parse_expression();
                    expect(token_type::RPAREN, "expected `)` after insert arguments");
                    match(token_type::SEMICOLON);
                    return ast::make_stmt_ptr(std::move(insert), loc);
                }
                token_pos_ = saved;
            }

            if (looks_like_braced_destructure() || looks_like_bare_destructure() ||
                check(token_type::SYMBOL) || check(token_type::KEYWORD))
            {
                return parse_assign_or_expr();
            }

            if (check(token_type::LBRACE))
            {
                const auto loc = here();
                advance();
                ast::block_stmt block;
                block.loc = loc;
                while (!check(token_type::RBRACE) && !at_end())
                {
                    block.statements.push_back(parse_statement());
                    match(token_type::SEMICOLON);
                }
                expect(token_type::RBRACE, "expected `}`");
                return ast::make_stmt_ptr(std::move(block), loc);
            }

            return parse_assign_or_expr();
        }

        /// Speculative check: `{a, b} =` destructure without consuming tokens permanently.
        bool looks_like_braced_destructure()
        {
            if (!check(token_type::LBRACE))
            {
                return false;
            }
            const std::size_t saved =token_pos_;
            advance();
            while (check_name() || (check(token_type::SYMBOL) && text(cur()) == "_"))
            {
                advance();
                if (!match(token_type::COMMA))
                {
                    break;
                }
            }
            const bool ok = match(token_type::RBRACE) &&
                            (check(token_type::ASSIGN) || check(token_type::ADD_ASSIGN));
           token_pos_ = saved;
            return ok;
        }

        /// Speculative check: `a, b =` destructure without consuming tokens permanently.
        bool looks_like_bare_destructure()
        {
            if (!check_name())
            {
                return false;
            }
            const std::size_t saved =token_pos_;
            advance();
            if (!match(token_type::COMMA))
            {
               token_pos_ = saved;
                return false;
            }
            while (check_name() || (check(token_type::SYMBOL) && text(cur()) == "_"))
            {
                advance();
                if (!match(token_type::COMMA))
                {
                    break;
                }
            }
            const bool ok = check(token_type::ASSIGN) || check(token_type::ADD_ASSIGN);
           token_pos_ = saved;
            return ok;
        }

        /// Parse assignment / destructure bind targets.
        std::vector<ast::bind_target> parse_targets(bool braced)
        {
            std::vector<ast::bind_target> targets;
            if (braced)
            {
                expect(token_type::LBRACE, "expected `{`");
            }
            do
            {
                ast::bind_target t;
                t.loc = here();
                if (check(token_type::SYMBOL) && text(cur()) == "_")
                {
                    advance();
                    t.is_discard = true;
                }
                else
                {
                    t.name = expect_name();
                }
                targets.push_back(std::move(t));
            } while (match(token_type::COMMA));
            if (braced)
            {
                expect(token_type::RBRACE, "expected `}` after destructure");
            }
            return targets;
        }

        /// Parse an assignment statement or fall back to an expression statement.
        std::unique_ptr<ast::stmt_node> parse_assign_or_expr()
        {
            const std::size_t saved = token_pos_;
            const auto loc = here();
            const bool braced = check(token_type::LBRACE);

            if (braced || check_name())
            {
                auto targets = parse_targets(braced);
                ast::assign_op op = ast::assign_op::Assign;
                bool is_assign = false;
                if (match(token_type::ASSIGN))
                {
                    is_assign = true;
                }
                else if (match(token_type::ADD_ASSIGN))
                {
                    is_assign = true;
                    op = ast::assign_op::AddAssign;
                }

                if (is_assign)
                {
                    ast::assignment_stmt assignment;
                    assignment.targets = std::move(targets);
                    assignment.op = op;
                    assignment.value = parse_expression();
                    maybe_attach_lambda_call(assignment.value);
                    return ast::make_stmt_ptr(std::move(assignment), loc);
                }
                token_pos_ = saved;
            }

            auto expression = parse_expression();
            maybe_attach_lambda_call(expression);
            const auto expr_loc = expression->loc;
            return ast::make_stmt_ptr(ast::expr_stmt{std::move(expression)}, expr_loc);
        }

        /// If a `(…)` follows a lambda expression, wrap it as a call.
        void maybe_attach_lambda_call(std::unique_ptr<ast::expr_node> &expr)
        {
            if (expr->type == ast::expr_type::Lambda && check(token_type::LPAREN))
            {
                const auto loc = expr->loc;
                advance();
                ast::call_expr call;
                call.callee = std::move(expr);
                if (!check(token_type::RPAREN))
                {
                    do
                    {
                        call.arguments.push_back(parse_expression());
                    } while (match(token_type::COMMA));
                }
                expect(token_type::RPAREN, "expected `)` after lambda call");
                expr = ast::make_expr_ptr(std::move(call), loc);
            }
        }

        // import = "load_package", identifier  (keyword already consumed)
        /// Finish parsing `load_package name` after the keyword was consumed.
        ast::load_package_stmt parse_load_package()
        {
            const auto loc = loc_of(prev());
            return ast::load_package_stmt{loc, expect_name()};
        }

        // imports = "load_packages", "{", identifier, {",", identifier}, "}"
        /// Parse `load_packages { a, b, … }` into @p imports.
        void parse_load_packages(std::vector<ast::load_package_stmt> &imports)
        {
            const auto list_loc = loc_of(prev());
            expect(token_type::LBRACE, "expected `{` after `load_packages`");
            do
            {
                const auto loc = here();
                imports.emplace_back(ast::load_package_stmt{loc, expect_name()});
            } while (match(token_type::COMMA));
            expect(token_type::RBRACE, "expected `}` after package list");
            (void)list_loc;
        }

        /// Parse `join [names…]` as a call expression statement.
        std::unique_ptr<ast::stmt_node> parse_join(const ast::source_loc &loc)
        {
            expect(token_type::LBRACKET, "expected `[` after `join`");
            ast::call_expr call;
            call.callee = ast::make_expr_ptr(ast::identifier{"join"}, loc);
            if (!check(token_type::RBRACKET))
            {
                do
                {
                    const auto name_loc = here();
                    call.arguments.push_back(
                        ast::make_expr_ptr(ast::identifier{expect_name()}, name_loc));
                } while (match(token_type::COMMA));
            }
            expect(token_type::RBRACKET, "expected `]` after join list");
            return ast::make_stmt_ptr(
                ast::expr_stmt{ast::make_expr_ptr(std::move(call), loc)}, loc);
        }

        /// Parse a `func` declaration (keyword already consumed).
        std::unique_ptr<ast::stmt_node> parse_func(const ast::source_loc &loc)
        {
            ast::func_decl func;
            func.name = expect_name();
            if (match(token_type::LT))
            {
                if (!check(token_type::GT))
                {
                    do
                    {
                        func.type_params.push_back(expect_name());
                    } while (match(token_type::COMMA));
                }
                expect(token_type::GT, "expected `>` after type parameters");
            }
            expect(token_type::LPAREN, "expected `(` after function name");
            func.parameters = parse_params();
            expect(token_type::RPAREN, "expected `)` after parameters");
            expect(token_type::COLON, "expected `:` before return type");
            func.return_type = parse_type();
            func.body = parse_block();
            return ast::make_stmt_ptr(std::move(func), loc);
        }

        /// Parse an `enum` declaration (keyword already consumed).
        std::unique_ptr<ast::stmt_node> parse_enum(const ast::source_loc &loc)
        {
            ast::enum_decl decl;
            decl.name = expect_name();
            expect(token_type::LBRACE, "expected `{` after enum name");
            if (!check(token_type::RBRACE))
            {
                do
                {
                    decl.members.push_back(expect_name());
                } while (match(token_type::COMMA));
            }
            expect(token_type::RBRACE, "expected `}` after enum members");
            return ast::make_stmt_ptr(std::move(decl), loc);
        }

        /// Parse an `object` declaration (keyword already consumed).
        std::unique_ptr<ast::stmt_node> parse_object(const ast::source_loc &loc)
        {
            ast::object_decl object;
            object.name = expect_name();
            expect(token_type::LBRACE, "expected `{` after object name");
            if (!check(token_type::RBRACE))
            {
                do
                {
                    ast::object_field field;
                    field.loc = here();
                    field.name = expect_name();
                    expect(token_type::COLON, "expected `:` after field name");
                    field.type = parse_type();
                    object.fields.push_back(std::move(field));
                } while (match(token_type::COMMA));
            }
            expect(token_type::RBRACE, "expected `}` after object fields");
            return ast::make_stmt_ptr(std::move(object), loc);
        }

        /// Peel `likely(expr)` / `unlikely(expr)` wrappers off an if-condition.
        static void apply_branch_hint(ast::if_branch &branch)
        {
            if (branch.condition == nullptr ||
                branch.condition->type != ast::expr_type::Call)
            {
                return;
            }
            auto &call = ast::as<ast::call_expr>(*branch.condition);
            if (call.arguments.size() != 1 ||
                call.callee->type != ast::expr_type::Identifier)
            {
                return;
            }
            const std::string &name = ast::as<ast::identifier>(*call.callee).name;
            if (name != "likely" && name != "unlikely")
            {
                return;
            }
            branch.hint = name == "likely" ? ast::branch_hint::Likely
                                           : ast::branch_hint::Unlikely;
            auto inner = std::move(call.arguments[0]);
            branch.condition = std::move(inner);
        }

        /// Parse an `if` / `else if` / `else` chain (keyword already consumed).
        std::unique_ptr<ast::stmt_node> parse_if(const ast::source_loc &loc)
        {
            ast::if_stmt stmt;
            stmt.then_branch = std::make_unique<ast::if_branch>();
            stmt.then_branch->condition = parse_expression();
            apply_branch_hint(*stmt.then_branch);
            stmt.then_branch->body = parse_block();

            while (match_kw("else"))
            {
                if (match_kw("if"))
                {
                    auto branch = std::make_unique<ast::if_branch>();
                    branch->condition = parse_expression();
                    apply_branch_hint(*branch);
                    branch->body = parse_block();
                    stmt.else_if_branches.push_back(std::move(branch));
                }
                else
                {
                    stmt.else_branch = parse_block();
                    break;
                }
            }
            return ast::make_stmt_ptr(std::move(stmt), loc);
        }

        /// Parse a `loop` statement (keyword already consumed).
        std::unique_ptr<ast::stmt_node> parse_loop(const ast::source_loc &loc)
        {
            ast::loop_stmt loop;
            if (!check(token_type::LBRACE))
            {
                loop.condition = parse_expression();
            }
            loop.body = parse_block();
            return ast::make_stmt_ptr(std::move(loop), loc);
        }

        /// Parse a `match` statement (keyword already consumed).
        std::unique_ptr<ast::stmt_node> parse_match(const ast::source_loc &loc)
        {
            ast::match_stmt match_s;
            match_s.scrutinee = parse_expression();
            expect(token_type::LBRACE, "expected `{` after match scrutinee");
            while (match_kw("case"))
            {
                ast::match_case arm;
                arm.loc = loc_of(prev());
                arm.enum_name = expect_name();
                expect(token_type::SCOPE, "expected `::` in match case");
                arm.member = expect_name();
                arm.body = parse_block();
                match_s.cases.push_back(std::move(arm));
            }
            expect(token_type::RBRACE, "expected `}` after match cases");
            return ast::make_stmt_ptr(std::move(match_s), loc);
        }

        /// Parse a `monitor` / `trap` statement (keyword already consumed).
        std::unique_ptr<ast::stmt_node> parse_monitor(const ast::source_loc &loc)
        {
            ast::monitor_stmt monitor;
            monitor.protected_block = parse_block();
            expect_kw("trap");
            expect(token_type::LPAREN, "expected `(` after `trap`");
            monitor.trap_name = expect_name();
            expect(token_type::COLON, "expected `:` after trap parameter");
            monitor.trap_type = parse_type();
            expect(token_type::RPAREN, "expected `)` after trap parameter");
            monitor.handler = parse_block();
            return ast::make_stmt_ptr(std::move(monitor), loc);
        }

        /// True when `<…>(` looks like generic call type arguments (not `a < b`).
        bool looks_like_call_type_args() const
        {
            if (!check(token_type::LT))
            {
                return false;
            }
            std::size_t i = token_pos_ + 1;
            int depth = 1;
            while (i < tokens_.size() && depth > 0)
            {
                const token_type t = tokens_[i].type;
                if (t == token_type::LT)
                {
                    ++depth;
                }
                else if (t == token_type::GT)
                {
                    --depth;
                    if (depth == 0)
                    {
                        return i + 1 < tokens_.size() &&
                               tokens_[i + 1].type == token_type::LPAREN;
                    }
                }
                else if (t == token_type::END || t == token_type::SEMICOLON)
                {
                    return false;
                }
                ++i;
            }
            return false;
        }

        /// `::reflexpr(…)` / `::members(…)` as a call with callee name `::name`.
        std::unique_ptr<ast::expr_node> parse_compiler_call_expr()
        {
            const auto loc = here();
            expect(token_type::SCOPE, "expected `::`");
            if (!check_kw("reflexpr") && !check_kw("members") &&
                !check_kw("function_members") && !check_kw("meta_params") &&
                !check_kw("params") && !check_kw("construct"))
            {
                error_here(
                    "expected `reflexpr`, `members`, `function_members`, "
                    "`meta_params`, `params`, or `construct` after `::`");
            }
            const std::string name = "::" + text(advance());
            expect(token_type::LPAREN, "expected `(` after compiler form");
            ast::call_expr call;
            call.callee = ast::make_expr_ptr(ast::identifier{name}, loc);
            if (!check(token_type::RPAREN))
            {
                do
                {
                    call.arguments.push_back(parse_expression());
                } while (match(token_type::COMMA));
            }
            expect(token_type::RPAREN, "expected `)` after compiler form");
            return ast::make_expr_ptr(std::move(call), loc);
        }

        /// Statement-level `::reflect_for` / `::match`.
        std::unique_ptr<ast::stmt_node> parse_compiler_stmt()
        {
            const auto loc = here();
            expect(token_type::SCOPE, "expected `::`");
            if (match_kw("reflect_for"))
            {
                expect(token_type::LPAREN, "expected `(` after `::reflect_for`");
                ast::reflect_for_stmt stmt;
                stmt.item_name = expect_name();
                expect_kw("of");
                stmt.collection = parse_expression();
                expect(token_type::RPAREN, "expected `)` after reflect_for header");
                stmt.body = parse_block();
                return ast::make_stmt_ptr(std::move(stmt), loc);
            }
            if (match_kw("match"))
            {
                ast::typeid_match_stmt stmt;
                stmt.scrutinee = parse_expression();
                expect(token_type::LBRACE, "expected `{` after `::match`");
                while (!check(token_type::RBRACE) && !at_end())
                {
                    expect(token_type::SCOPE, "expected `::case` or `::default`");
                    ast::typeid_match_case arm;
                    arm.loc = loc_of(prev());
                    if (match_kw("default"))
                    {
                        arm.is_default = true;
                    }
                    else
                    {
                        expect_kw("case");
                        expect_kw("typeid");
                        expect(token_type::LPAREN, "expected `(` after `typeid`");
                        arm.type_kind_name = expect_name();
                        expect(token_type::RPAREN, "expected `)` after typeid");
                    }
                    expect(token_type::ARROW, "expected `=>` in typeid match arm");
                    arm.body = parse_expression();
                    stmt.cases.push_back(std::move(arm));
                    match(token_type::SEMICOLON);
                    if (arm.is_default)
                    {
                        break;
                    }
                }
                expect(token_type::RBRACE, "expected `}` after `::match`");
                return ast::make_stmt_ptr(std::move(stmt), loc);
            }
            error_here("expected `reflect_for` or `match` after `::`");
            return ast::make_stmt_ptr(ast::break_stmt{}, loc);
        }
    };

} // namespace munx

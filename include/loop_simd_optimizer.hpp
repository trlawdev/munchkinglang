#pragma once

#include "ast.hpp"
#include "type_checker.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace munx
{

/// Matched element-wise array loop eligible for SIMD lowering.
struct simd_loop_match
{
    std::string out_var;
    std::string left_array;
    std::string right_array;
    ast::binary_op op{ast::binary_op::Add};
    std::string counter;
    std::string len_array;
    size_t consumed_statements{2};
};

namespace detail
{

[[nodiscard]] inline std::optional<std::string>
identifier_name(const ast::expr_node &expr)
{
    if (expr.type != ast::expr_type::Identifier)
    {
        return std::nullopt;
    }
    return ast::as<ast::identifier>(expr).name;
}

[[nodiscard]] inline bool references_identifier(const ast::expr_node &expr,
                                                std::string_view name)
{
    return identifier_name(expr) == name;
}

[[nodiscard]] inline std::optional<int64_t> int_literal_value(const ast::expr_node &expr)
{
    if (expr.type != ast::expr_type::IntLiteral)
    {
        return std::nullopt;
    }
    return static_cast<int64_t>(ast::as<ast::int_literal>(expr).value);
}

[[nodiscard]] inline std::optional<std::string>
assignment_target_name(const ast::assignment_stmt &assign)
{
    if (assign.targets.size() != 1 || assign.targets.front().is_discard)
    {
        return std::nullopt;
    }
    return assign.targets.front().name;
}

[[nodiscard]] inline bool is_empty_array_init(const ast::stmt_node &stmt)
{
    if (stmt.type != ast::stmt_type::Assignment)
    {
        return false;
    }
    const auto &assign = ast::as_stmt<ast::assignment_stmt>(stmt);
    if (assign.op != ast::assign_op::Assign || !assign.value)
    {
        return false;
    }
    if (assign.value->type == ast::expr_type::ArrayLiteral)
    {
        return ast::as<ast::array_literal>(*assign.value).elements.empty();
    }
    if (assign.value->type == ast::expr_type::TypedArrayLiteral)
    {
        return ast::as<ast::typed_array_literal>(*assign.value).elements.empty();
    }
    return false;
}

[[nodiscard]] inline bool body_increments_counter(const ast::block_stmt &body,
                                                  std::string_view counter)
{
    if (body.statements.empty())
    {
        return false;
    }
    const ast::stmt_node &last = *body.statements.back();
    if (last.type != ast::stmt_type::Assignment)
    {
        return false;
    }
    const auto &assign = ast::as_stmt<ast::assignment_stmt>(last);
    const auto target = assignment_target_name(assign);
    if (!target.has_value() || *target != counter || !assign.value)
    {
        return false;
    }
    if (assign.op == ast::assign_op::AddAssign)
    {
        const auto step = int_literal_value(*assign.value);
        return step.has_value() && *step == 1;
    }
    if (assign.op != ast::assign_op::Assign || assign.value->type != ast::expr_type::Binary)
    {
        return false;
    }
    const auto &binary = ast::as<ast::binary_expr>(*assign.value);
    return binary.op == ast::binary_op::Add && references_identifier(*binary.left, counter) &&
           int_literal_value(*binary.right) == std::optional<int64_t>{1};
}

[[nodiscard]] inline bool match_len_bound(const ast::expr_node &condition,
                                          std::string_view counter,
                                          std::string *len_array)
{
    if (condition.type != ast::expr_type::Binary)
    {
        return false;
    }
    const auto &compare = ast::as<ast::binary_expr>(condition);
    if (compare.op != ast::binary_op::Lt ||
        !references_identifier(*compare.left, counter))
    {
        return false;
    }
    if (compare.right->type != ast::expr_type::Member)
    {
        return false;
    }
    const auto &member = ast::as<ast::member_expr>(*compare.right);
    if (member.member != "len")
    {
        return false;
    }
    const auto array_name = identifier_name(*member.object);
    if (!array_name.has_value())
    {
        return false;
    }
    *len_array = *array_name;
    return true;
}

[[nodiscard]] inline std::optional<std::string>
parse_index_array(const ast::expr_node &expr, std::string_view counter)
{
    if (expr.type != ast::expr_type::Index)
    {
        return std::nullopt;
    }
    const auto &index = ast::as<ast::index_expr>(expr);
    const auto array_name = identifier_name(*index.object);
    if (!array_name.has_value() || !references_identifier(*index.index, counter))
    {
        return std::nullopt;
    }
    return array_name;
}

[[nodiscard]] inline bool primitive_array_type(const resolved_type &type)
{
    return type.tag == resolved_type::kind::Array && !type.elements.empty() &&
           type.elements.front().is_simd_lane_primitive();
}

[[nodiscard]] inline std::optional<simd_loop_match>
match_accumulation_body(const ast::assignment_stmt &assign, std::string_view counter,
                        const type_annotation_map &types)
{
    if (assign.op != ast::assign_op::Assign || !assign.value)
    {
        return std::nullopt;
    }
    const auto out_var = assignment_target_name(assign);
    if (!out_var.has_value() || assign.value->type != ast::expr_type::Binary)
    {
        return std::nullopt;
    }
    const auto &concat = ast::as<ast::binary_expr>(*assign.value);
    if (concat.op != ast::binary_op::Add ||
        !references_identifier(*concat.left, *out_var))
    {
        return std::nullopt;
    }

    const ast::expr_node *element_expr = nullptr;
    if (concat.right->type == ast::expr_type::ArrayLiteral)
    {
        const auto &literal = ast::as<ast::array_literal>(*concat.right);
        if (literal.elements.size() != 1)
        {
            return std::nullopt;
        }
        element_expr = literal.elements.front().get();
    }
    else if (concat.right->type == ast::expr_type::TypedArrayLiteral)
    {
        const auto &literal = ast::as<ast::typed_array_literal>(*concat.right);
        if (literal.elements.size() != 1)
        {
            return std::nullopt;
        }
        element_expr = literal.elements.front().get();
    }
    else
    {
        return std::nullopt;
    }

    if (element_expr->type != ast::expr_type::Binary)
    {
        return std::nullopt;
    }
    const auto &arith = ast::as<ast::binary_expr>(*element_expr);
    if (arith.op != ast::binary_op::Add && arith.op != ast::binary_op::Sub &&
        arith.op != ast::binary_op::Mul)
    {
        return std::nullopt;
    }

    const auto left_array = parse_index_array(*arith.left, counter);
    const auto right_array = parse_index_array(*arith.right, counter);
    if (!left_array.has_value() || !right_array.has_value())
    {
        return std::nullopt;
    }

    const auto &left_index = ast::as<ast::index_expr>(*arith.left);
    const auto &right_index = ast::as<ast::index_expr>(*arith.right);
    const resolved_type left_type = types.lookup(*left_index.object);
    const resolved_type right_type = types.lookup(*right_index.object);
    if (!primitive_array_type(left_type) || !primitive_array_type(right_type) ||
        left_type.elements.front() != right_type.elements.front())
    {
        return std::nullopt;
    }

    simd_loop_match match{};
    match.out_var = *out_var;
    match.left_array = *left_array;
    match.right_array = *right_array;
    match.op = arith.op;
    match.counter = std::string{counter};
    return match;
}

} // namespace detail

/// Try to match `counter = 0; loop counter < arr.len { out = out + [a[i] OP b[i]]; i += 1 }`.
[[nodiscard]] inline std::optional<simd_loop_match>
match_simd_elementwise_loop(const ast::stmt_node &init_stmt, const ast::loop_stmt &loop,
                            const type_annotation_map &types)
{
    if (init_stmt.type != ast::stmt_type::Assignment || !loop.condition.has_value())
    {
        return std::nullopt;
    }
    const auto &init = ast::as_stmt<ast::assignment_stmt>(init_stmt);
    if (init.op != ast::assign_op::Assign)
    {
        return std::nullopt;
    }
    const auto counter = detail::assignment_target_name(init);
    const auto start = detail::int_literal_value(*init.value);
    if (!counter.has_value() || !start.has_value() || *start != 0)
    {
        return std::nullopt;
    }

    std::string len_array;
    if (!detail::match_len_bound(**loop.condition, *counter, &len_array))
    {
        return std::nullopt;
    }
    if (!detail::body_increments_counter(*loop.body, *counter))
    {
        return std::nullopt;
    }
    if (loop.body->statements.size() != 2)
    {
        return std::nullopt;
    }
    const ast::stmt_node &body_stmt = *loop.body->statements.front();
    if (body_stmt.type != ast::stmt_type::Assignment)
    {
        return std::nullopt;
    }
    const auto match = detail::match_accumulation_body(
        ast::as_stmt<ast::assignment_stmt>(body_stmt), *counter, types);
    if (!match.has_value())
    {
        return std::nullopt;
    }

    if (len_array != match->left_array && len_array != match->right_array)
    {
        return std::nullopt;
    }

    simd_loop_match result = *match;
    result.len_array = len_array;
    result.consumed_statements = 2;
    return result;
}

/// Match optional `out = []` init before counter init and loop.
[[nodiscard]] inline std::optional<simd_loop_match>
match_simd_elementwise_loop_sequence(
    const std::vector<std::unique_ptr<ast::stmt_node>> &statements, size_t index,
    const type_annotation_map &types)
{
    if (index + 1 >= statements.size())
    {
        return std::nullopt;
    }

    if (index + 2 < statements.size() &&
        detail::is_empty_array_init(*statements[index]) &&
        statements[index + 1]->type == ast::stmt_type::Assignment &&
        statements[index + 2]->type == ast::stmt_type::Loop)
    {
        const auto &empty_init = ast::as_stmt<ast::assignment_stmt>(*statements[index]);
        const auto out_name = detail::assignment_target_name(empty_init);
        const auto &loop = ast::as_stmt<ast::loop_stmt>(*statements[index + 2]);
        const auto matched =
            match_simd_elementwise_loop(*statements[index + 1], loop, types);
        if (matched.has_value() && out_name.has_value() &&
            matched->out_var == *out_name)
        {
            simd_loop_match result = *matched;
            result.consumed_statements = 3;
            return result;
        }
    }

    if (statements[index]->type != ast::stmt_type::Assignment ||
        statements[index + 1]->type != ast::stmt_type::Loop)
    {
        return std::nullopt;
    }
    const auto &loop = ast::as_stmt<ast::loop_stmt>(*statements[index + 1]);
    return match_simd_elementwise_loop(*statements[index], loop, types);
}

} // namespace munx

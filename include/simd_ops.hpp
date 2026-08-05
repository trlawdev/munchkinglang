#pragma once

#include "vm_value.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include "ast.hpp"
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <immintrin.h>
#endif
#define MUNX_HAS_AVX2_INTRINSICS 1
#endif

namespace munx::vm
{

#if defined(MUNX_HAS_AVX2_INTRINSICS) && (defined(__GNUC__) || defined(__clang__))
#define MUNX_AVX2_TARGET __attribute__((target("avx2")))
#elif defined(MUNX_HAS_AVX2_INTRINSICS) && defined(_MSC_VER)
#define MUNX_AVX2_TARGET __declspec(target("avx2"))
#else
#define MUNX_AVX2_TARGET
#endif

/// @return True when the host CPU supports AVX2.
inline bool cpu_has_avx2() noexcept
{
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
    return __builtin_cpu_supports("avx2");
#elif defined(_MSC_VER)
    int info[4]{};
    __cpuid(info, 0);
    if (info[0] < 7)
    {
        return false;
    }
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
#else
    return false;
#endif
}

inline void require_avx2()
{
    if (!cpu_has_avx2())
    {
        throw_error("SIMD requires AVX2 support on this CPU");
    }
}

[[nodiscard]] inline simd_lane_kind lane_kind_of(const value &item)
{
    if (item.get_if<int64_t>() != nullptr)
    {
        return simd_lane_kind::Int;
    }
    if (item.get_if<double>() != nullptr)
    {
        return simd_lane_kind::Float;
    }
    if (item.get_if<char>() != nullptr)
    {
        return simd_lane_kind::Char;
    }
    if (item.get_if<bool>() != nullptr)
    {
        return simd_lane_kind::Bool;
    }
    throw_error("SIMD lanes must be int, float, char, or bool");
}

[[nodiscard]] inline int32_t lane_to_i32(const value &item, simd_lane_kind kind)
{
    switch (kind)
    {
    case simd_lane_kind::Int:
        return static_cast<int32_t>(as_integer(item));
    case simd_lane_kind::Char:
        return static_cast<int32_t>(static_cast<unsigned char>(*item.get_if<char>()));
    case simd_lane_kind::Bool:
        return *item.get_if<bool>() ? 1 : 0;
    default:
        throw_error("internal SIMD lane conversion error");
    }
}

[[nodiscard]] inline array_value repeat_array(const array_value &array, int64_t count)
{
    if (count < 0)
    {
        throw_error("array repeat count must be non-negative");
    }
    const value_vector &tile = array.data->items;
    auto repeated = std::make_shared<sequence_object>();
    if (count == 0 || tile.empty())
    {
        return array_value{repeated};
    }
    repeated->items.reserve(static_cast<size_t>(count) * tile.size());
    for (int64_t repeat = 0; repeat < count; ++repeat)
    {
        repeated->items.insert(repeated->items.end(), tile.begin(), tile.end());
    }
    return array_value{repeated};
}

[[nodiscard]] inline simd_value make_simd_from_array(const array_value &array)
{
    require_avx2();
    const value_vector &items = array.data->items;
    if (items.empty())
    {
        throw_error("cannot create SIMD vector from empty array");
    }

    const simd_lane_kind kind = lane_kind_of(items.front());
    simd_value out{};
    out.kind = kind;
    out.length = items.size();

    if (kind == simd_lane_kind::Float)
    {
        out.f32.resize(out.length);
        for (size_t index = 0; index < out.length; ++index)
        {
            if (lane_kind_of(items[index]) != kind)
            {
                throw_error("SIMD array elements must have the same primitive type");
            }
            out.f32[index] = static_cast<float>(as_number(items[index]));
        }
        return out;
    }

    out.i32.resize(out.length);
    for (size_t index = 0; index < out.length; ++index)
    {
        if (lane_kind_of(items[index]) != kind)
        {
            throw_error("SIMD array elements must have the same primitive type");
        }
        out.i32[index] = lane_to_i32(items[index], kind);
    }
    return out;
}

inline MUNX_AVX2_TARGET void mul_i32_lanes(const int32_t *left, const int32_t *right,
                                           int32_t *out, size_t length)
{
#if defined(MUNX_HAS_AVX2_INTRINSICS)
    size_t index = 0;
    constexpr size_t k_width = 8;
    for (; index + k_width <= length; index += k_width)
    {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(left + index));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(right + index));
        const __m256i product = _mm256_mullo_epi32(a, b);
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + index), product);
    }
    for (; index < length; ++index)
    {
        out[index] = left[index] * right[index];
    }
#else
    for (size_t index = 0; index < length; ++index)
    {
        out[index] = left[index] * right[index];
    }
#endif
}

inline MUNX_AVX2_TARGET void mul_f32_lanes(const float *left, const float *right, float *out,
                                           size_t length)
{
#if defined(MUNX_HAS_AVX2_INTRINSICS)
    size_t index = 0;
    constexpr size_t k_width = 8;
    for (; index + k_width <= length; index += k_width)
    {
        const __m256 a = _mm256_loadu_ps(left + index);
        const __m256 b = _mm256_loadu_ps(right + index);
        const __m256 product = _mm256_mul_ps(a, b);
        _mm256_storeu_ps(out + index, product);
    }
    for (; index < length; ++index)
    {
        out[index] = left[index] * right[index];
    }
#else
    for (size_t index = 0; index < length; ++index)
    {
        out[index] = left[index] * right[index];
    }
#endif
}

inline MUNX_AVX2_TARGET void add_i32_lanes(const int32_t *left, const int32_t *right,
                                           int32_t *out, size_t length)
{
#if defined(MUNX_HAS_AVX2_INTRINSICS)
    size_t index = 0;
    constexpr size_t k_width = 8;
    for (; index + k_width <= length; index += k_width)
    {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(left + index));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(right + index));
        const __m256i sum = _mm256_add_epi32(a, b);
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + index), sum);
    }
    for (; index < length; ++index)
    {
        out[index] = left[index] + right[index];
    }
#else
    for (size_t index = 0; index < length; ++index)
    {
        out[index] = left[index] + right[index];
    }
#endif
}

inline MUNX_AVX2_TARGET void sub_i32_lanes(const int32_t *left, const int32_t *right,
                                           int32_t *out, size_t length)
{
#if defined(MUNX_HAS_AVX2_INTRINSICS)
    size_t index = 0;
    constexpr size_t k_width = 8;
    for (; index + k_width <= length; index += k_width)
    {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(left + index));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(right + index));
        const __m256i diff = _mm256_sub_epi32(a, b);
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + index), diff);
    }
    for (; index < length; ++index)
    {
        out[index] = left[index] - right[index];
    }
#else
    for (size_t index = 0; index < length; ++index)
    {
        out[index] = left[index] - right[index];
    }
#endif
}

inline MUNX_AVX2_TARGET void add_f32_lanes(const float *left, const float *right, float *out,
                                           size_t length)
{
#if defined(MUNX_HAS_AVX2_INTRINSICS)
    size_t index = 0;
    constexpr size_t k_width = 8;
    for (; index + k_width <= length; index += k_width)
    {
        const __m256 a = _mm256_loadu_ps(left + index);
        const __m256 b = _mm256_loadu_ps(right + index);
        const __m256 sum = _mm256_add_ps(a, b);
        _mm256_storeu_ps(out + index, sum);
    }
    for (; index < length; ++index)
    {
        out[index] = left[index] + right[index];
    }
#else
    for (size_t index = 0; index < length; ++index)
    {
        out[index] = left[index] + right[index];
    }
#endif
}

inline MUNX_AVX2_TARGET void sub_f32_lanes(const float *left, const float *right, float *out,
                                           size_t length)
{
#if defined(MUNX_HAS_AVX2_INTRINSICS)
    size_t index = 0;
    constexpr size_t k_width = 8;
    for (; index + k_width <= length; index += k_width)
    {
        const __m256 a = _mm256_loadu_ps(left + index);
        const __m256 b = _mm256_loadu_ps(right + index);
        const __m256 diff = _mm256_sub_ps(a, b);
        _mm256_storeu_ps(out + index, diff);
    }
    for (; index < length; ++index)
    {
        out[index] = left[index] - right[index];
    }
#else
    for (size_t index = 0; index < length; ++index)
    {
        out[index] = left[index] - right[index];
    }
#endif
}

[[nodiscard]] inline simd_value simd_binary_op(const simd_value &left, const simd_value &right,
                                               ast::binary_op op)
{
    require_avx2();
    if (left.kind != right.kind)
    {
        throw_error("SIMD operands must have the same lane type");
    }
    if (left.length != right.length)
    {
        throw_error("SIMD operands must have the same length");
    }

    simd_value out{};
    out.kind = left.kind;
    out.length = left.length;

    if (left.kind == simd_lane_kind::Float)
    {
        out.f32.resize(out.length);
        switch (op)
        {
        case ast::binary_op::Add:
            add_f32_lanes(left.f32.data(), right.f32.data(), out.f32.data(), out.length);
            break;
        case ast::binary_op::Sub:
            sub_f32_lanes(left.f32.data(), right.f32.data(), out.f32.data(), out.length);
            break;
        case ast::binary_op::Mul:
            mul_f32_lanes(left.f32.data(), right.f32.data(), out.f32.data(), out.length);
            break;
        default:
            throw_error("unsupported SIMD arithmetic operator");
        }
        return out;
    }

    out.i32.resize(out.length);
    switch (op)
    {
    case ast::binary_op::Add:
        add_i32_lanes(left.i32.data(), right.i32.data(), out.i32.data(), out.length);
        break;
    case ast::binary_op::Sub:
        sub_i32_lanes(left.i32.data(), right.i32.data(), out.i32.data(), out.length);
        break;
    case ast::binary_op::Mul:
        mul_i32_lanes(left.i32.data(), right.i32.data(), out.i32.data(), out.length);
        break;
    default:
        throw_error("unsupported SIMD arithmetic operator");
    }
    return out;
}

[[nodiscard]] inline simd_value simd_mul(const simd_value &left, const simd_value &right)
{
    return simd_binary_op(left, right, ast::binary_op::Mul);
}

[[nodiscard]] inline value i32_lane_to_value(int32_t lane, simd_lane_kind kind)
{
    switch (kind)
    {
    case simd_lane_kind::Int:
        return value{static_cast<int64_t>(lane)};
    case simd_lane_kind::Char:
        return value{static_cast<char>(lane)};
    case simd_lane_kind::Bool:
        return value{lane != 0};
    default:
        throw_error("internal SIMD lane conversion error");
    }
}

[[nodiscard]] inline array_value simd_to_array(const simd_value &simd)
{
    auto array = std::make_shared<sequence_object>();
    array->items.reserve(simd.length);
    if (simd.kind == simd_lane_kind::Float)
    {
        for (size_t index = 0; index < simd.length; ++index)
        {
            array->items.push_back(value{static_cast<double>(simd.f32[index])});
        }
        return array_value{array};
    }
    for (size_t index = 0; index < simd.length; ++index)
    {
        array->items.push_back(i32_lane_to_value(simd.i32[index], simd.kind));
    }
    return array_value{array};
}

#undef MUNX_AVX2_TARGET

} // namespace munx::vm

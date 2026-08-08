#include "munx_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void munx_rt_abort(const char *msg)
{
    fprintf(stderr, "munx runtime: %s\n", msg ? msg : "abort");
    abort();
}

MunxValue munx_null(void)
{
    MunxValue v;
    v.tag = MUNX_TAG_NULL;
    v.as.i64 = 0;
    return v;
}

MunxValue munx_i64(int64_t x)
{
    MunxValue v;
    v.tag = MUNX_TAG_I64;
    v.as.i64 = x;
    return v;
}

MunxValue munx_f64(double x)
{
    MunxValue v;
    v.tag = MUNX_TAG_F64;
    v.as.f64 = x;
    return v;
}

MunxValue munx_bool(bool x)
{
    MunxValue v;
    v.tag = MUNX_TAG_BOOL;
    v.as.b = x;
    return v;
}

MunxValue munx_char(char x)
{
    MunxValue v;
    v.tag = MUNX_TAG_CHAR;
    v.as.c = x;
    return v;
}

MunxValue munx_string_n(const char *utf8, size_t n)
{
    char *copy = (char *)malloc(n + 1);
    if (!copy)
    {
        munx_rt_abort("out of memory");
    }
    memcpy(copy, utf8, n);
    copy[n] = '\0';
    MunxValue v;
    v.tag = MUNX_TAG_STRING;
    v.as.ptr = copy;
    return v;
}

MunxValue munx_string(const char *utf8)
{
    return munx_string_n(utf8, utf8 ? strlen(utf8) : 0);
}

static int is_num(MunxValue v)
{
    return v.tag == MUNX_TAG_I64 || v.tag == MUNX_TAG_F64 || v.tag == MUNX_TAG_BOOL ||
           v.tag == MUNX_TAG_CHAR;
}

static double as_f64(MunxValue v)
{
    switch (v.tag)
    {
    case MUNX_TAG_I64:
        return (double)v.as.i64;
    case MUNX_TAG_F64:
        return v.as.f64;
    case MUNX_TAG_BOOL:
        return v.as.b ? 1.0 : 0.0;
    case MUNX_TAG_CHAR:
        return (double)(unsigned char)v.as.c;
    default:
        munx_rt_abort("expected number");
        return 0.0;
    }
}

int64_t munx_as_i64(MunxValue v)
{
    switch (v.tag)
    {
    case MUNX_TAG_I64:
        return v.as.i64;
    case MUNX_TAG_F64:
        return (int64_t)v.as.f64;
    case MUNX_TAG_BOOL:
        return v.as.b ? 1 : 0;
    case MUNX_TAG_CHAR:
        return (int64_t)(unsigned char)v.as.c;
    default:
        munx_rt_abort("expected integer");
        return 0;
    }
}

bool munx_truthy(MunxValue v)
{
    switch (v.tag)
    {
    case MUNX_TAG_NULL:
        return false;
    case MUNX_TAG_BOOL:
        return v.as.b;
    case MUNX_TAG_I64:
        return v.as.i64 != 0;
    case MUNX_TAG_F64:
        return v.as.f64 != 0.0;
    case MUNX_TAG_CHAR:
        return v.as.c != '\0';
    case MUNX_TAG_STRING:
        return v.as.ptr != NULL && ((const char *)v.as.ptr)[0] != '\0';
    default:
        return true;
    }
}

MunxValue munx_add(MunxValue a, MunxValue b)
{
    if (a.tag == MUNX_TAG_STRING || b.tag == MUNX_TAG_STRING)
    {
        char buf_a[64];
        char buf_b[64];
        const char *sa;
        const char *sb;
        if (a.tag == MUNX_TAG_STRING)
        {
            sa = (const char *)a.as.ptr;
        }
        else if (a.tag == MUNX_TAG_I64)
        {
            snprintf(buf_a, sizeof buf_a, "%lld", (long long)a.as.i64);
            sa = buf_a;
        }
        else
        {
            munx_rt_abort("string concat type");
            sa = "";
        }
        if (b.tag == MUNX_TAG_STRING)
        {
            sb = (const char *)b.as.ptr;
        }
        else if (b.tag == MUNX_TAG_I64)
        {
            snprintf(buf_b, sizeof buf_b, "%lld", (long long)b.as.i64);
            sb = buf_b;
        }
        else
        {
            munx_rt_abort("string concat type");
            sb = "";
        }
        size_t na = strlen(sa);
        size_t nb = strlen(sb);
        char *out = (char *)malloc(na + nb + 1);
        if (!out)
        {
            munx_rt_abort("out of memory");
        }
        memcpy(out, sa, na);
        memcpy(out + na, sb, nb + 1);
        MunxValue v;
        v.tag = MUNX_TAG_STRING;
        v.as.ptr = out;
        return v;
    }
    if (a.tag == MUNX_TAG_F64 || b.tag == MUNX_TAG_F64)
    {
        return munx_f64(as_f64(a) + as_f64(b));
    }
    return munx_i64(munx_as_i64(a) + munx_as_i64(b));
}

MunxValue munx_sub(MunxValue a, MunxValue b)
{
    if (a.tag == MUNX_TAG_F64 || b.tag == MUNX_TAG_F64)
    {
        return munx_f64(as_f64(a) - as_f64(b));
    }
    return munx_i64(munx_as_i64(a) - munx_as_i64(b));
}

MunxValue munx_mul(MunxValue a, MunxValue b)
{
    if (a.tag == MUNX_TAG_F64 || b.tag == MUNX_TAG_F64)
    {
        return munx_f64(as_f64(a) * as_f64(b));
    }
    return munx_i64(munx_as_i64(a) * munx_as_i64(b));
}

MunxValue munx_div(MunxValue a, MunxValue b)
{
    if (a.tag == MUNX_TAG_F64 || b.tag == MUNX_TAG_F64)
    {
        double d = as_f64(b);
        if (d == 0.0)
        {
            munx_rt_abort("division by zero");
        }
        return munx_f64(as_f64(a) / d);
    }
    int64_t d = munx_as_i64(b);
    if (d == 0)
    {
        munx_rt_abort("division by zero");
    }
    return munx_i64(munx_as_i64(a) / d);
}

MunxValue munx_mod(MunxValue a, MunxValue b)
{
    int64_t d = munx_as_i64(b);
    if (d == 0)
    {
        munx_rt_abort("modulo by zero");
    }
    return munx_i64(munx_as_i64(a) % d);
}

MunxValue munx_neg(MunxValue a)
{
    if (a.tag == MUNX_TAG_F64)
    {
        return munx_f64(-a.as.f64);
    }
    return munx_i64(-munx_as_i64(a));
}

MunxValue munx_not(MunxValue a) { return munx_bool(!munx_truthy(a)); }

MunxValue munx_eq(MunxValue a, MunxValue b)
{
    if (a.tag == MUNX_TAG_STRING && b.tag == MUNX_TAG_STRING)
    {
        return munx_bool(strcmp((const char *)a.as.ptr, (const char *)b.as.ptr) == 0);
    }
    if (is_num(a) && is_num(b))
    {
        if (a.tag == MUNX_TAG_F64 || b.tag == MUNX_TAG_F64)
        {
            return munx_bool(as_f64(a) == as_f64(b));
        }
        return munx_bool(munx_as_i64(a) == munx_as_i64(b));
    }
    return munx_bool(a.tag == MUNX_TAG_NULL && b.tag == MUNX_TAG_NULL);
}

MunxValue munx_ne(MunxValue a, MunxValue b)
{
    return munx_bool(!munx_truthy(munx_eq(a, b)));
}

MunxValue munx_lt(MunxValue a, MunxValue b)
{
    if (a.tag == MUNX_TAG_F64 || b.tag == MUNX_TAG_F64)
    {
        return munx_bool(as_f64(a) < as_f64(b));
    }
    return munx_bool(munx_as_i64(a) < munx_as_i64(b));
}

MunxValue munx_gt(MunxValue a, MunxValue b)
{
    if (a.tag == MUNX_TAG_F64 || b.tag == MUNX_TAG_F64)
    {
        return munx_bool(as_f64(a) > as_f64(b));
    }
    return munx_bool(munx_as_i64(a) > munx_as_i64(b));
}

MunxValue munx_le(MunxValue a, MunxValue b)
{
    if (a.tag == MUNX_TAG_F64 || b.tag == MUNX_TAG_F64)
    {
        return munx_bool(as_f64(a) <= as_f64(b));
    }
    return munx_bool(munx_as_i64(a) <= munx_as_i64(b));
}

MunxValue munx_ge(MunxValue a, MunxValue b)
{
    if (a.tag == MUNX_TAG_F64 || b.tag == MUNX_TAG_F64)
    {
        return munx_bool(as_f64(a) >= as_f64(b));
    }
    return munx_bool(munx_as_i64(a) >= munx_as_i64(b));
}

static int g_argc = 0;
static char **g_argv = NULL;

void munx_set_argv(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
}

MunxValue munx_argv_len(void) { return munx_i64((int64_t)g_argc); }

MunxValue munx_argv_get(MunxValue index)
{
    int64_t i = munx_as_i64(index);
    if (i < 0 || i >= g_argc)
    {
        munx_rt_abort("argv index out of range");
    }
    return munx_string(g_argv[i]);
}

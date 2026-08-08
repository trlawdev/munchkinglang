#include "munx_rt.h"

#include <stdio.h>

void munx_print_cstr(const char *s)
{
    fputs(s ? s : "", stdout);
}

void munx_print(MunxValue v)
{
    switch (v.tag)
    {
    case MUNX_TAG_NULL:
        fputs("null", stdout);
        break;
    case MUNX_TAG_I64:
        printf("%lld", (long long)v.as.i64);
        break;
    case MUNX_TAG_F64:
        printf("%.15g", v.as.f64);
        break;
    case MUNX_TAG_BOOL:
        fputs(v.as.b ? "true" : "false", stdout);
        break;
    case MUNX_TAG_CHAR:
        fputc(v.as.c, stdout);
        break;
    case MUNX_TAG_STRING:
        fputs(v.as.ptr ? (const char *)v.as.ptr : "", stdout);
        break;
    default:
        fputs("<value>", stdout);
        break;
    }
}

void munx_println(void) { fputc('\n', stdout); }

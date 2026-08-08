#ifndef MUNX_RT_H
#define MUNX_RT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MunxTag {
    MUNX_TAG_NULL = 0,
    MUNX_TAG_I64,
    MUNX_TAG_F64,
    MUNX_TAG_BOOL,
    MUNX_TAG_CHAR,
    MUNX_TAG_STRING,
    MUNX_TAG_ARRAY,
    MUNX_TAG_PIPE = 8
} MunxTag;

typedef struct MunxValue {
    uint8_t tag;
    union {
        int64_t i64;
        double f64;
        bool b;
        char c;
        void *ptr;
    } as;
} MunxValue;

MunxValue munx_null(void);
MunxValue munx_i64(int64_t v);
MunxValue munx_f64(double v);
MunxValue munx_bool(bool v);
MunxValue munx_char(char v);
MunxValue munx_string(const char *utf8);
MunxValue munx_string_n(const char *utf8, size_t n);

MunxValue munx_add(MunxValue a, MunxValue b);
MunxValue munx_sub(MunxValue a, MunxValue b);
MunxValue munx_mul(MunxValue a, MunxValue b);
MunxValue munx_div(MunxValue a, MunxValue b);
MunxValue munx_mod(MunxValue a, MunxValue b);
MunxValue munx_neg(MunxValue a);
MunxValue munx_not(MunxValue a);

MunxValue munx_eq(MunxValue a, MunxValue b);
MunxValue munx_ne(MunxValue a, MunxValue b);
MunxValue munx_lt(MunxValue a, MunxValue b);
MunxValue munx_gt(MunxValue a, MunxValue b);
MunxValue munx_le(MunxValue a, MunxValue b);
MunxValue munx_ge(MunxValue a, MunxValue b);

bool munx_truthy(MunxValue v);
int64_t munx_as_i64(MunxValue v);

void munx_print(MunxValue v);
void munx_println(void);
void munx_print_cstr(const char *s);

void munx_set_argv(int argc, char **argv);
MunxValue munx_argv_len(void);
MunxValue munx_argv_get(MunxValue index);

void munx_rt_abort(const char *msg);

/* Pipe / string helpers (implemented in munx_pipe.c). */
MunxValue munx_pipe_open(MunxValue channel, MunxValue mode);
MunxValue munx_channel_open(MunxValue channel_id);
void munx_channel_insert(MunxValue pipe, MunxValue value);
MunxValue munx_channel_extract(MunxValue pipe);
void munx_pipe_close(MunxValue pipe);
void munx_pipe_insert(MunxValue pipe, MunxValue value);
MunxValue munx_pipe_extract(MunxValue pipe);
MunxValue munx_pipe_mode_out(void);
MunxValue munx_pipe_mode_in(void);
MunxValue munx_pipe_mode_subscribe(void);
void munx_pipe_session_begin(void);
void munx_pipe_session_end(void);
MunxValue munx_concat(MunxValue a, MunxValue b);
MunxValue munx_to_string(MunxValue v);
void munx_fail(MunxValue message);
void munx_sleep(MunxValue milliseconds);

#ifdef __cplusplus
}
#endif

#endif /* MUNX_RT_H */

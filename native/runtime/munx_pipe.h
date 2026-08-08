#ifndef MUNX_PIPE_H
#define MUNX_PIPE_H

#include "munx_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Attachment modes — match munx::vm::pipe_hub::attachment_mode. */
enum MunxPipeMode {
    MUNX_PIPE_WRITER = 0,
    MUNX_PIPE_QUEUE_IN = 1,
    MUNX_PIPE_BROADCAST_IN = 2,
    MUNX_PIPE_CHANNEL = 3
};

/* Wire tags for cross-process values (must match vm_pipe.hpp). */
enum MunxWireTag {
    MUNX_WIRE_NULL = 0,
    MUNX_WIRE_BOOL = 1,
    MUNX_WIRE_INT = 2,
    MUNX_WIRE_FLOAT = 3,
    MUNX_WIRE_CHAR = 4,
    MUNX_WIRE_STRING = 5
};

/* Ensure hub session (connect + Hello as Native). Idempotent. */
void munx_pipe_session_begin(void);
void munx_pipe_session_end(void);

/*
 * Open a named pipe channel via the hub.
 * mode_name: "out" | "in" | "subscribe" (as MunxValue string or identifier-encoded).
 * Returns a MunxValue with tag MUNX_TAG_PIPE.
 */
MunxValue munx_pipe_open(MunxValue channel, MunxValue mode);

/* Bidirectional channel peer (`channel(id)` / `:=>` / `<=:`). */
MunxValue munx_channel_open(MunxValue channel_id);
void munx_channel_insert(MunxValue pipe, MunxValue value);
MunxValue munx_channel_extract(MunxValue pipe);

void munx_pipe_close(MunxValue pipe);
void munx_pipe_insert(MunxValue pipe, MunxValue value);
MunxValue munx_pipe_extract(MunxValue pipe);

/* Helpers used by native codegen for sample compatibility. */
MunxValue munx_concat(MunxValue a, MunxValue b);
MunxValue munx_to_string(MunxValue v);
void munx_fail(MunxValue message);

/* Mode helpers for MIR (i64 mode codes). */
MunxValue munx_pipe_mode_out(void);
MunxValue munx_pipe_mode_in(void);
MunxValue munx_pipe_mode_subscribe(void);

#ifdef __cplusplus
}
#endif

#endif /* MUNX_PIPE_H */

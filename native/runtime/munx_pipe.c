#define _POSIX_C_SOURCE 200809L

#include "munx_pipe.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <time.h>

extern char **environ;

#ifndef MUNX_TAG_PIPE
#define MUNX_TAG_PIPE 8
#endif

static char *xstrdup(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *p = (char *)malloc(n + 1);
    if (!p)
    {
        munx_rt_abort("out of memory");
    }
    if (n)
    {
        memcpy(p, s, n);
    }
    p[n] = '\0';
    return p;
}

enum {
    OP_HELLO = 1,
    OP_BYE = 2,
    OP_ATTACH = 3,
    OP_DETACH = 4,
    OP_PUBLISH = 5,
    OP_DELIVER = 6,
    OP_ACK = 7,
    OP_ERROR = 8,
    OP_CHANNEL_OFFER = 9,
    OP_CHANNEL_ACCEPT = 10,
    OP_CHANNEL_BUSY = 11,
    OP_CHANNEL_RECV_READY = 12,
    OP_CHANNEL_DATA_ACK = 13,
    CLIENT_NATIVE = 2
};

typedef struct MunxPipeHandle {
    char *channel;
    int mode;
    int refs;
} MunxPipeHandle;

typedef struct DeliveryItem {
    uint8_t *data;
    size_t len;
    struct DeliveryItem *next;
} DeliveryItem;

typedef struct DeliveryQueue {
    char *channel;
    int mode;
    DeliveryItem *head;
    DeliveryItem *tail;
    int closed;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct DeliveryQueue *next;
} DeliveryQueue;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_fd = -1;
static int g_connected = 0;
static int g_shutdown = 0;
static pthread_t g_reader;
static int g_reader_alive = 0;
static DeliveryQueue *g_queues = NULL;
static pthread_mutex_t g_ack_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ack_cv = PTHREAD_COND_INITIALIZER;
static int g_waiting_ack = 0;
static int g_ack_ok = 0;
static char g_ack_err[256];
static int g_waiting_channel = 0;
static int g_channel_got = 0;
static int g_channel_op = 0;

static int hub_enabled(void)
{
    const char *e = getenv("MUNX_PIPE_HUB");
    if (!e || e[0] == '\0')
    {
        return 1;
    }
    return !(e[0] == '0' && e[1] == '\0');
}

static void pipe_dir(char *buf, size_t n)
{
    const char *e = getenv("MUNX_PIPE_DIR");
    if (e && e[0])
    {
        snprintf(buf, n, "%s", e);
        return;
    }
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0])
    {
        tmp = "/tmp";
    }
    snprintf(buf, n, "%s/munx-pipes", tmp);
}

static int write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (n == 0)
        {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *data, size_t len)
{
    uint8_t *p = (uint8_t *)data;
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = read(fd, p + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (n == 0)
        {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static uint8_t *frame_alloc(const uint8_t *body, uint32_t body_len, size_t *out_len)
{
    size_t n = 4 + body_len;
    uint8_t *f = (uint8_t *)malloc(n);
    if (!f)
    {
        return NULL;
    }
    f[0] = (uint8_t)(body_len & 0xff);
    f[1] = (uint8_t)((body_len >> 8) & 0xff);
    f[2] = (uint8_t)((body_len >> 16) & 0xff);
    f[3] = (uint8_t)((body_len >> 24) & 0xff);
    memcpy(f + 4, body, body_len);
    *out_len = n;
    return f;
}

static int send_frame(const uint8_t *body, uint32_t body_len)
{
    size_t n = 0;
    uint8_t *f = frame_alloc(body, body_len, &n);
    if (!f)
    {
        return -1;
    }
    int rc = write_all(g_fd, f, n);
    free(f);
    return rc;
}

static void append_u8(uint8_t **buf, size_t *len, size_t *cap, uint8_t v)
{
    if (*len + 1 > *cap)
    {
        *cap = *cap ? *cap * 2 : 64;
        *buf = (uint8_t *)realloc(*buf, *cap);
    }
    (*buf)[(*len)++] = v;
}

static void append_u32(uint8_t **buf, size_t *len, size_t *cap, uint32_t v)
{
    append_u8(buf, len, cap, (uint8_t)(v & 0xff));
    append_u8(buf, len, cap, (uint8_t)((v >> 8) & 0xff));
    append_u8(buf, len, cap, (uint8_t)((v >> 16) & 0xff));
    append_u8(buf, len, cap, (uint8_t)((v >> 24) & 0xff));
}

static void append_u64(uint8_t **buf, size_t *len, size_t *cap, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
    {
        append_u8(buf, len, cap, (uint8_t)((v >> (8 * i)) & 0xff));
    }
}

static void append_bytes(uint8_t **buf, size_t *len, size_t *cap, const void *p,
                         size_t n)
{
    append_u32(buf, len, cap, (uint32_t)n);
    for (size_t i = 0; i < n; ++i)
    {
        append_u8(buf, len, cap, ((const uint8_t *)p)[i]);
    }
}

static void append_str(uint8_t **buf, size_t *len, size_t *cap, const char *s)
{
    append_bytes(buf, len, cap, s, s ? strlen(s) : 0);
}

static DeliveryQueue *find_queue(const char *channel, int mode)
{
    for (DeliveryQueue *q = g_queues; q; q = q->next)
    {
        if (q->mode == mode && strcmp(q->channel, channel) == 0)
        {
            return q;
        }
    }
    return NULL;
}

static DeliveryQueue *ensure_queue(const char *channel, int mode)
{
    DeliveryQueue *q = find_queue(channel, mode);
    if (q)
    {
        return q;
    }
    q = (DeliveryQueue *)calloc(1, sizeof(DeliveryQueue));
    q->channel = xstrdup(channel);
    q->mode = mode;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    q->next = g_queues;
    g_queues = q;
    return q;
}

static void enqueue_delivery(const char *channel, int mode, const uint8_t *data,
                             size_t len)
{
    DeliveryQueue *q = find_queue(channel, mode);
    if (!q)
    {
        return;
    }
    DeliveryItem *item = (DeliveryItem *)calloc(1, sizeof(DeliveryItem));
    item->data = (uint8_t *)malloc(len);
    memcpy(item->data, data, len);
    item->len = len;
    pthread_mutex_lock(&q->mu);
    if (q->tail)
    {
        q->tail->next = item;
    }
    else
    {
        q->head = item;
    }
    q->tail = item;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void handle_body(const uint8_t *body, uint32_t len)
{
    if (len < 1)
    {
        return;
    }
    uint8_t op = body[0];
    if (op == OP_ACK)
    {
        pthread_mutex_lock(&g_ack_mu);
        g_ack_ok = 1;
        g_ack_err[0] = 0;
        pthread_cond_signal(&g_ack_cv);
        pthread_mutex_unlock(&g_ack_mu);
        return;
    }
    if (op == OP_CHANNEL_ACCEPT || op == OP_CHANNEL_BUSY)
    {
        pthread_mutex_lock(&g_ack_mu);
        if (g_waiting_channel)
        {
            g_channel_got = 1;
            g_channel_op = (int)op;
            g_ack_err[0] = 0;
            pthread_cond_signal(&g_ack_cv);
        }
        pthread_mutex_unlock(&g_ack_mu);
        return;
    }
    if (op == OP_ERROR && len >= 5)
    {
        uint32_t n = read_u32_le(body + 1);
        pthread_mutex_lock(&g_ack_mu);
        g_ack_ok = 0;
        if (n > sizeof(g_ack_err) - 1)
        {
            n = sizeof(g_ack_err) - 1;
        }
        memcpy(g_ack_err, body + 5, n);
        g_ack_err[n] = 0;
        if (g_waiting_channel)
        {
            g_channel_got = 1;
            g_channel_op = OP_ERROR;
        }
        pthread_cond_signal(&g_ack_cv);
        pthread_mutex_unlock(&g_ack_mu);
        return;
    }
    if (op == OP_DELIVER)
    {
        size_t off = 1;
        if (off + 4 > len)
        {
            return;
        }
        uint32_t clen = read_u32_le(body + off);
        off += 4;
        if (off + clen + 1 + 4 > len)
        {
            return;
        }
        char channel[512];
        if (clen >= sizeof(channel))
        {
            clen = sizeof(channel) - 1;
        }
        memcpy(channel, body + off, clen);
        channel[clen] = 0;
        off += clen;
        int mode = body[off++];
        uint32_t plen = read_u32_le(body + off);
        off += 4;
        if (off + plen > len)
        {
            return;
        }
        pthread_mutex_lock(&g_mu);
        ensure_queue(channel, mode);
        pthread_mutex_unlock(&g_mu);
        enqueue_delivery(channel, mode, body + off, plen);
    }
}

static void *reader_main(void *arg)
{
    (void)arg;
    uint8_t header[4];
    while (!g_shutdown)
    {
        struct pollfd pfd;
        pfd.fd = g_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 100);
        if (g_shutdown)
        {
            break;
        }
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (pr == 0)
        {
            continue;
        }
        if (read_all(g_fd, header, 4) != 0)
        {
            break;
        }
        uint32_t blen = read_u32_le(header);
        uint8_t *body = (uint8_t *)malloc(blen ? blen : 1);
        if (!body)
        {
            break;
        }
        if (blen && read_all(g_fd, body, blen) != 0)
        {
            free(body);
            break;
        }
        handle_body(body, blen);
        free(body);
    }
    pthread_mutex_lock(&g_mu);
    for (DeliveryQueue *q = g_queues; q; q = q->next)
    {
        pthread_mutex_lock(&q->mu);
        q->closed = 1;
        pthread_cond_broadcast(&q->cv);
        pthread_mutex_unlock(&q->mu);
    }
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_ack_mu);
    g_ack_ok = 0;
    snprintf(g_ack_err, sizeof(g_ack_err), "hub disconnected");
    if (g_waiting_channel)
    {
        g_channel_got = 1;
        g_channel_op = OP_ERROR;
    }
    pthread_cond_broadcast(&g_ack_cv);
    pthread_mutex_unlock(&g_ack_mu);
    return NULL;
}

static int try_connect(void)
{
    char dir[512];
    char sock[576];
    pipe_dir(dir, sizeof(dir));
    snprintf(sock, sizeof(sock), "%s/hub.sock", dir);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock) >= sizeof(addr.sun_path))
    {
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }
    g_fd = fd;
    return 0;
}

static int spawn_hub(void)
{
    const char *exe = getenv("MUNX_HUB_EXECUTABLE");
    if (!exe || !exe[0])
    {
        exe = getenv("MUNX_PIPE_HUB_BIN");
    }
    if (!exe || !exe[0])
    {
        exe = "munxc";
    }
    char *argv[] = {(char *)exe, (char *)"--pipe-hub", NULL};
    pid_t pid = 0;
    return posix_spawn(&pid, exe, NULL, NULL, argv, environ) == 0 ? 0 : -1;
}

static void send_hello_native(void)
{
    uint8_t *body = NULL;
    size_t len = 0, cap = 0;
    append_u8(&body, &len, &cap, OP_HELLO);
    append_u64(&body, &len, &cap, (uint64_t)getpid());
    append_u8(&body, &len, &cap, CLIENT_NATIVE);
    send_frame(body, (uint32_t)len);
    free(body);
}

void munx_pipe_session_begin(void)
{
    if (!hub_enabled())
    {
        return;
    }
    pthread_mutex_lock(&g_mu);
    if (g_connected)
    {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    char dir[512];
    pipe_dir(dir, sizeof(dir));
    mkdir(dir, 0755);

    for (int i = 0; i < 40; ++i)
    {
        if (try_connect() == 0)
        {
            goto connected;
        }
        struct timespec ts = {0, 25 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    spawn_hub();
    for (int i = 0; i < 200; ++i)
    {
        if (try_connect() == 0)
        {
            goto connected;
        }
        struct timespec ts = {0, 25 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    pthread_mutex_unlock(&g_mu);
    munx_rt_abort("native: could not connect to pipe hub "
                  "(set MUNX_HUB_EXECUTABLE to munxc)");

connected:
    g_shutdown = 0;
    g_connected = 1;
    send_hello_native();
    if (pthread_create(&g_reader, NULL, reader_main, NULL) == 0)
    {
        g_reader_alive = 1;
    }
    pthread_mutex_unlock(&g_mu);
}

void munx_pipe_session_end(void)
{
    pthread_mutex_lock(&g_mu);
    if (!g_connected)
    {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    uint8_t bye = OP_BYE;
    send_frame(&bye, 1);
    g_shutdown = 1;
    if (g_fd >= 0)
    {
        shutdown(g_fd, SHUT_RDWR);
        close(g_fd);
        g_fd = -1;
    }
    g_connected = 0;
    pthread_t reader = g_reader;
    int alive = g_reader_alive;
    g_reader_alive = 0;
    pthread_mutex_unlock(&g_mu);
    if (alive)
    {
        pthread_join(reader, NULL);
    }
}

static const char *value_cstr(MunxValue v)
{
    if (v.tag == MUNX_TAG_STRING && v.as.ptr)
    {
        return (const char *)v.as.ptr;
    }
    return "";
}

static int mode_from_value(MunxValue mode)
{
    if (mode.tag == MUNX_TAG_I64)
    {
        return (int)mode.as.i64;
    }
    if (mode.tag == MUNX_TAG_STRING)
    {
        const char *s = value_cstr(mode);
        if (strcmp(s, "out") == 0)
        {
            return MUNX_PIPE_WRITER;
        }
        if (strcmp(s, "in") == 0)
        {
            return MUNX_PIPE_QUEUE_IN;
        }
        if (strcmp(s, "subscribe") == 0)
        {
            return MUNX_PIPE_BROADCAST_IN;
        }
    }
    munx_rt_abort("pipe mode must be out, in, or subscribe");
    return 0;
}

void munx_sleep(MunxValue milliseconds)
{
    int64_t ms = munx_as_i64(milliseconds);
    if (ms < 0)
    {
        ms = 0;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    {
    }
}

MunxValue munx_pipe_mode_out(void) { return munx_i64(MUNX_PIPE_WRITER); }
MunxValue munx_pipe_mode_in(void) { return munx_i64(MUNX_PIPE_QUEUE_IN); }
MunxValue munx_pipe_mode_subscribe(void)
{
    return munx_i64(MUNX_PIPE_BROADCAST_IN);
}

static void wait_hub_ack(const char *what)
{
    pthread_mutex_lock(&g_ack_mu);
    while (g_waiting_ack && g_ack_err[0] == 0 && !g_ack_ok)
    {
        pthread_cond_wait(&g_ack_cv, &g_ack_mu);
        if (g_ack_ok || g_ack_err[0])
        {
            break;
        }
    }
    g_waiting_ack = 0;
    if (!g_ack_ok)
    {
        char msg[320];
        snprintf(msg, sizeof(msg), "%s: %s", what,
                 g_ack_err[0] ? g_ack_err : "rejected");
        pthread_mutex_unlock(&g_ack_mu);
        munx_rt_abort(msg);
    }
    pthread_mutex_unlock(&g_ack_mu);
}

/* Returns OP_CHANNEL_ACCEPT or OP_CHANNEL_BUSY; aborts on Error/disconnect. */
static int wait_channel_reply(void)
{
    pthread_mutex_lock(&g_ack_mu);
    while (!g_channel_got && g_ack_err[0] == 0)
    {
        pthread_cond_wait(&g_ack_cv, &g_ack_mu);
    }
    g_waiting_channel = 0;
    if (g_channel_op == OP_ERROR || g_ack_err[0])
    {
        char msg[320];
        snprintf(msg, sizeof(msg), "channel handshake error: %s",
                 g_ack_err[0] ? g_ack_err : "rejected");
        pthread_mutex_unlock(&g_ack_mu);
        munx_rt_abort(msg);
    }
    int op = g_channel_op;
    g_channel_got = 0;
    pthread_mutex_unlock(&g_ack_mu);
    return op;
}

static void channel_backoff(void)
{
    static unsigned seed = 0;
    if (seed == 0)
    {
        seed = (unsigned)getpid() ^ (unsigned)time(NULL);
    }
    seed = seed * 1103515245u + 12345u;
    int ms = 1 + (int)(seed % 10u);
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = (long)ms * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    {
    }
}

static MunxValue attach_handle(const char *name, int mode)
{
    pthread_mutex_lock(&g_mu);
    if (mode == MUNX_PIPE_QUEUE_IN || mode == MUNX_PIPE_BROADCAST_IN ||
        mode == MUNX_PIPE_CHANNEL)
    {
        ensure_queue(name, mode);
    }
    uint8_t *body = NULL;
    size_t len = 0, cap = 0;
    append_u8(&body, &len, &cap, OP_ATTACH);
    append_str(&body, &len, &cap, name);
    append_u8(&body, &len, &cap, (uint8_t)mode);
    pthread_mutex_lock(&g_ack_mu);
    g_waiting_ack = 1;
    g_ack_ok = 0;
    g_ack_err[0] = 0;
    pthread_mutex_unlock(&g_ack_mu);
    if (send_frame(body, (uint32_t)len) != 0)
    {
        free(body);
        pthread_mutex_unlock(&g_mu);
        munx_rt_abort("pipe hub attach failed");
    }
    free(body);
    pthread_mutex_unlock(&g_mu);
    wait_hub_ack("pipe hub attach error");

    MunxPipeHandle *h = (MunxPipeHandle *)calloc(1, sizeof(MunxPipeHandle));
    h->channel = xstrdup(name);
    h->mode = mode;
    h->refs = 1;
    MunxValue v;
    v.tag = MUNX_TAG_PIPE;
    v.as.ptr = h;
    return v;
}

MunxValue munx_pipe_open(MunxValue channel, MunxValue mode)
{
    munx_pipe_session_begin();
    const char *name = value_cstr(channel);
    if (channel.tag != MUNX_TAG_STRING || !name[0])
    {
        munx_rt_abort("pipe channel must be a non-empty string");
    }
    return attach_handle(name, mode_from_value(mode));
}

MunxValue munx_channel_open(MunxValue channel_id)
{
    munx_pipe_session_begin();
    const char *name = value_cstr(channel_id);
    if (channel_id.tag != MUNX_TAG_STRING || !name[0])
    {
        munx_rt_abort("channel id must be a non-empty string");
    }
    return attach_handle(name, MUNX_PIPE_CHANNEL);
}

void munx_pipe_close(MunxValue pipe)
{
    if (pipe.tag != MUNX_TAG_PIPE || !pipe.as.ptr)
    {
        return;
    }
    MunxPipeHandle *h = (MunxPipeHandle *)pipe.as.ptr;
    pthread_mutex_lock(&g_mu);
    if (g_connected)
    {
        uint8_t *body = NULL;
        size_t len = 0, cap = 0;
        append_u8(&body, &len, &cap, OP_DETACH);
        append_str(&body, &len, &cap, h->channel);
        append_u8(&body, &len, &cap, (uint8_t)h->mode);
        send_frame(body, (uint32_t)len);
        free(body);
    }
    pthread_mutex_unlock(&g_mu);
    free(h->channel);
    free(h);
}

static uint8_t *encode_value(MunxValue v, size_t *out_len)
{
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    switch (v.tag)
    {
    case MUNX_TAG_NULL:
        append_u8(&buf, &len, &cap, MUNX_WIRE_NULL);
        break;
    case MUNX_TAG_BOOL:
        append_u8(&buf, &len, &cap, MUNX_WIRE_BOOL);
        append_u8(&buf, &len, &cap, v.as.b ? 1 : 0);
        break;
    case MUNX_TAG_I64:
        append_u8(&buf, &len, &cap, MUNX_WIRE_INT);
        append_u64(&buf, &len, &cap, (uint64_t)v.as.i64);
        break;
    case MUNX_TAG_F64:
    {
        uint64_t bits = 0;
        memcpy(&bits, &v.as.f64, sizeof(bits));
        append_u8(&buf, &len, &cap, MUNX_WIRE_FLOAT);
        append_u64(&buf, &len, &cap, bits);
        break;
    }
    case MUNX_TAG_CHAR:
        append_u8(&buf, &len, &cap, MUNX_WIRE_CHAR);
        append_u8(&buf, &len, &cap, (uint8_t)v.as.c);
        break;
    case MUNX_TAG_STRING:
        append_u8(&buf, &len, &cap, MUNX_WIRE_STRING);
        append_str(&buf, &len, &cap, value_cstr(v));
        break;
    default:
        munx_rt_abort("unsupported value for pipe publish");
    }
    *out_len = len;
    return buf;
}

static MunxValue decode_value(const uint8_t *p, size_t len)
{
    if (len < 1)
    {
        return munx_null();
    }
    switch (p[0])
    {
    case MUNX_WIRE_NULL:
        return munx_null();
    case MUNX_WIRE_BOOL:
        return munx_bool(len > 1 && p[1] != 0);
    case MUNX_WIRE_INT:
    {
        if (len < 9)
        {
            return munx_null();
        }
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i)
        {
            bits |= (uint64_t)p[1 + i] << (8 * i);
        }
        return munx_i64((int64_t)bits);
    }
    case MUNX_WIRE_FLOAT:
    {
        if (len < 9)
        {
            return munx_null();
        }
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i)
        {
            bits |= (uint64_t)p[1 + i] << (8 * i);
        }
        double d = 0;
        memcpy(&d, &bits, sizeof(d));
        return munx_f64(d);
    }
    case MUNX_WIRE_CHAR:
        return munx_char(len > 1 ? (char)p[1] : 0);
    case MUNX_WIRE_STRING:
    {
        if (len < 5)
        {
            return munx_string("");
        }
        uint32_t n = read_u32_le(p + 1);
        if (5 + n > len)
        {
            n = (uint32_t)(len - 5);
        }
        return munx_string_n((const char *)(p + 5), n);
    }
    default:
        return munx_null();
    }
}

void munx_pipe_insert(MunxValue pipe, MunxValue value)
{
    if (pipe.tag != MUNX_TAG_PIPE || !pipe.as.ptr)
    {
        munx_rt_abort("pipe insert on non-pipe");
    }
    MunxPipeHandle *h = (MunxPipeHandle *)pipe.as.ptr;
    if (h->mode != MUNX_PIPE_WRITER)
    {
        munx_rt_abort("pipe insert requires out mode");
    }
    size_t plen = 0;
    uint8_t *payload = encode_value(value, &plen);
    pthread_mutex_lock(&g_mu);
    uint8_t *body = NULL;
    size_t len = 0, cap = 0;
    append_u8(&body, &len, &cap, OP_PUBLISH);
    append_str(&body, &len, &cap, h->channel);
    append_bytes(&body, &len, &cap, payload, plen);
    free(payload);
    pthread_mutex_lock(&g_ack_mu);
    g_waiting_ack = 1;
    g_ack_ok = 0;
    g_ack_err[0] = 0;
    pthread_mutex_unlock(&g_ack_mu);
    if (send_frame(body, (uint32_t)len) != 0)
    {
        free(body);
        pthread_mutex_unlock(&g_mu);
        munx_rt_abort("pipe publish failed");
    }
    free(body);
    pthread_mutex_unlock(&g_mu);
    wait_hub_ack("pipe publish error");
}

void munx_channel_insert(MunxValue pipe, MunxValue value)
{
    if (pipe.tag != MUNX_TAG_PIPE || !pipe.as.ptr)
    {
        munx_rt_abort("`:=>` on non-channel");
    }
    MunxPipeHandle *h = (MunxPipeHandle *)pipe.as.ptr;
    if (h->mode != MUNX_PIPE_CHANNEL)
    {
        munx_rt_abort("`:=>` requires a channel handle");
    }
    size_t plen = 0;
    uint8_t *payload = encode_value(value, &plen);

    for (int attempt = 0; attempt < 1024; ++attempt)
    {
        pthread_mutex_lock(&g_mu);
        uint8_t *offer = NULL;
        size_t olen = 0, ocap = 0;
        append_u8(&offer, &olen, &ocap, OP_CHANNEL_OFFER);
        append_str(&offer, &olen, &ocap, h->channel);
        pthread_mutex_lock(&g_ack_mu);
        g_waiting_channel = 1;
        g_channel_got = 0;
        g_channel_op = 0;
        g_ack_err[0] = 0;
        pthread_mutex_unlock(&g_ack_mu);
        if (send_frame(offer, (uint32_t)olen) != 0)
        {
            free(offer);
            free(payload);
            pthread_mutex_unlock(&g_mu);
            munx_rt_abort("channel offer failed");
        }
        free(offer);
        pthread_mutex_unlock(&g_mu);

        int reply = wait_channel_reply();
        if (reply == OP_CHANNEL_BUSY)
        {
            channel_backoff();
            continue;
        }

        pthread_mutex_lock(&g_mu);
        uint8_t *body = NULL;
        size_t len = 0, cap = 0;
        append_u8(&body, &len, &cap, OP_PUBLISH);
        append_str(&body, &len, &cap, h->channel);
        append_bytes(&body, &len, &cap, payload, plen);
        pthread_mutex_lock(&g_ack_mu);
        g_waiting_ack = 1;
        g_ack_ok = 0;
        g_ack_err[0] = 0;
        pthread_mutex_unlock(&g_ack_mu);
        if (send_frame(body, (uint32_t)len) != 0)
        {
            free(body);
            free(payload);
            pthread_mutex_unlock(&g_mu);
            munx_rt_abort("channel send failed");
        }
        free(body);
        pthread_mutex_unlock(&g_mu);
        wait_hub_ack("channel send error");
        free(payload);
        return;
    }
    free(payload);
    munx_rt_abort("channel send aborted after too many collisions");
}

MunxValue munx_pipe_extract(MunxValue pipe)
{
    if (pipe.tag != MUNX_TAG_PIPE || !pipe.as.ptr)
    {
        munx_rt_abort("pipe extract on non-pipe");
    }
    MunxPipeHandle *h = (MunxPipeHandle *)pipe.as.ptr;
    if (h->mode == MUNX_PIPE_WRITER || h->mode == MUNX_PIPE_CHANNEL)
    {
        munx_rt_abort("pipe extract requires in or subscribe mode");
    }
    pthread_mutex_lock(&g_mu);
    DeliveryQueue *q = ensure_queue(h->channel, h->mode);
    pthread_mutex_unlock(&g_mu);

    pthread_mutex_lock(&q->mu);
    while (!q->head && !q->closed)
    {
        pthread_cond_wait(&q->cv, &q->mu);
    }
    if (!q->head)
    {
        pthread_mutex_unlock(&q->mu);
        return munx_null();
    }
    DeliveryItem *item = q->head;
    q->head = item->next;
    if (!q->head)
    {
        q->tail = NULL;
    }
    pthread_mutex_unlock(&q->mu);
    MunxValue v = decode_value(item->data, item->len);
    free(item->data);
    free(item);
    return v;
}

MunxValue munx_channel_extract(MunxValue pipe)
{
    if (pipe.tag != MUNX_TAG_PIPE || !pipe.as.ptr)
    {
        munx_rt_abort("`<=:` on non-channel");
    }
    MunxPipeHandle *h = (MunxPipeHandle *)pipe.as.ptr;
    if (h->mode != MUNX_PIPE_CHANNEL)
    {
        munx_rt_abort("`<=:` requires a channel handle");
    }

    pthread_mutex_lock(&g_mu);
    DeliveryQueue *q = ensure_queue(h->channel, MUNX_PIPE_CHANNEL);
    uint8_t *ready = NULL;
    size_t rlen = 0, rcap = 0;
    append_u8(&ready, &rlen, &rcap, OP_CHANNEL_RECV_READY);
    append_str(&ready, &rlen, &rcap, h->channel);
    if (send_frame(ready, (uint32_t)rlen) != 0)
    {
        free(ready);
        pthread_mutex_unlock(&g_mu);
        munx_rt_abort("channel recv-ready failed");
    }
    free(ready);
    pthread_mutex_unlock(&g_mu);

    pthread_mutex_lock(&q->mu);
    while (!q->head && !q->closed)
    {
        pthread_cond_wait(&q->cv, &q->mu);
    }
    if (!q->head)
    {
        pthread_mutex_unlock(&q->mu);
        return munx_null();
    }
    DeliveryItem *item = q->head;
    q->head = item->next;
    if (!q->head)
    {
        q->tail = NULL;
    }
    pthread_mutex_unlock(&q->mu);

    pthread_mutex_lock(&g_mu);
    uint8_t *ack = NULL;
    size_t alen = 0, acap = 0;
    append_u8(&ack, &alen, &acap, OP_CHANNEL_DATA_ACK);
    append_str(&ack, &alen, &acap, h->channel);
    (void)send_frame(ack, (uint32_t)alen);
    free(ack);
    pthread_mutex_unlock(&g_mu);

    MunxValue v = decode_value(item->data, item->len);
    free(item->data);
    free(item);
    return v;
}

MunxValue munx_to_string(MunxValue v)
{
    char buf[64];
    switch (v.tag)
    {
    case MUNX_TAG_NULL:
        return munx_string("null");
    case MUNX_TAG_STRING:
        return v;
    case MUNX_TAG_BOOL:
        return munx_string(v.as.b ? "true" : "false");
    case MUNX_TAG_I64:
        snprintf(buf, sizeof(buf), "%lld", (long long)v.as.i64);
        return munx_string(buf);
    case MUNX_TAG_F64:
        snprintf(buf, sizeof(buf), "%.15g", v.as.f64);
        return munx_string(buf);
    case MUNX_TAG_CHAR:
        buf[0] = v.as.c;
        buf[1] = 0;
        return munx_string(buf);
    default:
        return munx_string("<value>");
    }
}

MunxValue munx_concat(MunxValue a, MunxValue b)
{
    MunxValue sa = munx_to_string(a);
    MunxValue sb = munx_to_string(b);
    const char *as = value_cstr(sa);
    const char *bs = value_cstr(sb);
    size_t na = strlen(as), nb = strlen(bs);
    char *out = (char *)malloc(na + nb + 1);
    memcpy(out, as, na);
    memcpy(out + na, bs, nb);
    out[na + nb] = 0;
    MunxValue r = munx_string(out);
    free(out);
    return r;
}

void munx_fail(MunxValue message)
{
    MunxValue s = munx_to_string(message);
    munx_rt_abort(value_cstr(s));
}

static void pipe_atexit(void) { munx_pipe_session_end(); }

__attribute__((constructor)) static void pipe_register_atexit(void)
{
    atexit(pipe_atexit);
}

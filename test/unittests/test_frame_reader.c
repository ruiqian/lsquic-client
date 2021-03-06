/* Copyright (c) 2017 - 2018 LiteSpeed Technologies Inc.  See LICENSE. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32
#include <unistd.h>
#else
#include <getopt.h>
#endif
#include <sys/queue.h>

#include "lsquic.h"
#include "lsquic_frame_common.h"
#include "lsquic_arr.h"
#include "lsquic_hpack_dec.h"
#include "lsquic_mm.h"
#include "lsquic_logger.h"

#define FRAME_READER_TESTING 0x100
#include "lsquic_frame_reader.h"


struct callback_value   /* What callback returns */
{
    enum {
        CV_HEADERS,
        CV_SETTINGS,
        CV_PUSH_PROMISE,
        CV_PRIORITY,
        CV_ERROR,
    }                                   type;
    unsigned                            stream_off; /* Checked only if not zero */
    union {
        struct uncompressed_headers     uh;
        struct {
            uint16_t                    id;
            uint32_t                    value;
        }                               setting;
        void                           *push_promise;
        struct cv_error {
            enum frame_reader_error     code;
            uint32_t                    stream_id;
        }                               error;
        struct cv_priority {
            uint32_t                    stream_id;
            int                         exclusive;
            uint32_t                    dep_stream_id;
            unsigned                    weight;
        }                               priority;
    }                                   u;
};


void
compare_headers (const struct uncompressed_headers *got_uh,
                 const struct uncompressed_headers *exp_uh)
{
    assert(got_uh->uh_stream_id == exp_uh->uh_stream_id);
    assert(got_uh->uh_oth_stream_id == exp_uh->uh_oth_stream_id);
    assert(got_uh->uh_weight == exp_uh->uh_weight);
    assert(got_uh->uh_exclusive == exp_uh->uh_exclusive);
    assert(got_uh->uh_size == exp_uh->uh_size);
    assert(strlen(got_uh->uh_headers) == got_uh->uh_size);
    assert(got_uh->uh_off == exp_uh->uh_off);
    assert(got_uh->uh_flags == exp_uh->uh_flags);
    assert(0 == memcmp(got_uh->uh_headers, exp_uh->uh_headers, got_uh->uh_size));
}


void
compare_push_promises (const struct uncompressed_headers *got_uh,
                 const struct uncompressed_headers *exp_uh)
{
    assert(got_uh->uh_stream_id == exp_uh->uh_stream_id);
    assert(got_uh->uh_oth_stream_id == exp_uh->uh_oth_stream_id);
    assert(got_uh->uh_size == exp_uh->uh_size);
    assert(strlen(got_uh->uh_headers) == got_uh->uh_size);
    assert(got_uh->uh_flags == exp_uh->uh_flags);
    assert(0 == memcmp(got_uh->uh_headers, exp_uh->uh_headers, got_uh->uh_size));
}


void
compare_priorities (const struct cv_priority *got_prio,
                    const struct cv_priority *exp_prio)
{
    assert(got_prio->stream_id      == exp_prio->stream_id);
    assert(got_prio->exclusive      == exp_prio->exclusive);
    assert(got_prio->dep_stream_id  == exp_prio->dep_stream_id);
    assert(got_prio->weight         == exp_prio->weight);
}


void
compare_errors (const struct cv_error *got_err,
                const struct cv_error *exp_err)
{
    assert(got_err->code == exp_err->code);
    assert(got_err->stream_id == exp_err->stream_id);
}


static void
compare_cb_vals (const struct callback_value *got,
                 const struct callback_value *exp)
{
    assert(got->type == exp->type);
    if (exp->stream_off)
        assert(exp->stream_off == got->stream_off);
    switch (got->type)
    {
    case CV_HEADERS:
        compare_headers(&got->u.uh, &exp->u.uh);
        break;
    case CV_PUSH_PROMISE:
        compare_push_promises(&got->u.uh, &exp->u.uh);
        break;
    case CV_ERROR:
        compare_errors(&got->u.error, &exp->u.error);
        break;
    case CV_PRIORITY:
        compare_priorities(&got->u.priority, &exp->u.priority);
        break;
    case CV_SETTINGS:
        /* TODO */
        break;
    }
}


static struct {
    size_t          in_sz;
    size_t          in_off;
    size_t          in_max_req_sz;
    size_t          in_max_sz;
    unsigned char   in_buf[0x1000];
} input;


static struct cb_ctx {
    unsigned                n_cb_vals;
    struct callback_value   cb_vals[10];
} g_cb_ctx;


static void
reset_cb_ctx (struct cb_ctx *cb_ctx)
{
    cb_ctx->n_cb_vals = 0;
    memset(&cb_ctx->cb_vals, 0xA5, sizeof(cb_ctx->cb_vals));
}


static size_t
uh_size (const struct uncompressed_headers *uh)
{
    return sizeof(*uh) - FRAME_READER_TESTING + uh->uh_size;
}


static void
on_incoming_headers (void *ctx, struct uncompressed_headers *uh)
{
    struct cb_ctx *cb_ctx = ctx;
    assert(cb_ctx == &g_cb_ctx);
    unsigned i = cb_ctx->n_cb_vals++;
    assert(i < sizeof(cb_ctx->cb_vals) / sizeof(cb_ctx->cb_vals[0]));
    cb_ctx->cb_vals[i].type = CV_HEADERS;
    cb_ctx->cb_vals[i].stream_off = input.in_off;
    assert(uh_size(uh) <= sizeof(*uh));
    memcpy(&cb_ctx->cb_vals[i].u.uh, uh, uh_size(uh) + 1 /* NUL byte */);
    free(uh);
}


static void
on_push_promise (void *ctx, struct uncompressed_headers *uh)
{
    struct cb_ctx *cb_ctx = ctx;
    assert(cb_ctx == &g_cb_ctx);
    unsigned i = cb_ctx->n_cb_vals++;
    assert(i < sizeof(cb_ctx->cb_vals) / sizeof(cb_ctx->cb_vals[0]));
    cb_ctx->cb_vals[i].type = CV_PUSH_PROMISE;
    cb_ctx->cb_vals[i].stream_off = input.in_off;
    assert(uh_size(uh) <= sizeof(*uh));
    memcpy(&cb_ctx->cb_vals[i].u.uh, uh, uh_size(uh) + 1 /* NUL byte */);
    free(uh);
}


static void
on_error (void *ctx, uint32_t stream_id, enum frame_reader_error error)
{
    struct cb_ctx *cb_ctx = ctx;
    assert(cb_ctx == &g_cb_ctx);
    unsigned i = cb_ctx->n_cb_vals++;
    assert(i < sizeof(cb_ctx->cb_vals) / sizeof(cb_ctx->cb_vals[0]));
    cb_ctx->cb_vals[i].type = CV_ERROR;
    cb_ctx->cb_vals[i].u.error.stream_id = stream_id;
    cb_ctx->cb_vals[i].u.error.code = error;
    cb_ctx->cb_vals[i].stream_off = input.in_off;
}


static void
on_settings (void *ctx, uint16_t id, uint32_t value)
{
    struct cb_ctx *cb_ctx = ctx;
    assert(cb_ctx == &g_cb_ctx);
    unsigned i = cb_ctx->n_cb_vals++;
    assert(i < sizeof(cb_ctx->cb_vals) / sizeof(cb_ctx->cb_vals[0]));
    cb_ctx->cb_vals[i].type = CV_SETTINGS;
    cb_ctx->cb_vals[i].u.setting.id = id;
    cb_ctx->cb_vals[i].u.setting.value = value;
    cb_ctx->cb_vals[i].stream_off = input.in_off;
}


static void
on_priority (void *ctx, uint32_t stream_id, int exclusive,
             uint32_t dep_stream_id, unsigned weight)
{
    struct cb_ctx *cb_ctx = ctx;
    assert(cb_ctx == &g_cb_ctx);
    unsigned i = cb_ctx->n_cb_vals++;
    assert(i < sizeof(cb_ctx->cb_vals) / sizeof(cb_ctx->cb_vals[0]));
    cb_ctx->cb_vals[i].type = CV_PRIORITY;
    cb_ctx->cb_vals[i].u.priority.stream_id     = stream_id;
    cb_ctx->cb_vals[i].u.priority.exclusive     = exclusive;
    cb_ctx->cb_vals[i].u.priority.dep_stream_id = dep_stream_id;
    cb_ctx->cb_vals[i].u.priority.weight        = weight;
    cb_ctx->cb_vals[i].stream_off = input.in_off;
}


static const struct frame_reader_callbacks frame_callbacks = {
    .frc_on_headers      = on_incoming_headers,
    .frc_on_push_promise = on_push_promise,
    .frc_on_settings     = on_settings,
    .frc_on_priority     = on_priority,
    .frc_on_error        = on_error,
};


static ssize_t
read_from_stream (struct lsquic_stream *stream, void *buf, size_t sz)
{
    if (sz > input.in_max_req_sz)
        input.in_max_req_sz = sz;
    if (input.in_sz - input.in_off < sz)
        sz = input.in_sz - input.in_off;
    if (sz > input.in_max_sz)
        sz = input.in_max_sz;
    memcpy(buf, input.in_buf + input.in_off, sz);
    input.in_off += sz;
    return sz;
}


struct frame_reader_test {
    unsigned                        frt_lineno;
    /* Input */
    enum frame_reader_flags         frt_fr_flags;
    unsigned char                   frt_buf[0x100];
    unsigned short                  frt_bufsz;
    unsigned                        frt_max_headers_sz;
    /* Output */
    unsigned short                  frt_in_off;
    int                             frt_err;      /* True if expecting error */
    unsigned                        frt_n_cb_vals;
    struct callback_value           frt_cb_vals[10];
};


#define UH_HEADERS(str) .uh_headers = (str), .uh_size = sizeof(str) - 1

static const struct frame_reader_test tests[] = {
    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x04,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS,
                            0x80|           /* <----- This bit must be ignored */
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                0x48, 0x82, 0x64, 0x02,
        },
        .frt_bufsz  = 13,
        .frt_n_cb_vals = 1,
        .frt_cb_vals = {
            {
                .type = CV_HEADERS,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0,
                    .uh_weight          = 0,
                    .uh_exclusive       = -1,
                    .uh_off             = 0,
                    UH_HEADERS("HTTP/1.1 302 Found\r\n\r\n"),
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x16,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS|HFHF_PADDED,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Padding length */0x11,
            /* Block fragment: */
                                0x48, 0x82, 0x64, 0x02,
            /* Padding: */      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                0xFF,
        },
        .frt_bufsz  = 9 + 1 + 4 + 17,
        .frt_n_cb_vals = 1,
        .frt_cb_vals = {
            {
                .type = CV_HEADERS,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0,
                    .uh_weight          = 0,
                    .uh_exclusive       = -1,
                    .uh_off             = 0,
                    UH_HEADERS("HTTP/1.1 302 Found\r\n\r\n"),
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x1B,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS|HFHF_PADDED|HFHF_PRIORITY|
                                                            HFHF_END_STREAM,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Padding length */0x11,
            /* Exclusive: */    0x80|
            /* Dep Stream Id: */
                                0x00, 0x00, 0x12, 0x34,
            /* Weight: */       0xFF,
            /* Block fragment: */
                                0x48, 0x82, 0x64, 0x02,
            /* Padding: */      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                0xFF,
            /* Length: */       0x00, 0x00, 0x05,
            /* Type: */         HTTP_FRAME_PRIORITY,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x00, 0x39,
            /* Dep Stream Id: */0x80, 0x00, 0x00, 0x19,
            /* Weight: */       0x77,
        },
        .frt_bufsz  = 9 + 1 + 5 + 4 + 17
                    + 9 + 5,
        .frt_n_cb_vals = 2,
        .frt_cb_vals = {
            {
                .type = CV_HEADERS,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0x1234,
                    .uh_weight          = 0xFF + 1,
                    .uh_exclusive       = 1,
                    .uh_off             = 0,
                    .uh_flags           = UH_FIN,
                    UH_HEADERS("HTTP/1.1 302 Found\r\n\r\n"),
                },
            },
            {
                .type = CV_PRIORITY,
                .u.priority = {
                    .stream_id      = 0x39,
                    .exclusive      = 1,
                    .dep_stream_id  = 0x19,
                    .weight         = 0x77 + 1,
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x09,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS|HFHF_PRIORITY,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Exclusive: */    0x00|
            /* Dep Stream Id: */
                                0x00, 0x00, 0x12, 0x34,
            /* Weight: */       0x00,
            /* Block fragment: */
                                0x48, 0x82, 0x64, 0x02,
        },
        .frt_bufsz  = 9 + 5 + 4,
        .frt_n_cb_vals = 1,
        .frt_cb_vals = {
            {
                .type = CV_HEADERS,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0x1234,
                    .uh_weight          = 1,
                    .uh_exclusive       = 0,
                    .uh_off             = 0,
                    UH_HEADERS("HTTP/1.1 302 Found\r\n\r\n"),
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x0E,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS|HFHF_PRIORITY,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Exclusive: */    0x00|
            /* Dep Stream Id: */
                                0x00, 0x00, 0x12, 0x34,
            /* Weight: */       0x00,
            /* Block fragment: */
                                0x48, 0x82, 0x64, 0x02,
                                0x60, 0x03, 0x61, 0x3d, 0x62,
        },
        .frt_bufsz  = 9 + 5 + 4 + 5,
        .frt_n_cb_vals = 1,
        .frt_cb_vals = {
            {
                .type = CV_HEADERS,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0x1234,
                    .uh_weight          = 1,
                    .uh_exclusive       = 0,
                    .uh_off             = 0,
                    UH_HEADERS("HTTP/1.1 302 Found\r\n"
                               "Cookie: a=b\r\n\r\n"),
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x18,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS|HFHF_PRIORITY,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Exclusive: */    0x00|
            /* Dep Stream Id: */
                                0x00, 0x00, 0x12, 0x34,
            /* Weight: */       0x00,
            /* Block fragment: */
                                0x48, 0x82, 0x64, 0x02,
                                0x60, 0x03, 0x61, 0x3d, 0x62,
                                0x60, 0x03, 0x63, 0x3d, 0x64,
                                0x60, 0x03, 0x65, 0x3d, 0x66,
        },
        .frt_bufsz  = 9 + 5 + 4 + 15,
        .frt_n_cb_vals = 1,
        .frt_cb_vals = {
            {
                .type = CV_HEADERS,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0x1234,
                    .uh_weight          = 1,
                    .uh_exclusive       = 0,
                    .uh_off             = 0,
                    UH_HEADERS("HTTP/1.1 302 Found\r\n"
                               "Cookie: a=b; c=d; e=f\r\n\r\n"),
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = FRF_SERVER,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x16,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS|HFHF_PRIORITY,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Exclusive: */    0x00|
            /* Dep Stream Id: */
                                0x00, 0x00, 0x12, 0x34,
            /* Weight: */       0x00,
            /* Block fragment: */
                                0x82, 0x84, 0x86, 0x41, 0x8c, 0xf1, 0xe3, 0xc2,
                                0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4,
                                0xff,
            /* Length: */       0x00, 0x00, 0xEE,
            /* Type: */         HTTP_FRAME_CONTINUATION,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                'W', 'H', 'A', 'T', 'E', 'V', 'E', 'R',
        },
        .frt_bufsz  = 9 + 5 + 17
                    + 9 + 0 + 8,
        .frt_err = 1,
        .frt_in_off = 9 + 5 + 17 + 9,
        .frt_n_cb_vals = 1,
        .frt_cb_vals = {
            {
                .type = CV_HEADERS,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0x1234,
                    .uh_weight          = 1,
                    .uh_exclusive       = 0,
                    .uh_off             = 0,
                    UH_HEADERS("GET / HTTP/1.1\r\nHost: www.example.com\r\n\r\n"),
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = FRF_SERVER,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x16,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS|HFHF_PRIORITY,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Exclusive: */    0x00|
            /* Dep Stream Id: */
                                0x00, 0x00, 0x12, 0x34,
            /* Weight: */       0x00,
            /* Block fragment: */
                                0x82, 0x84, 0x86, 0x41, 0x8c, 0xf1, 0xe3, 0xc2,
                                0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4,
                                0xff,
            /* Length: */       0x00, 0x00, 0xEE,
            /* Type: */         HTTP_FRAME_CONTINUATION,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                'W', 'H', 'A', 'T', 'E', 'V', 'E', 'R',
        },
        .frt_bufsz  = 9 + 5 + 17
                    + 9 + 0 + 8,
        .frt_err = 1,
        .frt_in_off = 9 + 5 + 17 + 9,
        .frt_n_cb_vals = 1,
        .frt_cb_vals = {
            {
                .type = CV_HEADERS,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0x1234,
                    .uh_weight          = 1,
                    .uh_exclusive       = 0,
                    .uh_off             = 0,
                    UH_HEADERS("GET / HTTP/1.1\r\nHost: www.example.com\r\n\r\n"),
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = FRF_SERVER,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x16,
            /* Type: */         0x01,
            /* Flags: */        HFHF_PRIORITY,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Exclusive: */    0x00|
            /* Dep Stream Id: */
                                0x00, 0x00, 0x12, 0x34,
            /* Weight: */       0x00,
            /* Block fragment: */
                                0x82, 0x84, 0x86, 0x41, 0x8c, 0xf1, 0xe3, 0xc2,
                                0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4,
                                0xff,
            /* Length: */       0x00, 0x00, 0xEE,
            /* Type: */         HTTP_FRAME_CONTINUATION,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0xFF, 0x30, 0x39, /* Stream ID does not match */
            /* Block fragment: */
                                'W', 'H', 'A', 'T', 'E', 'V', 'E', 'R',
        },
        .frt_bufsz  = 9 + 5 + 17
                    + 9 + 0 + 8,
        .frt_err = 1,
        .frt_in_off = 9 + 5 + 17 + 9,
        .frt_n_cb_vals = 0,
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = FRF_SERVER,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0xEE,
            /* Type: */         HTTP_FRAME_CONTINUATION,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                'W', 'H', 'A', 'T', 'E', 'V', 'E', 'R',
        },
        .frt_bufsz  = 9 + 0 + 8,
        .frt_err = 1,
        .frt_in_off = 9,
        .frt_n_cb_vals = 0,
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = FRF_SERVER,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x10,
            /* Type: */         0x01,
            /* Flags: */        0x00,   /* Note absence of HFHF_END_HEADERS */
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment:
             *   perl hpack.pl :method GET :path / host www.example.com
             */
                                0x82, 0x84, 0x66, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5,
                                0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff,
            /* Length: */       0x00, 0x00, 0x08,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                'W', 'H', 'A', 'T', 'E', 'V', 'E', 'R',
        },
        .frt_bufsz  = 9 + 0 + 16
                    + 9 + 0 + 8,
        .frt_in_off = 9 + 16 + 9,
        .frt_err = 1,
        .frt_n_cb_vals = 1,
        .frt_cb_vals = {
            {
                .type = CV_ERROR,
                .u.error = {
                    .stream_id  = 0x3039,
                    .code       = FR_ERR_EXPECTED_CONTIN,
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = FRF_SERVER,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x10,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment:
             *   perl hpack.pl :method GET :path / host www.example.com
             */
                                0x82, 0x84, 0x66, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5,
                                0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff,
            /* Length: */       0x00, 0x00, 0x1A,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS|HFHF_PRIORITY,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Exclusive: */    0x00|
            /* Dep Stream Id: */
                                0x00, 0x00, 0x12, 0x34,
            /* Weight: */       0x00,
            /* Block fragment:
             *   perl hpack.pl :method GET :path / :scheme http Host www.example.com
             */
                                0x82, 0x84, 0x86, 0x40, 0x83, 0xc6, 0x74, 0x27,
                                0x8c, 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b,
                                0xa0, 0xab, 0x90, 0xf4, 0xff,
            /* Length: */       0x00, 0x00, 0x11,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                0x82, 0x84, 0x86, 0x41, 0x8c, 0xf1, 0xe3, 0xc2,
                                0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4,
                                0xff,
        },
        .frt_bufsz  = 9 + 0 + 16
                    + 9 + 5 + 21
                    + 9 + 0 + 17,
        .frt_n_cb_vals = 3,
        .frt_cb_vals = {
            {
                .type = CV_ERROR,
                .u.error = {
                    .stream_id  = 12345,
                    .code       = FR_ERR_INCOMPL_REQ_PSEH,
                },
            },
            {
                .type = CV_ERROR,
                .u.error = {
                    .stream_id  = 12345,
                    .code       = FR_ERR_UPPERCASE_HEADER,
                },
            },
            {
                .type = CV_HEADERS,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0,
                    .uh_weight          = 0,
                    .uh_exclusive       = -1,
                    .uh_off             = 0,
                    UH_HEADERS("GET / HTTP/1.1\r\nHost: www.example.com\r\n\r\n"),
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x15,
            /* Type: */         HTTP_FRAME_PUSH_PROMISE,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Dep stream Id: */0x00, 0x12, 0x34, 0x56,
            /* Block fragment: */
                                0x82, 0x84, 0x86, 0x41, 0x8c, 0xf1, 0xe3, 0xc2,
                                0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4,
                                0xff,
        },
        .frt_bufsz  = 9 + 0 + 0x15,
        .frt_n_cb_vals = 1,
        .frt_cb_vals = {
            {
                .type = CV_PUSH_PROMISE,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0x123456,
                    .uh_flags           = UH_PP,
                    UH_HEADERS("GET / HTTP/1.1\r\nHost: www.example.com\r\n\r\n"),
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x02,
            /* Type: */         HTTP_FRAME_HEADERS,
            /* Flags: */        0x00,
                            0x80|           /* <----- This bit must be ignored */
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                0x48, 0x82,
            /* Length: */       0x00, 0x00, 0x02,
            /* Type: */         HTTP_FRAME_CONTINUATION,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                0x64, 0x02,
        },
        .frt_bufsz  = 9 + 2 + 9 + 2,
        .frt_n_cb_vals = 1,
        .frt_cb_vals = {
            {
                .type = CV_HEADERS,
                .u.uh = {
                    .uh_stream_id       = 12345,
                    .uh_oth_stream_id   = 0,
                    .uh_weight          = 0,
                    .uh_exclusive       = -1,
                    .uh_off             = 0,
                    UH_HEADERS("HTTP/1.1 302 Found\r\n\r\n"),
                },
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x00,
            /* Type: */         HTTP_FRAME_SETTINGS,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
        },
        .frt_bufsz  = 9,
        .frt_n_cb_vals = 1,
        .frt_err = 1,
        .frt_cb_vals = {
            {
                .type = CV_ERROR,
                .u.error.code = FR_ERR_INVALID_FRAME_SIZE,
                .u.error.stream_id = 12345,
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x07,
            /* Type: */         HTTP_FRAME_SETTINGS,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        },
        .frt_bufsz  = 9 + 7,
        .frt_n_cb_vals = 1,
        .frt_err = 1,
        .frt_in_off = 9,
        .frt_cb_vals = {
            {
                .type = CV_ERROR,
                .u.error.code = FR_ERR_INVALID_FRAME_SIZE,
                .u.error.stream_id = 12345,
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x06,
            /* Type: */         HTTP_FRAME_SETTINGS,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        },
        .frt_bufsz  = 9 + 6,
        .frt_n_cb_vals = 1,
        .frt_err = 1,
        .frt_in_off = 9,
        .frt_cb_vals = {
            {
                .type = CV_ERROR,
                .u.error.code = FR_ERR_NONZERO_STREAM_ID,
                .u.error.stream_id = 12345,
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x0C,
            /* Type: */         HTTP_FRAME_SETTINGS,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x00, 0x00,
                                0x00, SETTINGS_INITIAL_WINDOW_SIZE,
                                0x01, 0x02, 0x03, 0x04,
                                0x00, SETTINGS_HEADER_TABLE_SIZE,
                                0x02, 0x03, 0x04, 0x05,
        },
        .frt_bufsz  = 9 + 12,
        .frt_n_cb_vals = 2,
        .frt_cb_vals = {
            {
                .type = CV_SETTINGS,
                .u.setting.id    = SETTINGS_INITIAL_WINDOW_SIZE,
                .u.setting.value = 0x01020304,
            },
            {
                .type = CV_SETTINGS,
                .u.setting.id    = SETTINGS_HEADER_TABLE_SIZE,
                .u.setting.value = 0x02030405,
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x09,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS|HFHF_PRIORITY,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Exclusive: */    0x00|
            /* Dep Stream Id: */
                                0x00, 0x00, 0x12, 0x34,
            /* Weight: */       0x00,
            /* Block fragment: */
                                0x48, 0x82, 0x64, 0x02,
            /* Length: */       0x00, 0x00, 0x06,
            /* Type: */         HTTP_FRAME_SETTINGS,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x00, 0x00,
                                0x00, SETTINGS_INITIAL_WINDOW_SIZE,
                                0x01, 0x02, 0x03, 0x04,
        },
        .frt_bufsz  = 9 + 5 + 4 + 9 + 6,
        .frt_max_headers_sz = 10,
        .frt_n_cb_vals = 2,
        .frt_cb_vals = {
            {
                .type = CV_ERROR,
                .stream_off = 9 + 5 + 4,
                .u.error.code = FR_ERR_HEADERS_TOO_LARGE,
                .u.error.stream_id = 12345,
            },
            {
                .type = CV_SETTINGS,
                .u.setting.id    = SETTINGS_INITIAL_WINDOW_SIZE,
                .u.setting.value = 0x01020304,
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x11,
            /* Type: */         0x01,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                /* 0x11 bytes of no consequence: they are not
                                 * parsed.
                                 */
                                000, 001, 002, 003, 004, 005, 006, 007,
                                010, 011, 012, 013, 014, 015, 016, 017,
                                020,
            /* Length: */       0x00, 0x00, 0x06,
            /* Type: */         HTTP_FRAME_SETTINGS,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x00, 0x00,
                                0x00, SETTINGS_INITIAL_WINDOW_SIZE,
                                0x01, 0x02, 0x03, 0x04,
        },
        .frt_bufsz  = 9 + 0 + 0x11 + 9 + 6,
        .frt_max_headers_sz = 0x10,
        .frt_n_cb_vals = 2,
        .frt_cb_vals = {
            {
                .type = CV_ERROR,
                .stream_off = 9,
                .u.error.code = FR_ERR_HEADERS_TOO_LARGE,
                .u.error.stream_id = 12345,
            },
            {
                .type = CV_SETTINGS,
                .u.setting.id    = SETTINGS_INITIAL_WINDOW_SIZE,
                .u.setting.value = 0x01020304,
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x10,
            /* Type: */         0x01,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                /* 0x10 bytes of no consequence: they are not
                                 * parsed.
                                 */
                                000, 001, 002, 003, 004, 005, 006, 007,
                                010, 011, 012, 013, 014, 015, 016, 017,
            /* Length: */       0x00, 0x00, 0x10,
            /* Type: */         HTTP_FRAME_CONTINUATION,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                000, 001, 002, 003, 004, 005, 006, 007,
                                010, 011, 012, 013, 014, 015, 016, 017,
            /* Length: */       0x00, 0x00, 0x10,
            /* Type: */         HTTP_FRAME_CONTINUATION,
            /* Flags: */        HFHF_END_HEADERS,
            /* Stream Id: */    0x00, 0x00, 0x30, 0x39,
            /* Block fragment: */
                                000, 001, 002, 003, 004, 005, 006, 007,
                                010, 011, 012, 013, 014, 015, 016, 017,
            /* Length: */       0x00, 0x00, 0x06,
            /* Type: */         HTTP_FRAME_SETTINGS,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x00, 0x00,
                                0x00, SETTINGS_INITIAL_WINDOW_SIZE,
                                0x01, 0x02, 0x03, 0x04,
        },
        .frt_bufsz  = 9 + 0 + 0x10 + 9 + 0 + 0x10 + 9 + 0 + 0x10 + 9 + 6,
        .frt_max_headers_sz = 0x19,
        .frt_n_cb_vals = 2,
        .frt_cb_vals = {
            {
                .type = CV_ERROR,
                .stream_off = 9 + 0 + 0x10 + 9,
                .u.error.code = FR_ERR_HEADERS_TOO_LARGE,
                .u.error.stream_id = 12345,
            },
            {
                .type = CV_SETTINGS,
                .u.setting.id    = SETTINGS_INITIAL_WINDOW_SIZE,
                .u.setting.value = 0x01020304,
            },
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00,
                                            0x04,  /* <-- wrong payload size */
            /* Type: */         HTTP_FRAME_PRIORITY,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x00, 0x39,
            /* Dep Stream Id: */0x80, 0x00, 0x00, 0x19,
            /* Weight: */       0x77,
        },
        .frt_bufsz  = 9 + 5,
        .frt_n_cb_vals = 1,
        .frt_err = 1,
        .frt_in_off = 9,
        .frt_cb_vals = {
            {
                .type = CV_ERROR,
                .stream_off = 9,
                .u.error.code = FR_ERR_INVALID_FRAME_SIZE,
                .u.error.stream_id = 0x39,
            }
        },
    },

    {   .frt_lineno = __LINE__,
        .frt_fr_flags = 0,
        .frt_buf    = {
            /* Length: */       0x00, 0x00, 0x05,
            /* Type: */         HTTP_FRAME_PRIORITY,
            /* Flags: */        0x00,
            /* Stream Id: */    0x00, 0x00, 0x00, 0x00, /* Invalid stream ID */
            /* Dep Stream Id: */0x80, 0x00, 0x00, 0x19,
            /* Weight: */       0x77,
        },
        .frt_bufsz  = 9 + 5,
        .frt_n_cb_vals = 1,
        .frt_err = 1,
        .frt_in_off = 9,
        .frt_cb_vals = {
            {
                .type = CV_ERROR,
                .stream_off = 9,
                .u.error.code = FR_ERR_ZERO_STREAM_ID,
                .u.error.stream_id = 0x00,
            }
        },
    },

    {
        .frt_bufsz  = 0,
    },
};


static void
test_one_frt (const struct frame_reader_test *frt)
{
    struct lsquic_frame_reader *fr;
    unsigned short exp_off;
    struct lsquic_hdec hdec;
    struct lsquic_mm mm;
    int s;

    lsquic_mm_init(&mm);
    lsquic_hdec_init(&hdec);
    memset(&input, 0, sizeof(input));
    memcpy(input.in_buf, frt->frt_buf, frt->frt_bufsz);
    input.in_sz  = frt->frt_bufsz;

    do
    {
        reset_cb_ctx(&g_cb_ctx);
        input.in_off = 0;
        ++input.in_max_sz;

        fr = lsquic_frame_reader_new(frt->frt_fr_flags, frt->frt_max_headers_sz,
                &mm, NULL, read_from_stream, &hdec, &frame_callbacks, &g_cb_ctx);
        do
        {
            s = lsquic_frame_reader_read(fr);
            if (s != 0)
                break;
        }
        while (input.in_off < input.in_sz);

        assert(frt->frt_err || 0 == s);

        assert(g_cb_ctx.n_cb_vals == frt->frt_n_cb_vals);

        unsigned i;
        for (i = 0; i < g_cb_ctx.n_cb_vals; ++i)
            compare_cb_vals(&g_cb_ctx.cb_vals[i], &frt->frt_cb_vals[i]);

        exp_off = frt->frt_in_off;
        if (!exp_off)
            exp_off = frt->frt_bufsz;
        assert(input.in_off == exp_off);

        lsquic_frame_reader_destroy(fr);
    }
    while (input.in_max_sz < input.in_max_req_sz);
    lsquic_hdec_cleanup(&hdec);
    lsquic_mm_cleanup(&mm);
}


int
main (int argc, char **argv)
{
    int opt;

    while (-1 != (opt = getopt(argc, argv, "l:")))
    {
        switch (opt)
        {
        case 'l':
            lsquic_log_to_fstream(stderr, LLTS_NONE);
            lsquic_logger_lopt(optarg);
            break;
        default:
            exit(1);
        }
    }

    const struct frame_reader_test *frt;
    for (frt = tests; frt->frt_bufsz > 0; ++frt)
        test_one_frt(frt);
    return 0;
}

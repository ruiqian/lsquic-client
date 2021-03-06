/* Copyright (c) 2017 - 2018 LiteSpeed Technologies Inc.  See LICENSE. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <fcntl.h>
#include <limits.h>
#ifndef WIN32
#include <unistd.h>
#else
#include <getopt.h>
#endif

#include "lsquic.h"

#include "lsquic_alarmset.h"
#include "lsquic_packet_common.h"
#include "lsquic_packet_in.h"
#include "lsquic_conn_flow.h"
#include "lsquic_rtt.h"
#include "lsquic_sfcw.h"
#include "lsquic_stream.h"
#include "lsquic_types.h"
#include "lsquic_malo.h"
#include "lsquic_mm.h"
#include "lsquic_conn_public.h"
#include "lsquic_logger.h"
#include "lsquic_parse.h"
#include "lsquic_conn.h"
#include "lsquic_engine_public.h"
#include "lsquic_cubic.h"
#include "lsquic_pacer.h"
#include "lsquic_senhist.h"
#include "lsquic_send_ctl.h"
#include "lsquic_ver_neg.h"
#include "lsquic_packet_out.h"

static const struct parse_funcs *const pf = select_pf_by_ver(LSQVER_037);

struct test_ctl_settings
{
    int     tcs_schedule_stream_packets_immediately;
    int     tcs_have_delayed_packets;
    int     tcs_can_send;
    enum buf_packet_type
            tcs_bp_type;
    enum lsquic_packno_bits
            tcs_guess_packno_bits,
            tcs_calc_packno_bits;
};


static struct test_ctl_settings g_ctl_settings;


static void
init_buf (void *buf, size_t sz);


/* Set values to default */
static void
init_test_ctl_settings (struct test_ctl_settings *settings)
{
    settings->tcs_schedule_stream_packets_immediately      = 1;
    settings->tcs_have_delayed_packets = 0;
    settings->tcs_can_send             = 1;
    settings->tcs_bp_type              = BPT_HIGHEST_PRIO;
    settings->tcs_guess_packno_bits    = PACKNO_LEN_2;
    settings->tcs_calc_packno_bits     = PACKNO_LEN_2;
}


#if __GNUC__
__attribute__((unused))
#endif
static void
apply_test_ctl_settings (const struct test_ctl_settings *settings)
{
    g_ctl_settings = *settings;
}


enum lsquic_packno_bits
lsquic_send_ctl_calc_packno_bits (struct lsquic_send_ctl *ctl)
{
    return g_ctl_settings.tcs_calc_packno_bits;
}


int
lsquic_send_ctl_schedule_stream_packets_immediately (struct lsquic_send_ctl *ctl)
{
    return g_ctl_settings.tcs_schedule_stream_packets_immediately;
}


int
lsquic_send_ctl_have_delayed_packets (const struct lsquic_send_ctl *ctl)
{
    return g_ctl_settings.tcs_have_delayed_packets;
}


int
lsquic_send_ctl_can_send (struct lsquic_send_ctl *ctl)
{
    return g_ctl_settings.tcs_can_send;
}


enum lsquic_packno_bits
lsquic_send_ctl_guess_packno_bits (struct lsquic_send_ctl *ctl)
{
    return g_ctl_settings.tcs_guess_packno_bits;
}


enum buf_packet_type
lsquic_send_ctl_determine_bpt (struct lsquic_send_ctl *ctl,
                                        const struct lsquic_stream *stream)
{
    return g_ctl_settings.tcs_bp_type;
}


/* This function is only here to avoid crash in the test: */
void
lsquic_engine_add_conn_to_pend_rw (struct lsquic_engine_public *enpub,
                                lsquic_conn_t *conn, enum rw_reason reason)
{
}


static unsigned n_closed;
static enum stream_ctor_flags stream_ctor_flags =
                                        SCF_CALL_ON_NEW|SCF_DI_AUTOSWITCH;

struct test_ctx {
    lsquic_stream_t     *stream;
};


static lsquic_stream_ctx_t *
on_new_stream (void *stream_if_ctx, lsquic_stream_t *stream)
{
    struct test_ctx *test_ctx = stream_if_ctx;
    test_ctx->stream = stream;
    return NULL;
}


static void
on_close (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    ++n_closed;
}


const struct lsquic_stream_if stream_if = {
    .on_new_stream          = on_new_stream,
    .on_close               = on_close,
};


/* This does not do anything beyond just acking the packet: we do not attempt
 * to update the send controller to have the correct state.
 */
static void
ack_packet (lsquic_send_ctl_t *send_ctl, lsquic_packno_t packno)
{
    struct lsquic_packet_out *packet_out;
    TAILQ_FOREACH(packet_out, &send_ctl->sc_unacked_packets, po_next)
        if (packet_out->po_packno == packno)
        {
            lsquic_packet_out_ack_streams(packet_out);
            return;
        }
    assert(0);
}


static size_t
read_from_scheduled_packets (lsquic_send_ctl_t *send_ctl, uint32_t stream_id,
    unsigned char *const begin, size_t bufsz, uint64_t first_offset, int *p_fin,
    int fullcheck)
{
    const struct parse_funcs *const pf_local = send_ctl->sc_conn_pub->lconn->cn_pf;
    unsigned char *p = begin;
    unsigned char *const end = p + bufsz;
    const struct stream_rec *srec;
    struct packet_out_srec_iter posi;
    struct lsquic_packet_out *packet_out;
    struct stream_frame frame;
    int len, fin = 0;

    TAILQ_FOREACH(packet_out, &send_ctl->sc_scheduled_packets, po_next)
        for (srec = posi_first(&posi, packet_out); srec;
                                                    srec = posi_next(&posi))
        {
            if (fullcheck)
            {
                assert(srec->sr_frame_types & (1 << QUIC_FRAME_STREAM));
                if (packet_out->po_packno != 1)
                {
                    /* First packet may contain two stream frames, do not
                     * check it.
                     */
                    assert(!posi_next(&posi));
                    if (TAILQ_NEXT(packet_out, po_next))
                    {
                        assert(packet_out->po_data_sz == packet_out->po_n_alloc);
                        assert(srec->sr_len == packet_out->po_data_sz);
                    }
                }
            }
            if ((srec->sr_frame_types & (1 << QUIC_FRAME_STREAM)) &&
                                            srec->sr_stream->id == stream_id)
            {
                assert(!fin);
                len = pf_local->pf_parse_stream_frame(packet_out->po_data + srec->sr_off,
                    packet_out->po_data_sz - srec->sr_off, &frame);
                assert(len > 0);
                assert(frame.stream_id == srec->sr_stream->id);
                /* Otherwise not enough to copy to: */
                assert(end - p >= frame.data_frame.df_size);
                /* Checks offset ordering: */
                assert(frame.data_frame.df_offset ==
                                        first_offset + (uintptr_t) (p - begin));
                if (frame.data_frame.df_fin)
                {
                    assert(!fin);
                    fin = 1;
                }
                memcpy(p, packet_out->po_data + srec->sr_off + len -
                    frame.data_frame.df_size, frame.data_frame.df_size);
                p += frame.data_frame.df_size;
            }
        }

    if (p_fin)
        *p_fin = fin;
    return p + bufsz - end;
}


static struct test_ctx test_ctx;


struct test_objs {
    struct lsquic_engine_public eng_pub;
    struct lsquic_conn        lconn;
    struct lsquic_conn_public conn_pub;
    struct lsquic_send_ctl    send_ctl;
    struct lsquic_alarmset    alset;
    void                     *stream_if_ctx;
    struct ver_neg            ver_neg;
    const struct lsquic_stream_if *
                              stream_if;
    unsigned                  initial_stream_window;
    enum stream_ctor_flags    ctor_flags;
};


static void
init_test_objs (struct test_objs *tobjs, unsigned initial_conn_window,
                unsigned initial_stream_window)
{
    memset(tobjs, 0, sizeof(*tobjs));
    tobjs->lconn.cn_pf = pf;
    tobjs->lconn.cn_pack_size = 1370;
    lsquic_mm_init(&tobjs->eng_pub.enp_mm);
    TAILQ_INIT(&tobjs->conn_pub.sending_streams);
    TAILQ_INIT(&tobjs->conn_pub.read_streams);
    TAILQ_INIT(&tobjs->conn_pub.write_streams);
    TAILQ_INIT(&tobjs->conn_pub.service_streams);
    lsquic_cfcw_init(&tobjs->conn_pub.cfcw, &tobjs->conn_pub,
                                                    initial_conn_window);
    lsquic_conn_cap_init(&tobjs->conn_pub.conn_cap, initial_conn_window);
    lsquic_alarmset_init(&tobjs->alset, 0);
    tobjs->conn_pub.mm = &tobjs->eng_pub.enp_mm;
    tobjs->conn_pub.lconn = &tobjs->lconn;
    tobjs->conn_pub.enpub = &tobjs->eng_pub;
    tobjs->conn_pub.send_ctl = &tobjs->send_ctl;
    tobjs->conn_pub.packet_out_malo =
                        lsquic_malo_create(sizeof(struct lsquic_packet_out));
    tobjs->initial_stream_window = initial_stream_window;
    lsquic_send_ctl_init(&tobjs->send_ctl, &tobjs->alset, &tobjs->eng_pub,
        &tobjs->ver_neg, &tobjs->conn_pub, tobjs->lconn.cn_pack_size);
    tobjs->stream_if = &stream_if;
    tobjs->stream_if_ctx = &test_ctx;
    tobjs->ctor_flags = stream_ctor_flags;
}


static void
deinit_test_objs (struct test_objs *tobjs)
{
    assert(!lsquic_malo_first(tobjs->eng_pub.enp_mm.malo.stream_frame));
    lsquic_send_ctl_cleanup(&tobjs->send_ctl);
    lsquic_malo_destroy(tobjs->conn_pub.packet_out_malo);
    lsquic_mm_cleanup(&tobjs->eng_pub.enp_mm);
}


/* Create a new stream frame.  Each stream frame has a real packet_in to
 * back it up, just like in real code.  The contents of the packet do
 * not matter.
 */
static stream_frame_t *
new_frame_in_ext (struct test_objs *tobjs, size_t off, size_t sz, int fin,
                                                            const void *data)
{
    lsquic_packet_in_t *packet_in;
    stream_frame_t *frame;

    assert(sz <= 1370);

    packet_in = lsquic_mm_get_packet_in(&tobjs->eng_pub.enp_mm);
    if (data)
        packet_in->pi_data = (void *) data;
    else
    {
        packet_in->pi_data = lsquic_mm_get_1370(&tobjs->eng_pub.enp_mm);
        packet_in->pi_flags |= PI_OWN_DATA;
        memset(packet_in->pi_data, 'A', sz);
    }
    /* This is not how stream frame looks in the packet: we have no
     * header.  In our test case it does not matter, as we only care
     * about stream frame.
     */
    packet_in->pi_data_sz = sz;
    packet_in->pi_refcnt = 1;

    frame = lsquic_malo_get(tobjs->eng_pub.enp_mm.malo.stream_frame);
    memset(frame, 0, sizeof(*frame));
    frame->packet_in = packet_in;
    frame->data_frame.df_offset = off;
    frame->data_frame.df_size = sz;
    frame->data_frame.df_data = &packet_in->pi_data[0];
    frame->data_frame.df_fin  = fin;

    return frame;
}


static stream_frame_t *
new_frame_in (struct test_objs *tobjs, size_t off, size_t sz, int fin)
{
    return new_frame_in_ext(tobjs, off, sz, fin, NULL);
}


static lsquic_stream_t *
new_stream_ext (struct test_objs *tobjs, unsigned stream_id, uint64_t send_off)
{
    return lsquic_stream_new_ext(stream_id, &tobjs->conn_pub, tobjs->stream_if,
        tobjs->stream_if_ctx, tobjs->initial_stream_window, send_off,
        tobjs->ctor_flags);
}


static lsquic_stream_t *
new_stream (struct test_objs *tobjs, unsigned stream_id)
{
    return new_stream_ext(tobjs, stream_id, 0);
}


static void
run_frame_ordering_test (uint64_t run_id /* This is used to make it easier to set breakpoints */,
                         int *idx, size_t idx_sz, int read_asap)
{
    int s;
    size_t nw = 0, i;
    char buf[0x1000];

    struct test_objs tobjs;

    init_test_objs(&tobjs, 0x4000, 0x4000);

    lsquic_stream_t *stream = new_stream(&tobjs, 123);
    struct lsquic_mm *const mm = &tobjs.eng_pub.enp_mm;
    struct malo *const frame_malo = mm->malo.stream_frame;

    lsquic_packet_in_t *packet_in = lsquic_mm_get_packet_in(mm);
    packet_in->pi_data = lsquic_mm_get_1370(mm);
    packet_in->pi_flags |= PI_OWN_DATA;
    assert(idx_sz <= 10);
    memcpy(packet_in->pi_data, "0123456789", 10);
    packet_in->pi_data_sz = 10;
    packet_in->pi_refcnt = idx_sz;

    printf("inserting ");
    for (i = 0; i < idx_sz; ++i)
    {
        stream_frame_t *frame;
        frame = lsquic_malo_get(frame_malo);
        memset(frame, 0, sizeof(*frame));
        frame->packet_in = packet_in;
        frame->data_frame.df_offset = idx[i];
        if (idx[i] + 1 == (int) idx_sz)
        {
            printf("<FIN>");
            frame->data_frame.df_size = 0;
            frame->data_frame.df_fin         = 1;
        }
        else
        {
            printf("%c", packet_in->pi_data[idx[i]]);
            frame->data_frame.df_size = 1;
            frame->data_frame.df_data = &packet_in->pi_data[idx[i]];
        }
        if (frame->data_frame.df_fin && read_asap && i + 1 == idx_sz)
        {   /* Last frame is the FIN frame.  Read before inserting zero-sized
             * FIN frame.
             */
            nw = lsquic_stream_read(stream, buf, 10);
            assert(("Read idx_sz bytes", nw == idx_sz - 1));
            assert(("Have not reached fin yet (frame has not come in)",
                -1 == lsquic_stream_read(stream, buf, 1) && errno == EWOULDBLOCK));
        }
        s = lsquic_stream_frame_in(stream, frame);
        assert(("Inserted frame", 0 == s));
    }
    printf("\n");

    if (read_asap && nw == idx_sz - 1)
    {
        assert(("Reached fin", 0 == lsquic_stream_read(stream, buf, 1)));
    }
    else
    {
        nw = lsquic_stream_read(stream, buf, 10);
        assert(("Read idx_sz bytes", nw == idx_sz - 1));
        assert(("Reached fin", 0 == lsquic_stream_read(stream, buf, 1)));
    }

    lsquic_stream_destroy(stream);

    assert(("all frames have been released", !lsquic_malo_first(frame_malo)));
    deinit_test_objs(&tobjs);
}


static void
permute_and_run (uint64_t run_id,
                 int mask, int level, int *idx, size_t idx_sz)
{
    size_t i;
    for (i = 0; i < idx_sz; ++i)
    {
        if (!(mask & (1 << i)))
        {
            idx[level] = i;
            if (level + 1 == (int) idx_sz)
            {
                run_frame_ordering_test(run_id, idx, idx_sz, 0);
                run_frame_ordering_test(run_id, idx, idx_sz, 1);
            }
            else
                permute_and_run(run_id | (i << (8 * level)),
                                mask | (1 << i), level + 1, idx, idx_sz);
        }
    }
}


/* Client: we send some data and FIN, and remote end sends some data and
 * FIN.
 */
static void
test_loc_FIN_rem_FIN (struct test_objs *tobjs)
{
    lsquic_stream_t *stream;
    lsquic_packet_out_t *packet_out;
    char buf_out[0x100];
    unsigned char buf[0x100];
    ssize_t n;
    int s, fin;;

    init_buf(buf_out, sizeof(buf_out));

    stream = new_stream(tobjs, 345);
    n = lsquic_stream_write(stream, buf_out, 100);
    assert(n == 100);
    assert(0 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    s = lsquic_stream_flush(stream);
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    n = read_from_scheduled_packets(&tobjs->send_ctl, stream->id, buf,
                                                    sizeof(buf), 0, &fin, 0);
    assert(100 == n);
    assert(0 == memcmp(buf_out, buf, 100));
    assert(!fin);

    /* Pretend we sent out a packet: */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs->send_ctl);
    lsquic_send_ctl_sent_packet(&tobjs->send_ctl, packet_out, 1);

    s = lsquic_stream_shutdown(stream, 1);
    assert(s == 0);
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl)); /* Shutdown performs a flush */
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));  /* No need to close stream yet */

    n = read_from_scheduled_packets(&tobjs->send_ctl, stream->id, buf,
                                                sizeof(buf), 100, &fin, 0);
    assert(0 == n);
    assert(fin);

    /* Pretend we sent out this packet as well: */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs->send_ctl);
    lsquic_send_ctl_sent_packet(&tobjs->send_ctl, packet_out, 1);

    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));  /* No need to close stream yet */

    s = lsquic_stream_frame_in(stream, new_frame_in(tobjs, 0, 100, 0));
    assert(0 == s);

    n = lsquic_stream_read(stream, buf, 60);
    assert(60 == n);
    n = lsquic_stream_read(stream, buf, 60);
    assert(40 == n);

    s = lsquic_stream_frame_in(stream, new_frame_in(tobjs, 100, 0, 1));
    assert(0 == s);
    n = lsquic_stream_read(stream, buf, 60);
    assert(0 == n);
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));

    s = lsquic_stream_shutdown(stream, 0);
    assert(0 == s);
    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & (STREAM_SERVICE_FLAGS))
                                == (STREAM_CALL_ONCLOSE));
    ack_packet(&tobjs->send_ctl, 1);
    ack_packet(&tobjs->send_ctl, 2);
    assert((stream->stream_flags & (STREAM_SERVICE_FLAGS))
                                == (STREAM_CALL_ONCLOSE|STREAM_FREE_STREAM));

    lsquic_stream_destroy(stream);
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));

    assert(100 == tobjs->conn_pub.cfcw.cf_max_recv_off);
    assert(100 == tobjs->conn_pub.cfcw.cf_read_off);
}


/* Server: we read data and FIN, and then send data and FIN.
 */
static void
test_rem_FIN_loc_FIN (struct test_objs *tobjs)
{
    lsquic_stream_t *stream;
    char buf_out[0x100];
    unsigned char buf[0x100];
    size_t n;
    int s, fin;
    lsquic_packet_out_t *packet_out;

    stream = new_stream(tobjs, 345);

    s = lsquic_stream_frame_in(stream, new_frame_in(tobjs, 0, 100, 0));
    assert(0 == s);

    n = lsquic_stream_read(stream, buf, 60);
    assert(60 == n);
    n = lsquic_stream_read(stream, buf, 60);
    assert(40 == n);

    s = lsquic_stream_frame_in(stream, new_frame_in(tobjs, 100, 0, 1));
    assert(0 == s);
    n = lsquic_stream_read(stream, buf, 60);
    assert(0 == n);
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));

    s = lsquic_stream_shutdown(stream, 0);
    assert(0 == s);
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));

    init_buf(buf_out, sizeof(buf_out));
    n = lsquic_stream_write(stream, buf_out, 100);
    assert(n == 100);
    assert(0 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    s = lsquic_stream_flush(stream);
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    n = read_from_scheduled_packets(&tobjs->send_ctl, stream->id, buf,
                                                    sizeof(buf), 0, &fin, 0);
    assert(100 == n);
    assert(0 == memcmp(buf_out, buf, 100));
    assert(!fin);

    /* Pretend we sent out a packet: */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs->send_ctl);
    lsquic_send_ctl_sent_packet(&tobjs->send_ctl, packet_out, 1);

    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));  /* No need to close stream yet */

    s = lsquic_stream_shutdown(stream, 1);
    assert(s == 0);
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl)); /* Shutdown performs a flush */

    /* Now we can call on_close: */
    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & (STREAM_SERVICE_FLAGS))
                                            == STREAM_CALL_ONCLOSE);

    n = read_from_scheduled_packets(&tobjs->send_ctl, stream->id, buf,
                                                sizeof(buf), 100, &fin, 0);
    assert(0 == n);
    assert(fin);

    /* Pretend we sent out this packet as well: */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs->send_ctl);
    lsquic_send_ctl_sent_packet(&tobjs->send_ctl, packet_out, 1);

    /* Cannot free stream yet: packets have not been acked */
    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & (STREAM_SERVICE_FLAGS))
                                            == STREAM_CALL_ONCLOSE);

    ack_packet(&tobjs->send_ctl, 1);
    ack_packet(&tobjs->send_ctl, 2);

    /* Now we can free the stream: */
    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & (STREAM_SERVICE_FLAGS))
                                == (STREAM_CALL_ONCLOSE|STREAM_FREE_STREAM));

    lsquic_stream_destroy(stream);
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));

    assert(100 == tobjs->conn_pub.cfcw.cf_max_recv_off);
    assert(100 == tobjs->conn_pub.cfcw.cf_read_off);
}


/* Server: we read data and close the read side before reading FIN, which
 * DOES NOT result in stream being reset.
 */
static void
test_rem_data_loc_close (struct test_objs *tobjs)
{
    lsquic_stream_t *stream;
    char buf[0x100];
    ssize_t n;
    int s;

    stream = new_stream(tobjs, 345);

    s = lsquic_stream_frame_in(stream, new_frame_in(tobjs, 0, 100, 0));
    assert(0 == s);

    n = lsquic_stream_read(stream, buf, 60);
    assert(60 == n);

    s = lsquic_stream_shutdown(stream, 0);
    assert(0 == s);
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert(!((stream->stream_flags & (STREAM_SERVICE_FLAGS))
                                            == STREAM_CALL_ONCLOSE));

    n = lsquic_stream_read(stream, buf, 60);
    assert(n == -1);    /* Cannot read from closed stream */

    /* Close write side */
    s = lsquic_stream_shutdown(stream, 1);
    assert(0 == s);

    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl)); /* Shutdown performs a flush */

    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & (STREAM_SERVICE_FLAGS))
                                            == STREAM_CALL_ONCLOSE);

    s = lsquic_stream_rst_in(stream, 100, 1);
    assert(0 == s);

    lsquic_stream_destroy(stream);
    /* This simply checks that the stream got removed from the queue: */
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));

    assert(100 == tobjs->conn_pub.cfcw.cf_max_recv_off);
    assert(100 == tobjs->conn_pub.cfcw.cf_read_off);
}



/* Client: we send some data and FIN, but remote end sends some data and
 * then resets the stream.  The client gets an error when it reads from
 * stream, after which it closes and destroys the stream.
 */
static void
test_loc_FIN_rem_RST (struct test_objs *tobjs)
{
    lsquic_packet_out_t *packet_out;
    lsquic_stream_t *stream;
    char buf_out[0x100];
    unsigned char buf[0x100];
    ssize_t n;
    int s, fin;

    init_buf(buf_out, sizeof(buf_out));

    stream = new_stream(tobjs, 345);
    n = lsquic_stream_write(stream, buf_out, 100);
    assert(n == 100);
    assert(0 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    s = lsquic_stream_flush(stream);
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    n = read_from_scheduled_packets(&tobjs->send_ctl, stream->id, buf,
                                                    sizeof(buf), 0, &fin, 0);
    assert(100 == n);
    assert(0 == memcmp(buf_out, buf, 100));
    assert(!fin);

    /* Pretend we sent out a packet: */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs->send_ctl);
    lsquic_send_ctl_sent_packet(&tobjs->send_ctl, packet_out, 1);

    s = lsquic_stream_shutdown(stream, 1);
    assert(s == 0);
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl)); /* Shutdown performs a flush */
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));  /* No need to close stream yet */

    n = read_from_scheduled_packets(&tobjs->send_ctl, stream->id, buf,
                                                    sizeof(buf), 100, &fin, 0);
    assert(0 == n);
    assert(fin);

    /* Pretend we sent out this packet as well: */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs->send_ctl);
    lsquic_send_ctl_sent_packet(&tobjs->send_ctl, packet_out, 1);

    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));  /* No need to close stream yet */

    s = lsquic_stream_frame_in(stream, new_frame_in(tobjs, 0, 100, 0));
    assert(0 == s);
    s = lsquic_stream_rst_in(stream, 100, 0);
    assert(0 == s);

    /* No RST to send, we already sent FIN */
    assert(0 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    /* The stream is not yet done: the user code has not closed it yet */
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert(0 == (stream->stream_flags & (STREAM_SERVICE_FLAGS)));
    assert(0 == (stream->stream_flags & STREAM_U_READ_DONE));

    s = lsquic_stream_read(stream, buf, sizeof(buf));
    assert(-1 == s);    /* Error collected */
    s = lsquic_stream_close(stream);
    assert(0 == s);     /* Stream closed successfully */

    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & (STREAM_SERVICE_FLAGS))
                                == (STREAM_CALL_ONCLOSE));

    ack_packet(&tobjs->send_ctl, 1);
    ack_packet(&tobjs->send_ctl, 2);

    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & (STREAM_SERVICE_FLAGS))
                                == (STREAM_CALL_ONCLOSE|STREAM_FREE_STREAM));

    lsquic_stream_destroy(stream);
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));

    assert(100 == tobjs->conn_pub.cfcw.cf_max_recv_off);
    assert(100 == tobjs->conn_pub.cfcw.cf_read_off);
}


/* Client: we send some data (no FIN), and remote end sends some data and
 * then resets the stream.
 */
static void
test_loc_data_rem_RST (struct test_objs *tobjs)
{
    lsquic_packet_out_t *packet_out;
    lsquic_stream_t *stream;
    char buf_out[0x100];
    unsigned char buf[0x100];
    ssize_t n;
    int s, fin;

    init_buf(buf_out, sizeof(buf_out));

    stream = new_stream(tobjs, 345);
    n = lsquic_stream_write(stream, buf_out, 100);
    assert(n == 100);
    assert(0 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    s = lsquic_stream_flush(stream);
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    n = read_from_scheduled_packets(&tobjs->send_ctl, stream->id, buf,
                                                    sizeof(buf), 0, &fin, 0);
    assert(100 == n);
    assert(0 == memcmp(buf_out, buf, 100));
    assert(!fin);

    /* Pretend we sent out a packet: */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs->send_ctl);
    lsquic_send_ctl_sent_packet(&tobjs->send_ctl, packet_out, 1);

    s = lsquic_stream_frame_in(stream, new_frame_in(tobjs, 0, 100, 0));
    assert(0 == s);
    s = lsquic_stream_rst_in(stream, 200, 0);
    assert(0 == s);

    ack_packet(&tobjs->send_ctl, 1);

    assert(!TAILQ_EMPTY(&tobjs->conn_pub.sending_streams));
    assert((stream->stream_flags & STREAM_SENDING_FLAGS)
                                            == STREAM_SEND_RST);

    /* Not yet closed: error needs to be collected */
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert(0 == (stream->stream_flags & STREAM_SERVICE_FLAGS));

    n = lsquic_stream_write(stream, buf, 100);
    assert(-1 == n);    /* Error collected */
    s = lsquic_stream_close(stream);
    assert(0 == s);     /* Stream successfully closed */

    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & STREAM_SERVICE_FLAGS)
                                        == STREAM_CALL_ONCLOSE);

    lsquic_stream_rst_frame_sent(stream);
    lsquic_stream_call_on_close(stream);

    assert(TAILQ_EMPTY(&tobjs->conn_pub.sending_streams));
    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & STREAM_SERVICE_FLAGS)
                                        == STREAM_FREE_STREAM);

    lsquic_stream_destroy(stream);
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));

    assert(200 == tobjs->conn_pub.cfcw.cf_max_recv_off);
    assert(200 == tobjs->conn_pub.cfcw.cf_read_off);
}


/* We send some data and RST, receive data and FIN
 */
static void
test_loc_RST_rem_FIN (struct test_objs *tobjs)
{
    lsquic_packet_out_t *packet_out;
    lsquic_stream_t *stream;
    char buf_out[0x100];
    unsigned char buf[0x100];
    size_t n;
    int s, fin;

    init_buf(buf_out, sizeof(buf_out));

    stream = new_stream(tobjs, 345);

    n = lsquic_stream_write(stream, buf_out, 100);
    assert(n == 100);
    assert(0 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    s = lsquic_stream_flush(stream);
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    n = read_from_scheduled_packets(&tobjs->send_ctl, stream->id, buf,
                                                    sizeof(buf), 0, &fin, 0);
    assert(100 == n);
    assert(0 == memcmp(buf_out, buf, 100));
    assert(!fin);

    /* Pretend we sent out a packet: */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs->send_ctl);
    lsquic_send_ctl_sent_packet(&tobjs->send_ctl, packet_out, 1);

    assert(1 == stream->n_unacked);
    ack_packet(&tobjs->send_ctl, 1);
    assert(0 == stream->n_unacked);

    lsquic_stream_reset(stream, 0);
    assert(!TAILQ_EMPTY(&tobjs->conn_pub.sending_streams));
    assert((stream->stream_flags & STREAM_SENDING_FLAGS)
                                            == STREAM_SEND_RST);

    s = lsquic_stream_frame_in(stream, new_frame_in(tobjs, 0, 90, 1));
    assert(s == 0);
    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & STREAM_SERVICE_FLAGS)
                                        == STREAM_CALL_ONCLOSE);

    lsquic_stream_rst_frame_sent(stream);
    lsquic_stream_call_on_close(stream);

    assert(TAILQ_EMPTY(&tobjs->conn_pub.sending_streams));
    assert(!TAILQ_EMPTY(&tobjs->conn_pub.service_streams));
    assert((stream->stream_flags & STREAM_SERVICE_FLAGS)
                                        == STREAM_FREE_STREAM);

    lsquic_stream_destroy(stream);
    assert(TAILQ_EMPTY(&tobjs->conn_pub.service_streams));

    assert(90 == tobjs->conn_pub.cfcw.cf_max_recv_off);
    assert(90 == tobjs->conn_pub.cfcw.cf_read_off);
}


/* Write data to the stream, but do not flush: connection cap take a hit.
 * After stream is destroyed, connection cap should go back up.
 */
static void
test_reset_stream_with_unflushed_data (struct test_objs *tobjs)
{
    lsquic_stream_t *stream;
    char buf[0x100];
    size_t n;
    const struct lsquic_conn_cap *const cap = &tobjs->conn_pub.conn_cap;

    assert(0x4000 == lsquic_conn_cap_avail(cap));   /* Self-check */
    stream = new_stream(tobjs, 345);
    n = lsquic_stream_write(stream, buf, 100);
    assert(n == 100);

    /* Unflushed data counts towards connection cap for connection-limited
     * stream:
     */
    assert(0x4000 - 100 == lsquic_conn_cap_avail(cap));

    lsquic_stream_destroy(stream);
    assert(0x4000 == lsquic_conn_cap_avail(cap));   /* Goes back up */
}


/* Write a little data to the stream, flush and then reset it: connection
 * cap should NOT go back up.
 */
static void
test_reset_stream_with_flushed_data (struct test_objs *tobjs)
{
    char buf[0x100];
    size_t n;
    lsquic_stream_t *stream;
    const struct lsquic_conn_cap *const cap = &tobjs->conn_pub.conn_cap;

    assert(0x4000 == lsquic_conn_cap_avail(cap));   /* Self-check */
    stream = new_stream(tobjs, 345);
    n = lsquic_stream_write(stream, buf, 100);
    assert(n == 100);

    /* Unflushed data counts towards connection cap for
     * connection-limited stream:
     */
    assert(0x4000 - 100 == lsquic_conn_cap_avail(cap));

    /* Flush the stream: */
    lsquic_stream_flush(stream);
    assert(0x4000 - 100 == lsquic_conn_cap_avail(cap));

    lsquic_stream_destroy(stream);
    assert(0x4000 - 100 == lsquic_conn_cap_avail(cap));   /* Still unchanged */
}


/* Write data to the handshake stream and flush: this should not affect
 * connection cap.
 */
static void
test_unlimited_stream_flush_data (struct test_objs *tobjs)
{
    char buf[0x100];
    size_t n;
    lsquic_stream_t *stream;
    const struct lsquic_conn_cap *const cap = &tobjs->conn_pub.conn_cap;

    assert(0x4000 == lsquic_conn_cap_avail(cap));   /* Self-check */
    stream = new_stream(tobjs, LSQUIC_STREAM_HANDSHAKE);
    n = lsquic_stream_write(stream, buf, 100);
    assert(n == 100);

    /* We DO NOT take connection cap hit after stream is flushed: */
    lsquic_stream_flush(stream);
    assert(0x4000 == lsquic_conn_cap_avail(cap));

    lsquic_stream_reset(stream, 0xF00DF00D);
    assert(0x4000 == lsquic_conn_cap_avail(cap));   /* Still unchanged */

    lsquic_stream_destroy(stream);
    assert(0x4000 == lsquic_conn_cap_avail(cap));   /* Still unchanged */
}


/* Test that data gets flushed when stream is closed. */
static void
test_data_flush_on_close (struct test_objs *tobjs)
{
    lsquic_stream_t *stream;
    const struct lsquic_conn_cap *const cap = &tobjs->conn_pub.conn_cap;
    char buf[0x100];
    size_t n;

    assert(0x4000 == lsquic_conn_cap_avail(cap));   /* Self-check */
    stream = new_stream(tobjs, 345);
    n = lsquic_stream_write(stream, buf, 100);
    assert(n == 100);
    assert(0 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    lsquic_stream_close(stream);
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs->send_ctl));

    /* We take connection cap hit after stream is flushed: */
    assert(0x4000 - 100 == lsquic_conn_cap_avail(cap)); /* Conn cap hit */

    lsquic_stream_destroy(stream);
}


/* In this function, we test stream termination conditions.  In particular,
 * we are interested in when the stream becomes finished (this is when
 * connection closes it and starts ignoring frames that come after this):
 * we need to test the following scenarios, both normal and abnormal
 * termination, initiated both locally and remotely.
 *
 * We avoid formalities like calling wantread() and wantwrite() and
 * dispatching read and write callbacks.
 */
static void
test_termination (void)
{
    struct test_objs tobjs;
    unsigned i;
    void (*const test_funcs[])(struct test_objs *) = {
        test_loc_FIN_rem_FIN,
        test_rem_FIN_loc_FIN,
        test_rem_data_loc_close,
        test_loc_FIN_rem_RST,
        test_loc_data_rem_RST,
        test_loc_RST_rem_FIN,
    };

    for (i = 0; i < sizeof(test_funcs) / sizeof(test_funcs[0]); ++i)
    {
        init_test_ctl_settings(&g_ctl_settings);
        g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;
        init_test_objs(&tobjs, 0x4000, 0x4000);
        test_funcs[i](&tobjs);
        deinit_test_objs(&tobjs);
    }
}


/* Test flush-related corner cases */
static void
test_flushing (void)
{
    struct test_objs tobjs;
    unsigned i;
    void (*const test_funcs[])(struct test_objs *) = {
        test_reset_stream_with_unflushed_data,
        test_reset_stream_with_flushed_data,
        test_unlimited_stream_flush_data,
        test_data_flush_on_close,
    };

    for (i = 0; i < sizeof(test_funcs) / sizeof(test_funcs[0]); ++i)
    {
        init_test_objs(&tobjs, 0x4000, 0x4000);
        test_funcs[i](&tobjs);
        deinit_test_objs(&tobjs);
    }
}


static void
test_writev (void)
{
    unsigned i;
    struct test_objs tobjs;
    lsquic_stream_t *stream;
    ssize_t n;
    unsigned char buf_in[0x4000];
    unsigned char buf_out[0x4000];
    int fin;

    struct {
        struct iovec iov[0x20];
        int          count;
    } tests[] = {
        { .iov  = {
            { .iov_base = buf_in, .iov_len  = 0x4000, },
          },
          .count = 1,
        },
        { .iov  = {
            { .iov_base = buf_in         , .iov_len  = 0x1000, },
            { .iov_base = buf_in + 0x1000, .iov_len  = 0x3000, },
          },
          .count = 2,
        },
        { .iov  = {
            { .iov_base = buf_in         , .iov_len  = 0x1000, },
            { .iov_base = buf_in + 0x1000, .iov_len  = 0x1000, },
            { .iov_base = buf_in + 0x2000, .iov_len  = 0x1000, },
            { .iov_base = buf_in + 0x3000, .iov_len  = 0x1000, },
          },
          .count = 4,
        },
        { .iov  = {
            { .iov_base = buf_in         , .iov_len  = 0x1000, },
            { .iov_base = buf_in + 0x1000, .iov_len  = 0x1000, },
            { .iov_base = buf_in + 0x2000, .iov_len  = 0x1000, },
            { .iov_base = buf_in + 0x3000, .iov_len  = 0xFF0,  },
            { .iov_base = buf_in + 0x3FF0, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FF1, .iov_len  = 0,      },
            { .iov_base = buf_in + 0x3FF1, .iov_len  = 0,      },
            { .iov_base = buf_in + 0x3FF1, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FF2, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FF3, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FF4, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FF5, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FF6, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FF7, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FF8, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FF9, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FFA, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FFB, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FFC, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FFD, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FFE, .iov_len  = 1,      },
            { .iov_base = buf_in + 0x3FFF, .iov_len  = 1,      },
          },
          .count = 22,
        },
    };

    memset(buf_in,          'A', 0x1000);
    memset(buf_in + 0x1000, 'B', 0x1000);
    memset(buf_in + 0x2000, 'C', 0x1000);
    memset(buf_in + 0x3000, 'D', 0x1000);

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    {
        init_test_objs(&tobjs, UINT_MAX, UINT_MAX);
        stream = new_stream(&tobjs, 12345);
        n = lsquic_stream_writev(stream, tests[i].iov, tests[i].count);
        assert(0x4000 == n);
        lsquic_stream_flush(stream);
        n = read_from_scheduled_packets(&tobjs.send_ctl, stream->id, buf_out,
                                                sizeof(buf_out), 0, &fin, 0);
        assert(0x4000 == n);
        assert(0 == memcmp(buf_out, buf_in, 0x1000));
        assert(!fin);
        lsquic_stream_destroy(stream);
        deinit_test_objs(&tobjs);
    }
}


static void
test_prio_conversion (void)
{
    struct test_objs tobjs;
    lsquic_stream_t *stream;
    unsigned prio;
    int s;

    init_test_objs(&tobjs, UINT_MAX, UINT_MAX);
    stream = new_stream(&tobjs, 123);

    s = lsquic_stream_set_priority(stream, -2);
    assert(-1 == s);
    s = lsquic_stream_set_priority(stream, 0);
    assert(-1 == s);
    s = lsquic_stream_set_priority(stream, 257);
    assert(-1 == s);

    for (prio = 1; prio <= 256; ++prio)
    {
        s = lsquic_stream_set_priority(stream, prio);
        assert(0 == s);
        assert(prio == lsquic_stream_priority(stream));
    }

    lsquic_stream_destroy(stream);
    deinit_test_objs(&tobjs);
}


static void
test_read_in_middle (void)
{
    int s;
    size_t nw = 0;
    char buf[0x1000];
    const char data[] = "AAABBBCCC";
    struct test_objs tobjs;
    stream_frame_t *frame;

    init_test_objs(&tobjs, 0x4000, 0x4000);

    lsquic_stream_t *stream = new_stream(&tobjs, 123);

    frame = new_frame_in_ext(&tobjs, 0, 3, 0, &data[0]);
    s = lsquic_stream_frame_in(stream, frame);
    assert(0 == s);

    /* Hole */

    frame = new_frame_in_ext(&tobjs, 6, 3, 0, &data[6]);
    s = lsquic_stream_frame_in(stream, frame);
    assert(0 == s);

    /* Read up to hole */

    nw = lsquic_stream_read(stream, buf, sizeof(buf));
    assert(3 == nw);
    assert(0 == memcmp(buf, "AAA", 3));

    frame = new_frame_in_ext(&tobjs, 3, 3, 0, &data[3]);
    s = lsquic_stream_frame_in(stream, frame);
    assert(0 == s);

    nw = lsquic_stream_read(stream, buf, sizeof(buf));
    assert(6 == nw);
    assert(0 == memcmp(buf, "BBBCCC", 6));

    lsquic_stream_destroy(stream);
    deinit_test_objs(&tobjs);
}


/* Test that connection flow control does not go past the max when both
 * connection limited and unlimited streams are used.
 */
static void
test_conn_unlimited (void)
{
    size_t nw;
    struct test_objs tobjs;
    lsquic_stream_t *header_stream, *data_stream;

    init_test_objs(&tobjs, 0x4000, 0x4000);

    unsigned char *const data = calloc(1, 0x4000);

    /* Test 1: first write headers, then data stream */
    header_stream = new_stream(&tobjs, LSQUIC_STREAM_HANDSHAKE);
    data_stream = new_stream(&tobjs, 123);
    nw = lsquic_stream_write(header_stream, data, 98);
    assert(98 == nw);
    lsquic_stream_flush(header_stream);
    nw = lsquic_stream_write(data_stream, data, 0x4000);
    assert(0x4000 == nw);
    assert(tobjs.conn_pub.conn_cap.cc_sent <= tobjs.conn_pub.conn_cap.cc_max);
    lsquic_stream_destroy(header_stream);
    lsquic_stream_destroy(data_stream);

    /* Test 2: first write data, then headers stream */
    header_stream = new_stream(&tobjs, LSQUIC_STREAM_HANDSHAKE);
    data_stream = new_stream(&tobjs, 123);
    lsquic_conn_cap_init(&tobjs.conn_pub.conn_cap, 0x4000);
    nw = lsquic_stream_write(data_stream, data, 0x4000);
    assert(0x4000 == nw);
    nw = lsquic_stream_write(header_stream, data, 98);
    assert(98 == nw);
    lsquic_stream_flush(header_stream);
    assert(tobjs.conn_pub.conn_cap.cc_sent <= tobjs.conn_pub.conn_cap.cc_max);

    lsquic_stream_destroy(header_stream);
    lsquic_stream_destroy(data_stream);

    deinit_test_objs(&tobjs);
    free(data);
}


static void
test_reading_from_stream2 (void)
{
    struct test_objs tobjs;
    char buf[0x1000];
    struct iovec iov[2];
    lsquic_packet_in_t *packet_in;
    lsquic_stream_t *stream;
    stream_frame_t *frame;
    ssize_t nw;
    int s;
    const char data[] = "1234567890";

    init_test_objs(&tobjs, 0x4000, 0x4000);
    stream = new_stream(&tobjs, 123);

    frame = new_frame_in_ext(&tobjs, 0, 6, 0, &data[0]);
    s = lsquic_stream_frame_in(stream, frame);
    assert(("Inserted frame #1", 0 == s));

    frame = new_frame_in_ext(&tobjs, 6, 4, 0, &data[6]);
    s = lsquic_stream_frame_in(stream, frame);
    assert(("Inserted frame #2", 0 == s));

    /* Invalid frame: FIN in the middle */
    frame = new_frame_in(&tobjs, 6, 0, 1);
    s = lsquic_stream_frame_in(stream, frame);
    assert(("Invalid frame: FIN in the middle", -1 == s));

    /* Test for overlaps and DUPs: */
    if (!(stream_ctor_flags & SCF_USE_DI_HASH))
    {
        int dup;
        unsigned offset, length;
        for (offset = 0; offset < 9; ++offset)
        {
            for (length = 1; length < 10; ++length)
            {
                dup = (offset == 0 && length == 6)
                   || (offset == 6 && length == 4);
                frame = new_frame_in(&tobjs, offset, length, 0);
                s = lsquic_stream_frame_in(stream, frame);
                if (dup)
                    assert(("Dup OK", 0 == s));
                else
                    assert(("Invalid frame: overlap", -1 == s));
            }
        }
    }

    nw = lsquic_stream_read(stream, buf, 8);
    assert(("Read 8 bytes", nw == 8));
    assert(("Expected 8 bytes", 0 == memcmp(buf, "12345678", nw)));

    /* Insert invalid frame: its offset + length is before the already-read
     * offset.
     */
    frame = new_frame_in_ext(&tobjs, 0, 6, 0, &data[0]);
    packet_in = lsquic_packet_in_get(frame->packet_in); /* incref to check for dups below */
    assert(2 == packet_in->pi_refcnt);  /* Self-check */
    s = lsquic_stream_frame_in(stream, frame);
    assert(("Insert frame before already-read offset succeeds (duplicate)",
                                                                s == 0));
    assert(("Duplicate frame has been thrown out",
                                        packet_in->pi_refcnt == 1));
    lsquic_packet_in_put(&tobjs.eng_pub.enp_mm, packet_in);
    packet_in = NULL;

    iov[0].iov_base = buf;
    iov[0].iov_len  = 1;
    iov[1].iov_base = buf + 1;
    iov[1].iov_len  = sizeof(buf) - 1;
    nw = lsquic_stream_readv(stream, iov, 2);
    assert(("Read 2 bytes", nw == 2));
    assert(("Expected 2 bytes", 0 == memcmp(buf, "90", nw)));
    nw = lsquic_stream_read(stream, buf, 8);
    assert(("Read -1 bytes (EWOULDBLOCK)", -1 == nw && errno == EWOULDBLOCK));
    nw = lsquic_stream_read(stream, buf, 8);
    assert(("Read -1 bytes again (EWOULDBLOCK)", -1 == nw && errno == EWOULDBLOCK));

    /* Insert invalid frame: its offset + length is before the already-read
     * offset.  This test is different from before: now there is buffered
     * incoming data.
     */
    frame = new_frame_in_ext(&tobjs, 0, 6, 0, &data[0]);
    packet_in = lsquic_packet_in_get(frame->packet_in); /* incref to check for dups below */
    assert(2 == packet_in->pi_refcnt);  /* Self-check */
    s = lsquic_stream_frame_in(stream, frame);
    assert(("Insert frame before already-read offset succeeds (duplicate)",
                                                                s == 0));
    assert(("Duplicate frame has been thrown out",
                                        packet_in->pi_refcnt == 1));
    lsquic_packet_in_put(&tobjs.eng_pub.enp_mm, packet_in);
    packet_in = NULL;

    /* Last frame has no data but has a FIN flag set */
    frame = new_frame_in_ext(&tobjs, 10, 0, 1,
                (void *) 1234     /* Intentionally invalid: this pointer
                                   * should not be used
                                   */);
    s = lsquic_stream_frame_in(stream, frame);
    assert(("Inserted frame #3", 0 == s));

    /* Invalid frame: writing after FIN */
    frame = new_frame_in(&tobjs, 10, 2, 0);
    s = lsquic_stream_frame_in(stream, frame);
    assert(("Invalid frame caught", -1 == s));

    /* Duplicate FIN frame */
    frame = new_frame_in_ext(&tobjs, 10, 0, 1,
                (void *) 1234     /* Intentionally invalid: this pointer
                                   * should not be used
                                   */);
    s = lsquic_stream_frame_in(stream, frame);
    assert(("Duplicate FIN frame", 0 == s));

    nw = lsquic_stream_read(stream, buf, 1);
    assert(("Read 0 bytes (at EOR)", 0 == nw));

    lsquic_stream_destroy(stream);
    deinit_test_objs(&tobjs);
}


static void
test_writing_to_stream_schedule_stream_packets_immediately (void)
{
    ssize_t nw;
    struct test_objs tobjs;
    struct lsquic_conn *const lconn = &tobjs.lconn;
    struct lsquic_stream *stream;
    int s;
    unsigned char buf[0x1000];
    struct lsquic_conn_cap *const conn_cap = &tobjs.conn_pub.conn_cap;

    init_test_ctl_settings(&g_ctl_settings);
    g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;

    init_test_objs(&tobjs, 0x4000, 0x4000);
    n_closed = 0;
    stream = new_stream(&tobjs, 123);
    assert(("Stream initialized", stream));
    const struct test_ctx *const test_ctx_local  = tobjs.stream_if_ctx;
    assert(("on_new_stream called correctly", stream == test_ctx_local->stream));
    assert(LSQUIC_STREAM_DEFAULT_PRIO == lsquic_stream_priority(stream));

    assert(lconn == lsquic_stream_conn(stream));

    nw = lsquic_stream_write(stream, "Dude, where is", 14);
    assert(("14 bytes written correctly", nw == 14));

    assert(("not packetized",
                        0 == lsquic_send_ctl_n_scheduled(&tobjs.send_ctl)));
    /* Cap hit is taken immediately, even for flushed data */
    assert(("connection cap is reduced by 14 bytes",
                    lsquic_conn_cap_avail(conn_cap) == 0x4000 - 14));
    s = lsquic_stream_flush(stream);
    assert(0 == s);
    assert(("packetized -- 1 packet",
                        1 == lsquic_send_ctl_n_scheduled(&tobjs.send_ctl)));

    nw = lsquic_stream_write(stream, " my car?!", 9);
    assert(("9 bytes written correctly", nw == 9));
    s = lsquic_stream_flush(stream);
    assert(0 == s);
    assert(("packetized -- 2 packets now",
                        2 == lsquic_send_ctl_n_scheduled(&tobjs.send_ctl)));

    assert(("connection cap is reduced by 23 bytes",
                    lsquic_conn_cap_avail(conn_cap) == 0x4000 - 23));

    nw = read_from_scheduled_packets(&tobjs.send_ctl, stream->id, buf,
                                                    sizeof(buf), 0, NULL, 0);
    assert(23 == nw);
    assert(0 == memcmp(buf, "Dude, where is my car?!", 23));
    assert(("cannot reduce max_send below what's been sent already",
                            -1 == lsquic_stream_set_max_send_off(stream, 15)));
    assert(("cannot reduce max_send below what's been sent already #2",
                            -1 == lsquic_stream_set_max_send_off(stream, 22)));
    assert(("can set to the same value...",
                             0 == lsquic_stream_set_max_send_off(stream, 23)));
    assert(("...or larger",
                             0 == lsquic_stream_set_max_send_off(stream, 23000)));
    lsquic_stream_destroy(stream);
    assert(("on_close called", 1 == n_closed));
    deinit_test_objs(&tobjs);
}


static void
test_writing_to_stream_outside_callback (void)
{
    ssize_t nw;
    struct test_objs tobjs;
    struct lsquic_conn *const lconn = &tobjs.lconn;
    struct lsquic_stream *stream;
    int s;
    unsigned char buf[0x1000];
    struct lsquic_conn_cap *const conn_cap = &tobjs.conn_pub.conn_cap;

    init_test_ctl_settings(&g_ctl_settings);
    g_ctl_settings.tcs_schedule_stream_packets_immediately = 0;
    g_ctl_settings.tcs_bp_type = BPT_OTHER_PRIO;
    const struct buf_packet_q *const bpq =
            &tobjs.send_ctl.sc_buffered_packets[g_ctl_settings.tcs_bp_type];

    init_test_objs(&tobjs, 0x4000, 0x4000);
    n_closed = 0;
    stream = new_stream(&tobjs, 123);
    assert(("Stream initialized", stream));
    const struct test_ctx *const test_ctx_local = tobjs.stream_if_ctx;
    assert(("on_new_stream called correctly", stream == test_ctx_local->stream));
    assert(LSQUIC_STREAM_DEFAULT_PRIO == lsquic_stream_priority(stream));

    assert(lconn == lsquic_stream_conn(stream));

    nw = lsquic_stream_write(stream, "Dude, where is", 14);
    assert(("14 bytes written correctly", nw == 14));

    assert(("not packetized", 0 == bpq->bpq_count));
    s = lsquic_stream_flush(stream);
    assert(0 == s);
    assert(("packetized -- 1 packet", 1 == bpq->bpq_count));

    nw = lsquic_stream_write(stream, " my car?!", 9);
    assert(("9 bytes written correctly", nw == 9));
    s = lsquic_stream_flush(stream);
    assert(0 == s);
    assert(("packetized -- 2 packets now", 2 == bpq->bpq_count));

    assert(("connection cap is reduced by 23 bytes",
                    lsquic_conn_cap_avail(conn_cap) == 0x4000 - 23));

    /* Now we are magically inside the callback: */
    g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;
    lsquic_send_ctl_schedule_buffered(&tobjs.send_ctl,
                                                g_ctl_settings.tcs_bp_type);
    assert(("packetized -- 2 packets now",
                        2 == lsquic_send_ctl_n_scheduled(&tobjs.send_ctl)));

    nw = read_from_scheduled_packets(&tobjs.send_ctl, stream->id, buf,
                                                    sizeof(buf), 0, NULL, 0);
    assert(23 == nw);
    assert(0 == memcmp(buf, "Dude, where is my car?!", 23));
    assert(("cannot reduce max_send below what's been sent already",
                            -1 == lsquic_stream_set_max_send_off(stream, 15)));
    assert(("cannot reduce max_send below what's been sent already #2",
                            -1 == lsquic_stream_set_max_send_off(stream, 22)));
    assert(("can set to the same value...",
                             0 == lsquic_stream_set_max_send_off(stream, 23)));
    assert(("...or larger",
                             0 == lsquic_stream_set_max_send_off(stream, 23000)));
    lsquic_stream_destroy(stream);
    assert(("on_close called", 1 == n_closed));
    deinit_test_objs(&tobjs);
}


/* Test window update logic, connection-limited */
static void
test_window_update1 (void)
{
    ssize_t nw;
    struct test_objs tobjs;
    struct lsquic_stream *stream;
    unsigned char buf[0x1000];
    lsquic_packet_out_t *packet_out;
    struct lsquic_conn_cap *const conn_cap = &tobjs.conn_pub.conn_cap;
    int s;

    init_test_ctl_settings(&g_ctl_settings);
    g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;

    init_test_objs(&tobjs, 0x4000, 0x4000);
    n_closed = 0;
    stream = new_stream_ext(&tobjs, 123, 3);
    nw = lsquic_stream_write(stream, "1234567890", 10);
    assert(("lsquic_stream_write is limited by the send window", 3 == nw));
    assert(("cc_tosend is updated immediately",
                                            3 == conn_cap->cc_sent));
    s = lsquic_stream_flush(stream);
    assert(0 == s);
    assert(("cc_tosend is updated when limited by connection",
                                            3 == conn_cap->cc_sent));
    nw = read_from_scheduled_packets(&tobjs.send_ctl, stream->id, buf,
                                                    sizeof(buf), 0, NULL, 0);
    assert(nw == 3);
    assert(0 == memcmp(buf, "123", 3));

    /* Pretend we sent out a packet: */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs.send_ctl);
    lsquic_send_ctl_sent_packet(&tobjs.send_ctl, packet_out, 1);

    lsquic_stream_window_update(stream, 20);
    nw = lsquic_stream_write(stream, "4567890", 7);
    assert(("lsquic_stream_write: wrote remainig 7 bytes", 7 == nw));
    s = lsquic_stream_flush(stream);
    assert(0 == s);

    /* Verify written data: */
    nw = read_from_scheduled_packets(&tobjs.send_ctl, stream->id, buf,
                                                    sizeof(buf), 3, NULL, 0);
    assert(nw == 7);
    assert(0 == memcmp(buf, "4567890", 7));

    lsquic_stream_destroy(stream);
    assert(("on_close called", 1 == n_closed));
    deinit_test_objs(&tobjs);
}


/* Test two: large frame in the middle -- it is the one that is moved out
 * into new packet.
 */
static void
test_bad_packbits_guess_2 (void)
{
    lsquic_packet_out_t *packet_out;
    ssize_t nw;
    struct test_objs tobjs;
    struct lsquic_stream *streams[3];
    char buf[0x1000];
    unsigned char buf_out[0x1000];
    int s, fin;

    init_buf(buf, sizeof(buf));

    init_test_ctl_settings(&g_ctl_settings);
    g_ctl_settings.tcs_schedule_stream_packets_immediately = 0;
    g_ctl_settings.tcs_guess_packno_bits = PACKNO_LEN_1;

    init_test_objs(&tobjs, 0x1000, 0x1000);
    streams[0] = new_stream(&tobjs, 5);
    streams[1] = new_stream(&tobjs, 7);
    streams[2] = new_stream(&tobjs, 9);

    /* Perfrom writes on the three streams.  This is tuned to fill a single
     * packet completely -- we check this later in this function.
     */
    s = lsquic_stream_shutdown(streams[0], 1);
    assert(s == 0);
    nw = lsquic_stream_write(streams[1], buf, 1337);
    assert(nw == 1337);
    s = lsquic_stream_flush(streams[1]);
    assert(0 == s);
    nw = lsquic_stream_write(streams[2], buf + 1337, 1);
    assert(nw == 1);
    s = lsquic_stream_shutdown(streams[2], 1);
    assert(s == 0);

    /* Verify that we got one packet filled to the top: */
    const struct buf_packet_q *const bpq =
            &tobjs.send_ctl.sc_buffered_packets[g_ctl_settings.tcs_bp_type];
    assert(("packetized -- 1 packet", 1 == bpq->bpq_count));
    packet_out = TAILQ_FIRST(&bpq->bpq_packets);
    assert(0 == lsquic_packet_out_avail(packet_out));

    assert(1 == streams[0]->n_unacked);
    assert(1 == streams[1]->n_unacked);
    assert(1 == streams[2]->n_unacked);

    g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;
    g_ctl_settings.tcs_calc_packno_bits = PACKNO_LEN_6;
    s = lsquic_send_ctl_schedule_buffered(&tobjs.send_ctl,
                                                g_ctl_settings.tcs_bp_type);
    assert(2 == lsquic_send_ctl_n_scheduled(&tobjs.send_ctl));

    /* Verify written data: */
    nw = read_from_scheduled_packets(&tobjs.send_ctl, streams[0]->id, buf_out,
                                                sizeof(buf_out), 0, &fin, 0);
    assert(nw == 0);
    assert(fin);
    nw = read_from_scheduled_packets(&tobjs.send_ctl, streams[1]->id, buf_out,
                                                sizeof(buf_out), 0, &fin, 0);
    assert(nw == 1337);
    assert(!fin);
    assert(0 == memcmp(buf, buf_out, 1337));
    nw = read_from_scheduled_packets(&tobjs.send_ctl, streams[2]->id, buf_out,
                                                sizeof(buf_out), 0, &fin, 0);
    assert(nw == 1);
    assert(fin);
    assert(0 == memcmp(buf + 1337, buf_out, 1));

    /* Verify packets */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs.send_ctl);
    assert(lsquic_packet_out_packno_bits(packet_out) == PACKNO_LEN_6);
    assert(1 == packet_out->po_packno);
    assert(packet_out->po_frame_types & (1 << QUIC_FRAME_STREAM));
    lsquic_send_ctl_sent_packet(&tobjs.send_ctl, packet_out, 1);
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs.send_ctl);
    assert(lsquic_packet_out_packno_bits(packet_out) == PACKNO_LEN_6);
    assert(2 == packet_out->po_packno);
    assert(packet_out->po_frame_types & (1 << QUIC_FRAME_STREAM));
    lsquic_send_ctl_sent_packet(&tobjs.send_ctl, packet_out, 1);

    assert(1 == streams[0]->n_unacked);
    assert(1 == streams[1]->n_unacked);
    assert(1 == streams[2]->n_unacked);
    ack_packet(&tobjs.send_ctl, 1);
    assert(0 == streams[0]->n_unacked);
    assert(1 == streams[1]->n_unacked);
    assert(0 == streams[2]->n_unacked);
    ack_packet(&tobjs.send_ctl, 2);
    assert(0 == streams[0]->n_unacked);
    assert(0 == streams[1]->n_unacked);
    assert(0 == streams[2]->n_unacked);

    lsquic_stream_destroy(streams[0]);
    lsquic_stream_destroy(streams[1]);
    lsquic_stream_destroy(streams[2]);
    deinit_test_objs(&tobjs);
}


/* Test three: split large STREAM frame into two halves.  The second half
 * goes into new packet.
 */
static void
test_bad_packbits_guess_3 (void)
{
    lsquic_packet_out_t *packet_out;
    ssize_t nw;
    struct test_objs tobjs;
    struct lsquic_stream *streams[1];
    char buf[0x1000];
    unsigned char buf_out[0x1000];
    int s, fin;

    init_buf(buf, sizeof(buf));

    init_test_ctl_settings(&g_ctl_settings);
    g_ctl_settings.tcs_schedule_stream_packets_immediately = 0;
    g_ctl_settings.tcs_guess_packno_bits = PACKNO_LEN_1;

    init_test_objs(&tobjs, 0x1000, 0x1000);
    streams[0] = new_stream(&tobjs, 5);

    nw = lsquic_stream_write(streams[0], buf,
                /* Use odd number to test halving logic: */ 1343);
    assert(nw == 1343);
    s = lsquic_stream_shutdown(streams[0], 1);
    assert(s == 0);

    /* Verify that we got one packet filled to the top (minus one byte) */
    const struct buf_packet_q *const bpq =
            &tobjs.send_ctl.sc_buffered_packets[g_ctl_settings.tcs_bp_type];
    assert(("packetized -- 1 packet", 1 == bpq->bpq_count));
    packet_out = TAILQ_FIRST(&bpq->bpq_packets);
    assert(1 == lsquic_packet_out_avail(packet_out));

    assert(1 == streams[0]->n_unacked);

    g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;
    g_ctl_settings.tcs_calc_packno_bits = PACKNO_LEN_4;
    s = lsquic_send_ctl_schedule_buffered(&tobjs.send_ctl,
                                                g_ctl_settings.tcs_bp_type);
    assert(2 == lsquic_send_ctl_n_scheduled(&tobjs.send_ctl));

    /* Verify written data: */
    nw = read_from_scheduled_packets(&tobjs.send_ctl, streams[0]->id, buf_out,
                                                sizeof(buf_out), 0, &fin, 0);
    assert(nw == 1343);
    assert(fin);
    assert(0 == memcmp(buf, buf_out, 1343));

    /* Verify packets */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs.send_ctl);
    assert(lsquic_packet_out_packno_bits(packet_out) == PACKNO_LEN_4);
    assert(1 == packet_out->po_packno);
    assert(packet_out->po_frame_types & (1 << QUIC_FRAME_STREAM));
    lsquic_send_ctl_sent_packet(&tobjs.send_ctl, packet_out, 1);
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs.send_ctl);
    assert(lsquic_packet_out_packno_bits(packet_out) == PACKNO_LEN_4);
    assert(2 == packet_out->po_packno);
    assert(packet_out->po_frame_types & (1 << QUIC_FRAME_STREAM));
    lsquic_send_ctl_sent_packet(&tobjs.send_ctl, packet_out, 1);

    assert(2 == streams[0]->n_unacked);
    ack_packet(&tobjs.send_ctl, 1);
    assert(1 == streams[0]->n_unacked);
    ack_packet(&tobjs.send_ctl, 2);
    assert(0 == streams[0]->n_unacked);

    lsquic_stream_destroy(streams[0]);
    deinit_test_objs(&tobjs);
}


struct packetization_test_stream_ctx
{
    const unsigned char    *buf;
    unsigned                len, off, write_size;
};


static lsquic_stream_ctx_t *
packetization_on_new_stream (void *stream_if_ctx, lsquic_stream_t *stream)
{
    lsquic_stream_wantwrite(stream, 1);
    return stream_if_ctx;
}


static void
packetization_on_close (lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
}


#define RANDOM_WRITE_SIZE ~0U

static unsigned
calc_n_to_write (unsigned write_size)
{
    if (write_size == RANDOM_WRITE_SIZE)
        return rand() % 1000 + 1;
    else
        return write_size;
}


static void
packetization_write_as_much_as_you_can (lsquic_stream_t *stream,
                                         lsquic_stream_ctx_t *ctx)
{
    struct packetization_test_stream_ctx *const pack_ctx = (void *) ctx;
    unsigned n_to_write;
    ssize_t n_written;

    while (pack_ctx->off < pack_ctx->len)
    {
        n_to_write = calc_n_to_write(pack_ctx->write_size);
        if (n_to_write > pack_ctx->len - pack_ctx->off)
            n_to_write = pack_ctx->len - pack_ctx->off;
        n_written = lsquic_stream_write(stream, pack_ctx->buf + pack_ctx->off,
                                        n_to_write);
        assert(n_written >= 0);
        if (n_written == 0)
            break;
        pack_ctx->off += n_written;
    }

    lsquic_stream_wantwrite(stream, 0);
}


static void
packetization_perform_one_write (lsquic_stream_t *stream,
                                         lsquic_stream_ctx_t *ctx)
{
    struct packetization_test_stream_ctx *const pack_ctx = (void *) ctx;
    unsigned n_to_write;
    ssize_t n_written;

    n_to_write = calc_n_to_write(pack_ctx->write_size);
    if (n_to_write > pack_ctx->len - pack_ctx->off)
        n_to_write = pack_ctx->len - pack_ctx->off;
    n_written = lsquic_stream_write(stream, pack_ctx->buf + pack_ctx->off,
                                    n_to_write);
    assert(n_written >= 0);
    pack_ctx->off += n_written;
    if (n_written == 0)
        lsquic_stream_wantwrite(stream, 0);
}


static const struct lsquic_stream_if packetization_inside_once_stream_if = {
    .on_new_stream          = packetization_on_new_stream,
    .on_close               = packetization_on_close,
    .on_write               = packetization_write_as_much_as_you_can,
};


static const struct lsquic_stream_if packetization_inside_many_stream_if = {
    .on_new_stream          = packetization_on_new_stream,
    .on_close               = packetization_on_close,
    .on_write               = packetization_perform_one_write,
};


static void
test_packetization (int schedule_stream_packets_immediately, int dispatch_once,
                    unsigned write_size, unsigned first_stream_sz)
{
    struct test_objs tobjs;
    struct lsquic_stream *streams[2];
    size_t nw;
    int fin;
    unsigned char buf[0x8000];
    unsigned char buf_out[0x8000];

    struct packetization_test_stream_ctx packet_stream_ctx =
    {
        .buf = buf,
        .off = 0,
        .len = sizeof(buf),
        .write_size = write_size,
    };

    init_buf(buf, sizeof(buf));

    init_test_ctl_settings(&g_ctl_settings);
    g_ctl_settings.tcs_schedule_stream_packets_immediately = schedule_stream_packets_immediately;

    init_test_objs(&tobjs,
        /* Test limits a bit while we are at it: */
        sizeof(buf) - 1, sizeof(buf) - 1);
    tobjs.stream_if_ctx = &packet_stream_ctx;

    if (schedule_stream_packets_immediately)
    {
        if (dispatch_once)
        {
            tobjs.stream_if = &packetization_inside_once_stream_if;
            tobjs.ctor_flags |= SCF_DISP_RW_ONCE;
        }
        else
            tobjs.stream_if = &packetization_inside_many_stream_if;
    }
    else
        /* Need this for on_new_stream() callback not to mess with
         * the context, otherwise this is not used.
         */
        tobjs.stream_if = &packetization_inside_many_stream_if;

    streams[0] = new_stream(&tobjs, 7);
    streams[1] = new_stream_ext(&tobjs, 5, sizeof(buf) - 1);

    if (first_stream_sz)
    {
        lsquic_stream_write(streams[0], buf, first_stream_sz);
        lsquic_stream_flush(streams[0]);
    }

    if (schedule_stream_packets_immediately)
        lsquic_stream_dispatch_write_events(streams[1]);
    else
    {
        packetization_write_as_much_as_you_can(streams[1],
                                                (void *) &packet_stream_ctx);
        g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;
        lsquic_send_ctl_schedule_buffered(&tobjs.send_ctl, BPT_HIGHEST_PRIO);
        g_ctl_settings.tcs_schedule_stream_packets_immediately = 0;
    }

    assert(packet_stream_ctx.off == packet_stream_ctx.len - first_stream_sz - 1);

    /* Verify written data: */
    nw = read_from_scheduled_packets(&tobjs.send_ctl, streams[1]->id, buf_out,
                                     sizeof(buf_out), 0, &fin, 1);
    assert(nw == sizeof(buf) - first_stream_sz - 1);
    assert(!fin);
    assert(0 == memcmp(buf, buf_out, sizeof(buf) - first_stream_sz - 1));

    lsquic_stream_destroy(streams[0]);
    lsquic_stream_destroy(streams[1]);
    deinit_test_objs(&tobjs);
}


/* Test window update logic, not connection limited */
static void
test_window_update2 (void)
{
    ssize_t nw;
    int s;
    struct test_objs tobjs;
    struct lsquic_stream *stream;
    lsquic_packet_out_t *packet_out;
    struct lsquic_conn_cap *const conn_cap = &tobjs.conn_pub.conn_cap;
    unsigned char buf[0x1000];

    init_test_objs(&tobjs, 0x4000, 0x4000);
    n_closed = 0;
    stream = new_stream_ext(&tobjs, LSQUIC_STREAM_HANDSHAKE, 3);
    nw = lsquic_stream_write(stream, "1234567890", 10);
    lsquic_stream_flush(stream);
    assert(("lsquic_stream_write is limited by the send window", 3 == nw));
    s = lsquic_stream_flush(stream);
    assert(0 == s);
    assert(("cc_tosend is not updated when not limited by connection",
                                            0 == conn_cap->cc_sent));
    assert(stream->stream_flags & STREAM_SEND_BLOCKED);
    nw = read_from_scheduled_packets(&tobjs.send_ctl, stream->id, buf,
                                                    sizeof(buf), 0, NULL, 0);
    assert(nw == 3);
    assert(0 == memcmp(buf, "123", 3));

    /* Pretend we sent out a packet: */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs.send_ctl);
    lsquic_send_ctl_sent_packet(&tobjs.send_ctl, packet_out, 1);

    lsquic_stream_window_update(stream, 20);
    nw = lsquic_stream_write(stream, "4567890", 7);
    assert(("lsquic_stream_write: wrote remainig 7 bytes", 7 == nw));
    s = lsquic_stream_flush(stream);
    assert(0 == s);

    /* Verify written data: */
    nw = read_from_scheduled_packets(&tobjs.send_ctl, stream->id, buf,
                                                    sizeof(buf), 3, NULL, 0);
    assert(nw == 7);
    assert(0 == memcmp(buf, "4567890", 7));

    lsquic_stream_destroy(stream);
    assert(("on_close called", 1 == n_closed));

    deinit_test_objs(&tobjs);
}


/* Test that stream is marked as both stream- and connection-blocked */
static void
test_blocked_flags (void)
{
    ssize_t nw;
    struct test_objs tobjs;
    struct lsquic_stream *stream;
    struct lsquic_conn_cap *const conn_cap = &tobjs.conn_pub.conn_cap;
    int s;

    init_test_ctl_settings(&g_ctl_settings);
    g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;

    init_test_objs(&tobjs, 3, 3);
    stream = new_stream_ext(&tobjs, 123, 3);
    nw = lsquic_stream_write(stream, "1234567890", 10);
    assert(("lsquic_stream_write is limited by the send window", 3 == nw));
    assert(("cc_tosend is updated immediately",
                                            3 == conn_cap->cc_sent));
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs.send_ctl)); /* Flush occurred already */
    s = lsquic_stream_flush(stream);
    assert(0 == s);
    assert(("cc_tosend is updated when limited by connection",
                                            3 == conn_cap->cc_sent));
    assert(stream->stream_flags & STREAM_SEND_BLOCKED);
    assert(3 == stream->blocked_off);
    assert(tobjs.lconn.cn_flags & LSCONN_SEND_BLOCKED);
    assert(3 == conn_cap->cc_blocked);

    lsquic_stream_destroy(stream);
    deinit_test_objs(&tobjs);
}


static void
test_forced_flush_when_conn_blocked (void)
{
    ssize_t nw;
    struct test_objs tobjs;
    struct lsquic_stream *stream;
    struct lsquic_conn_cap *const conn_cap = &tobjs.conn_pub.conn_cap;

    init_test_ctl_settings(&g_ctl_settings);
    g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;

    init_test_objs(&tobjs, 3, 0x1000);
    stream = new_stream(&tobjs, 123);
    nw = lsquic_stream_write(stream, "1234567890", 10);
    assert(("lsquic_stream_write is limited by the send window", 3 == nw));
    assert(("cc_tosend is updated immediately",
                                            3 == conn_cap->cc_sent));
    assert(1 == lsquic_send_ctl_n_scheduled(&tobjs.send_ctl)); /* Flush occurred */
    assert(tobjs.lconn.cn_flags & LSCONN_SEND_BLOCKED);
    assert(3 == conn_cap->cc_blocked);

    lsquic_stream_destroy(stream);
    deinit_test_objs(&tobjs);
}


static int
my_gen_stream_frame_err (unsigned char *buf, size_t bufsz,
                         uint32_t stream_id, uint64_t offset,
                         int fin, size_t size, gsf_read_f read,
                         void *stream)
{
    return -1;
}


static void
test_conn_abort (void)
{
    ssize_t nw;
    struct test_objs tobjs;
    struct lsquic_stream *stream;
    struct parse_funcs my_pf;
    int s;

    init_test_ctl_settings(&g_ctl_settings);
    g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;

    init_test_objs(&tobjs, 0x1000, 0x1000);
    my_pf = *tobjs.lconn.cn_pf;
    my_pf.pf_gen_stream_frame = my_gen_stream_frame_err;
    tobjs.lconn.cn_pf = &my_pf;

    stream = new_stream(&tobjs, 123);
    nw = lsquic_stream_write(stream, "1234567890", 10);
    assert(10 == nw);   /* No error yet */
    s = lsquic_stream_flush(stream);
    assert(s < 0);
    assert(stream->stream_flags & STREAM_ABORT_CONN);
    assert(!TAILQ_EMPTY(&tobjs.conn_pub.service_streams));

    lsquic_stream_destroy(stream);
    deinit_test_objs(&tobjs);
}


/* Test one: large frame first, followed by small frames to finish off
 * the packet.
 */
static void
test_bad_packbits_guess_1 (void)
{
    lsquic_packet_out_t *packet_out;
    ssize_t nw;
    struct test_objs tobjs;
    struct lsquic_stream *streams[3];
    char buf[0x1000];
    unsigned char buf_out[0x1000];
    int s, fin;

    init_buf(buf, sizeof(buf));

    init_test_ctl_settings(&g_ctl_settings);
    g_ctl_settings.tcs_schedule_stream_packets_immediately = 0;
    g_ctl_settings.tcs_guess_packno_bits = PACKNO_LEN_1;

    init_test_objs(&tobjs, 0x1000, 0x1000);
    streams[0] = new_stream(&tobjs, 5);
    streams[1] = new_stream(&tobjs, 7);
    streams[2] = new_stream(&tobjs, 9);

    /* Perfrom writes on the three streams.  This is tuned to fill a single
     * packet completely -- we check this later in this function.
     */
    nw = lsquic_stream_write(streams[0], buf, 1337);
    assert(nw == 1337);
    s = lsquic_stream_flush(streams[0]);
    assert(0 == s);
    s = lsquic_stream_shutdown(streams[1], 1);
    assert(s == 0);
    nw = lsquic_stream_write(streams[2], buf + 1337, 1);
    assert(nw == 1);
    s = lsquic_stream_shutdown(streams[2], 1);
    assert(s == 0);

    /* Verify that we got one packet filled to the top: */
    const struct buf_packet_q *const bpq =
            &tobjs.send_ctl.sc_buffered_packets[g_ctl_settings.tcs_bp_type];
    assert(("packetized -- 1 packet", 1 == bpq->bpq_count));
    packet_out = TAILQ_FIRST(&bpq->bpq_packets);
    assert(0 == lsquic_packet_out_avail(packet_out));

    assert(1 == streams[0]->n_unacked);
    assert(1 == streams[1]->n_unacked);
    assert(1 == streams[2]->n_unacked);

    g_ctl_settings.tcs_schedule_stream_packets_immediately = 1;
    g_ctl_settings.tcs_calc_packno_bits = PACKNO_LEN_6;
    s = lsquic_send_ctl_schedule_buffered(&tobjs.send_ctl,
                                                g_ctl_settings.tcs_bp_type);
    assert(2 == lsquic_send_ctl_n_scheduled(&tobjs.send_ctl));

    /* Verify written data: */
    nw = read_from_scheduled_packets(&tobjs.send_ctl, streams[0]->id, buf_out,
                                                sizeof(buf_out), 0, &fin, 0);
    assert(nw == 1337);
    assert(!fin);
    assert(0 == memcmp(buf, buf_out, 1337));
    nw = read_from_scheduled_packets(&tobjs.send_ctl, streams[1]->id, buf_out,
                                                sizeof(buf_out), 0, &fin, 0);
    assert(nw == 0);
    assert(fin);
    nw = read_from_scheduled_packets(&tobjs.send_ctl, streams[2]->id, buf_out,
                                                sizeof(buf_out), 0, &fin, 0);
    assert(nw == 1);
    assert(fin);
    assert(0 == memcmp(buf + 1337, buf_out, 1));

    /* Verify packets */
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs.send_ctl);
    assert(lsquic_packet_out_packno_bits(packet_out) == PACKNO_LEN_6);
    assert(1 == packet_out->po_packno);
    assert(packet_out->po_frame_types & (1 << QUIC_FRAME_STREAM));
    lsquic_send_ctl_sent_packet(&tobjs.send_ctl, packet_out, 1);
    packet_out = lsquic_send_ctl_next_packet_to_send(&tobjs.send_ctl);
    assert(lsquic_packet_out_packno_bits(packet_out) == PACKNO_LEN_6);
    assert(2 == packet_out->po_packno);
    assert(packet_out->po_frame_types & (1 << QUIC_FRAME_STREAM));
    lsquic_send_ctl_sent_packet(&tobjs.send_ctl, packet_out, 1);

    assert(1 == streams[0]->n_unacked);
    assert(1 == streams[1]->n_unacked);
    assert(1 == streams[2]->n_unacked);
    ack_packet(&tobjs.send_ctl, 1);
    assert(0 == streams[0]->n_unacked);
    assert(1 == streams[1]->n_unacked);
    assert(1 == streams[2]->n_unacked);
    ack_packet(&tobjs.send_ctl, 2);
    assert(0 == streams[0]->n_unacked);
    assert(0 == streams[1]->n_unacked);
    assert(0 == streams[2]->n_unacked);

    lsquic_stream_destroy(streams[0]);
    lsquic_stream_destroy(streams[1]);
    lsquic_stream_destroy(streams[2]);
    deinit_test_objs(&tobjs);
}


int
main (int argc, char **argv)
{
    int opt;

    lsquic_global_init(LSQUIC_GLOBAL_SERVER);

    while (-1 != (opt = getopt(argc, argv, "Ahl:")))
    {
        switch (opt)
        {
        case 'A':
            stream_ctor_flags &= ~SCF_DI_AUTOSWITCH;
            break;
        case 'h':
            stream_ctor_flags |= SCF_USE_DI_HASH;
            break;
        case 'l':
            lsquic_logger_lopt(optarg);
            break;
        default:
            exit(1);
        }
    }

    init_test_ctl_settings(&g_ctl_settings);

    test_writing_to_stream_schedule_stream_packets_immediately();
    test_writing_to_stream_outside_callback();
    test_window_update1();
    test_window_update2();
    test_forced_flush_when_conn_blocked();
    test_blocked_flags();
    test_reading_from_stream2();

    {
        int idx[6];
        permute_and_run(0, 0, 0, idx, sizeof(idx) / sizeof(idx[0]));
    }

    test_termination();

    test_writev();

    test_prio_conversion();

    test_read_in_middle();

    test_conn_unlimited();

    test_flushing();

    test_conn_abort();

    test_bad_packbits_guess_1();
    test_bad_packbits_guess_2();
    test_bad_packbits_guess_3();

    const unsigned fp_sizes[] = { 0, 10, 100, 501, 1290, };
    unsigned i;
    for (i = 0; i < sizeof(fp_sizes) / sizeof(fp_sizes[0]); ++i)
    {
        int once;
        unsigned write_size;
        for (write_size = 1; write_size < QUIC_MAX_PACKET_SZ; ++write_size)
            test_packetization(0, 0, write_size, fp_sizes[i]);
        srand(7891);
        for (write_size = 1; write_size < QUIC_MAX_PACKET_SZ * 10; ++write_size)
            test_packetization(0, 0, RANDOM_WRITE_SIZE, fp_sizes[i]);
        for (once = 0; once < 2; ++once)
        {
            for (write_size = 1; write_size < QUIC_MAX_PACKET_SZ; ++write_size)
                test_packetization(1, once, write_size, fp_sizes[i]);
            srand(7891);
            for (write_size = 1; write_size < QUIC_MAX_PACKET_SZ * 10; ++write_size)
                test_packetization(1, once, RANDOM_WRITE_SIZE, fp_sizes[i]);
        }
    }

    return 0;
}

static const char on_being_idle[] =
"ON BEING IDLE."
""
"Now, this is a subject on which I flatter myself I really am _au fait_."
"The gentleman who, when I was young, bathed me at wisdom's font for nine"
"guineas a term--no extras--used to say he never knew a boy who could"
"do less work in more time; and I remember my poor grandmother once"
"incidentally observing, in the course of an instruction upon the use"
"of the Prayer-book, that it was highly improbable that I should ever do"
"much that I ought not to do, but that she felt convinced beyond a doubt"
"that I should leave undone pretty well everything that I ought to do."
""
"I am afraid I have somewhat belied half the dear old lady's prophecy."
"Heaven help me! I have done a good many things that I ought not to have"
"done, in spite of my laziness. But I have fully confirmed the accuracy"
"of her judgment so far as neglecting much that I ought not to have"
"neglected is concerned. Idling always has been my strong point. I take"
"no credit to myself in the matter--it is a gift. Few possess it. There"
"are plenty of lazy people and plenty of slow-coaches, but a genuine"
"idler is a rarity. He is not a man who slouches about with his hands in"
"his pockets. On the contrary, his most startling characteristic is that"
"he is always intensely busy."
""
"It is impossible to enjoy idling thoroughly unless one has plenty of"
"work to do. There is no fun in doing nothing when you have nothing to"
"do. Wasting time is merely an occupation then, and a most exhausting"
"one. Idleness, like kisses, to be sweet must be stolen."
""
"Many years ago, when I was a young man, I was taken very ill--I never"
"could see myself that much was the matter with me, except that I had"
"a beastly cold. But I suppose it was something very serious, for the"
"doctor said that I ought to have come to him a month before, and that"
"if it (whatever it was) had gone on for another week he would not have"
"answered for the consequences. It is an extraordinary thing, but I"
"never knew a doctor called into any case yet but what it transpired"
"that another day's delay would have rendered cure hopeless. Our medical"
"guide, philosopher, and friend is like the hero in a melodrama--he"
"always comes upon the scene just, and only just, in the nick of time. It"
"is Providence, that is what it is."
""
"Well, as I was saying, I was very ill and was ordered to Buxton for a"
"month, with strict injunctions to do nothing whatever all the while"
"that I was there. \"Rest is what you require,\" said the doctor, \"perfect"
"rest.\""
""
"It seemed a delightful prospect. \"This man evidently understands my"
"complaint,\" said I, and I pictured to myself a glorious time--a four"
"weeks' _dolce far niente_ with a dash of illness in it. Not too much"
"illness, but just illness enough--just sufficient to give it the flavor"
"of suffering and make it poetical. I should get up late, sip chocolate,"
"and have my breakfast in slippers and a dressing-gown. I should lie out"
"in the garden in a hammock and read sentimental novels with a melancholy"
"ending, until the books should fall from my listless hand, and I should"
"recline there, dreamily gazing into the deep blue of the firmament,"
"watching the fleecy clouds floating like white-sailed ships across"
"its depths, and listening to the joyous song of the birds and the low"
"rustling of the trees. Or, on becoming too weak to go out of doors,"
"I should sit propped up with pillows at the open window of the"
"ground-floor front, and look wasted and interesting, so that all the"
"pretty girls would sigh as they passed by."
""
"And twice a day I should go down in a Bath chair to the Colonnade to"
"drink the waters. Oh, those waters! I knew nothing about them then,"
"and was rather taken with the idea. \"Drinking the waters\" sounded"
"fashionable and Queen Anne-fied, and I thought I should like them. But,"
"ugh! after the first three or four mornings! Sam Weller's description of"
"them as \"having a taste of warm flat-irons\" conveys only a faint idea of"
"their hideous nauseousness. If anything could make a sick man get well"
"quickly, it would be the knowledge that he must drink a glassful of them"
"every day until he was recovered. I drank them neat for six consecutive"
"days, and they nearly killed me; but after then I adopted the plan of"
"taking a stiff glass of brandy-and-water immediately on the top of them,"
"and found much relief thereby. I have been informed since, by various"
"eminent medical gentlemen, that the alcohol must have entirely"
"counteracted the effects of the chalybeate properties contained in the"
"water. I am glad I was lucky enough to hit upon the right thing."
;

static void
init_buf (void *buf, size_t sz)
{
    unsigned char *p = buf;
    unsigned char *const end = (unsigned char*)buf + sz;
    size_t n;

    while (p < end)
    {
        n = end - p;
        if (sizeof(on_being_idle) - 1 < n)
            n = sizeof(on_being_idle) - 1;
        memcpy(p, on_being_idle, n);
        p +=n;
    }

    assert(p == end);
}

/**
 * @file
 * Transmission Control Protocol, incoming traffic
 *
 * The input processing functions of the TCP layer.
 *
 * These functions are generally called in the order (ip_input() ->)
 * tcp_input() -> * tcp_process() -> tcp_receive() (-> application).
 *
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

#include "core/lwip/opt.h"

#include "core/lwip/tcp_impl.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef struct parsed_ip_hdr {
    bool is_ipv6;
    s16_t header_length;
    u16_t total_length;
    const void *src, *dest;
} parsed_ip_hdr_t;

/* TODO Remove duplicate knowledge in tcp_in_data (seqno, TCP flags, length, tcphdr). */
typedef struct tcp_in_data {
    struct pbuf *recv_data;
    struct tcp_hdr *tcphdr;
    parsed_ip_hdr_t iphdr;
    u32_t seqno;
    u32_t ackno;
    u16_t tcplen;
    u8_t flags;
    u8_t recv_flags;
    struct tcp_seg inseg;
} tcp_in_data;

/* Forward declarations. */
static err_t tcp_process(struct tcp_pcb *pcb, tcp_in_data *in_data);
static void tcp_receive(struct tcp_pcb *pcb, tcp_in_data *in_data);
static bool tcp_parseopt_ts(u8_t *opts, u16_t opts_len, u32_t *tsval);
static void tcp_parseopt(struct tcp_pcb *pcb, tcp_in_data *in_data);

static void tcp_listen_input(struct tcp_pcb *pcb, tcp_in_data *in_data);
static err_t tcp_timewait_input(struct tcp_pcb *pcb, tcp_in_data *in_data);
static s8_t tcp_quickack(struct tcp_pcb *pcb, tcp_in_data *in_data);

/**
 * Send quickack if TCP_QUICKACK is enabled
 * Change LWIP_TCP_QUICKACK_THRESHOLD value in order to send quickacks
 * depending on the payload size.
 */
s8_t tcp_quickack(struct tcp_pcb *pcb, tcp_in_data *in_data)
{
#if TCP_QUICKACK_THRESHOLD
    return pcb->quickack && in_data->tcplen <= TCP_QUICKACK_THRESHOLD;
#else
    LWIP_UNUSED_ARG(in_data);
    return pcb->quickack;
#endif
}

static inline void fill_parsed_ip_hdr(const void *payload, parsed_ip_hdr_t *iphdr)
{
    const u8_t *view_8bit = (const u8_t *)payload;
    const u16_t *view_16bit = (const u16_t *)payload;

    iphdr->is_ipv6 = (view_8bit[0] >> 4U) == IPV6_VERSION;
    if (iphdr->is_ipv6) {
        iphdr->src = (void *)&view_8bit[8];
        iphdr->dest = (void *)&view_8bit[24];
        iphdr->header_length = 40;
        iphdr->total_length = ntohs(view_16bit[2U]) + iphdr->header_length;
    } else {
        iphdr->src = (const void *)&view_8bit[12];
        iphdr->dest = (const void *)&view_8bit[16];
        iphdr->header_length = ((view_8bit[0] & 0x0f) * 4);
        iphdr->total_length = ntohs(view_16bit[1U]);
    }
}

void L3_level_tcp_input(struct pbuf *p, struct tcp_pcb *pcb)
{
    u8_t hdrlen;
    err_t err;
    tcp_in_data in_data;

    fill_parsed_ip_hdr(p->payload, &in_data.iphdr);

    /* Trim pbuf. This should have been done at the netif layer,
     * but we'll do it anyway just to be sure that its done. */
    pbuf_realloc(p, in_data.iphdr.total_length);

    /* remove header from payload */
    if (pbuf_header(p, (-1) * in_data.iphdr.header_length) ||
        (p->tot_len < sizeof(struct tcp_hdr))) {
        /* drop short packets */
        LWIP_DEBUGF(TCP_INPUT_DEBUG,
                    ("tcp_input: short packet (%" U16_F " bytes) discarded\n", (u16_t)p->tot_len));
        pbuf_free(p);
        return;
    }

    in_data.tcphdr = (struct tcp_hdr *)p->payload;
    tcp_debug_print(in_data.tcphdr);

    /* Move the payload pointer in the pbuf so that it points to the
       TCP data instead of the TCP header. */
    hdrlen = TCPH_HDRLEN(in_data.tcphdr);
    if (pbuf_header(p, -(hdrlen * 4))) {
        /* drop short packets */
        LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_input: short packet\n"));
        pbuf_free(p);
        return;
    }

    /* Convert fields in TCP header to host byte order. */
    in_data.tcphdr->src = ntohs(in_data.tcphdr->src);
    in_data.tcphdr->dest = ntohs(in_data.tcphdr->dest);
    in_data.seqno = in_data.tcphdr->seqno = ntohl(in_data.tcphdr->seqno);
    in_data.ackno = in_data.tcphdr->ackno = ntohl(in_data.tcphdr->ackno);
    in_data.tcphdr->wnd = ntohs(in_data.tcphdr->wnd);

    in_data.flags = TCPH_FLAGS(in_data.tcphdr);
    in_data.tcplen = p->tot_len + ((in_data.flags & (TCP_FIN | TCP_SYN)) ? 1 : 0);

    if (pcb != NULL) {

        if (PCB_IN_ACTIVE_STATE(pcb)) {
/* The incoming segment belongs to a connection. */
#if TCP_INPUT_DEBUG
#if TCP_DEBUG
            tcp_debug_print_state(get_tcp_state(pcb));
#endif /* TCP_DEBUG */
#endif /* TCP_INPUT_DEBUG */

            /* Set up a tcp_seg structure. */
            in_data.inseg.next = NULL;
            in_data.inseg.len = p->tot_len;
            in_data.inseg.p = p;
            in_data.inseg.tcphdr = in_data.tcphdr;
            in_data.inseg.seqno = in_data.seqno;
            in_data.inseg.flags = 0;
            in_data.inseg.tcp_flags = in_data.flags;

            in_data.recv_data = NULL;
            in_data.recv_flags = 0;

            pcb->is_in_input = 1;
            err = tcp_process(pcb, &in_data);
            /* A return value of ERR_ABRT means that tcp_abort() was called
               and that the pcb has been freed. If so, we don't do anything. */
            if (err != ERR_ABRT) {
                if (in_data.recv_flags & TF_RESET) {
                    /* TF_RESET means that the connection was reset by the other
                       end. We then call the error callback to inform the
                       application that the connection is dead before we
                       deallocate the PCB.
                       Error callback will trigger event only if pcb is active. */
                    TCP_EVENT_ERR(pcb->errf, pcb->my_container, ERR_RST);
                    tcp_pcb_remove(pcb);
                } else if (in_data.recv_flags & TF_CLOSED) {
                    /* The connection has been closed and we will deallocate the
                       PCB. */
                    tcp_pcb_remove(pcb);
                } else {
                    /* If the application has registered a "sent" function to be
                       called when new send buffer space is available, we call it
                       now. */
                    if (pcb->acked > 0) {
                        TCP_EVENT_SENT(pcb, pcb->acked, err);
                        if (err == ERR_ABRT) {
                            goto aborted;
                        }
                    }

                    if (in_data.recv_data) {
                        if (pcb->flags & TF_RXCLOSED) {
                            /* received data although already closed -> abort (send RST) to
                                           notify the remote host that not all data has been
                               processed */
                            pbuf_free(in_data.recv_data);
                            tcp_abort(pcb);
                            goto aborted;
                        }
                        if (in_data.flags & TCP_PSH) {
                            in_data.recv_data->flags |= PBUF_FLAG_PUSH;
                        }
                        /* Notify application that data has been received. */
                        TCP_EVENT_RECV(pcb, in_data.recv_data, ERR_OK, err);
                        if (err == ERR_ABRT) {
                            goto aborted;
                        }
                        /* If the upper layer can't receive this data, store it */
                        if (err != ERR_OK) {
                            pcb->rcv_wnd += in_data.recv_data->tot_len;
                            pbuf_free(in_data.recv_data);
                        }
                    }

                    /* If a FIN segment was received, we call the callback
                       function with a NULL buffer to indicate EOF. */
                    if (in_data.recv_flags & TF_GOT_FIN) {
                        /* correct rcv_wnd as the application won't call tcp_recved()
                           for the FIN's seqno */
                        if (pcb->rcv_wnd != pcb->rcv_wnd_max) {
                            pcb->rcv_wnd++;
                        }
                        TCP_EVENT_CLOSED(pcb, err);
                        if (err == ERR_ABRT) {
                            goto aborted;
                        }
                    }

                    pcb->is_in_input = 0;
                    /* Try to send something out. */
                    tcp_output(pcb);
                }
            }
            /* Jump target if pcb has been aborted in a callback (by calling tcp_abort()).
               Below this line, 'pcb' may not be dereferenced! */
        aborted:
            pcb->is_in_input = 0;
            in_data.recv_data = NULL;

            /* tcp_receive() sets in_data.inseg.p to NULL in case of recv_data */
            if (in_data.inseg.p != NULL) {
                pbuf_free(in_data.inseg.p);
                in_data.inseg.p = NULL;
            }
        } else if (PCB_IN_LISTEN_STATE(pcb)) {
            LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_input: packed for LISTENing connection.\n"));
            tcp_listen_input(pcb, &in_data);
            pbuf_free(p);
        } else if (PCB_IN_TIME_WAIT_STATE(pcb)) {
            LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_input: packed for TIME_WAITing connection.\n"));
            tcp_timewait_input(pcb, &in_data);
            pbuf_free(p);
        } else {
            LWIP_DEBUGF(TCP_RST_DEBUG, ("tcp_input: illegal get_tcp_state(pcb).\n"));
            pbuf_free(p);
        }
    } else {

        /* If no matching PCB was found, send a TCP RST (reset) to the
           sender. */
        LWIP_DEBUGF(TCP_RST_DEBUG, ("tcp_input: no PCB match found, resetting.\n"));
        if (!(TCPH_FLAGS(in_data.tcphdr) & TCP_RST)) {
            tcp_rst(in_data.ackno, in_data.seqno + in_data.tcplen, in_data.tcphdr->dest,
                    in_data.tcphdr->src, pcb);
        }
        pbuf_free(p);
    }
}

/**
 * Called by L3_level_tcp_input() when a segment arrives for a listening
 * connection (from L3_level_tcp_input()).
 *
 * @param pcb the listen tcp_pcb for which a segment arrived
 * @return The new pcb if there is one. Otherwise, NULL.
 *
 * @note the segment which arrived is saved in global variables, therefore only the pcb
 *       involved is passed as a parameter to this function
 */
static void tcp_listen_input(struct tcp_pcb *pcb, tcp_in_data *in_data)
{
    struct tcp_pcb *npcb = NULL;
    err_t rc;

    if (in_data->flags & (TCP_RST | TCP_FIN)) {
        /* An incoming RST should be ignored. Return.
           An incoming FIN should be ignored. Return. */
        return;
    }

    /* In the LISTEN state, we check for incoming SYN segments,
       creates a new PCB, and responds with a SYN|ACK. */
    if (in_data->flags & TCP_ACK) {
        /* For incoming segments with the ACK flag set, respond with a RST. */
        LWIP_DEBUGF(TCP_RST_DEBUG, ("tcp_listen_input: ACK in LISTEN, sending reset\n"));
        tcp_rst(in_data->ackno + 1, in_data->seqno + in_data->tcplen, in_data->tcphdr->dest,
                in_data->tcphdr->src, NULL);
    } else if (in_data->flags & TCP_SYN) {
        LWIP_DEBUGF(TCP_DEBUG,
                    ("TCP connection request %" U16_F " -> %" U16_F ".\n", in_data->tcphdr->src,
                     in_data->tcphdr->dest));

        TCP_EVENT_CLONE_PCB(pcb, &npcb, rc);

        /* If a new PCB could not be created (probably due to lack of memory),
           we don't do anything, but rely on the sender will retransmit the
           SYN at a time when we have more memory available. */
        if (npcb == NULL) {
            LWIP_DEBUGF(TCP_DEBUG, ("tcp_listen_input: could not allocate PCB\n"));
            return;
        }

        /* Set up the new PCB. */
        npcb->is_ipv6 = in_data->iphdr.is_ipv6;
        ip_addr_from_raw(&npcb->local_ip, in_data->iphdr.dest, in_data->iphdr.is_ipv6);
        npcb->local_port = pcb->local_port;
        ip_addr_from_raw(&npcb->remote_ip, in_data->iphdr.src, in_data->iphdr.is_ipv6);
        npcb->remote_port = in_data->tcphdr->src;
        set_tcp_state(npcb, SYN_RCVD);
        npcb->rcv_nxt = in_data->seqno + 1;
        npcb->rcv_ann_right_edge = npcb->rcv_nxt;
        npcb->snd_wl1 = in_data->seqno - 1; /* initialise to seqno-1 to force window update */
        npcb->callback_arg = pcb->callback_arg;
        npcb->accept = pcb->accept;
        /* inherit socket options */
        npcb->so_options = pcb->so_options & SOF_INHERITED;

        npcb->snd_scale = 0;
        npcb->rcv_scale = 0;

        /* calculate advtsd_mss before parsing MSS option such that the resulting mss will take into
         * account the updated advertized MSS */
        npcb->advtsd_mss = tcp_send_mss(npcb);

        /* Parse any options in the SYN. */
        tcp_parseopt(npcb, in_data);

        npcb->rcv_wnd = TCP_WND_SCALED(npcb);
        npcb->rcv_ann_wnd = TCP_WND_SCALED(npcb);
        npcb->rcv_wnd_max = TCP_WND_SCALED(npcb);
        npcb->rcv_wnd_max_desired = TCP_WND_SCALED(npcb);

        npcb->snd_wnd = SND_WND_SCALE(npcb, in_data->tcphdr->wnd);
        npcb->snd_wnd_max = npcb->snd_wnd;
        npcb->ssthresh = npcb->snd_wnd;
#if TCP_CALCULATE_EFF_SEND_MSS
        // mss can be changed by tcp_parseopt, need to take the MIN
        UPDATE_PCB_BY_MSS(npcb, LWIP_MIN(npcb->mss, npcb->advtsd_mss));
#endif /* TCP_CALCULATE_EFF_SEND_MSS */

        /* Register the new PCB so that we can begin sending segments
         for it. */
        TCP_EVENT_SYN_RECEIVED(pcb, npcb, rc);
        if (rc != ERR_OK) {
            return;
        }

        /* Send a SYN|ACK together with the MSS option. */
        if (ERR_OK == tcp_enqueue_flags(npcb, TCP_SYN | TCP_ACK)) {
            tcp_output(npcb);
        } else {
            tcp_abandon(npcb, 0);
        }

        TCP_EVENT_ACCEPTED_PCB(pcb, npcb);
    }
}

/**
 * Reuse TIME-WAIT socket and move it to SYN-RCVD state.
 */
static err_t tcp_pcb_reuse(struct tcp_pcb *pcb, tcp_in_data *in_data)
{
    err_t rc;

    tcp_pcb_recycle(pcb);
    set_tcp_state(pcb, SYN_RCVD);
    pcb->rcv_nxt = in_data->seqno + 1;
    pcb->rcv_ann_right_edge = pcb->rcv_nxt;
    pcb->snd_wl1 = in_data->seqno - 1; /* initialise to seqno-1 to force window update */
    pcb->mss = pcb->advtsd_mss = tcp_send_mss(pcb);
    /* Parse any options in the SYN. */
    tcp_parseopt(pcb, in_data);
    pcb->rcv_wnd = TCP_WND_SCALED(pcb);
    pcb->rcv_ann_wnd = TCP_WND_SCALED(pcb);
    pcb->rcv_wnd_max = TCP_WND_SCALED(pcb);
    pcb->rcv_wnd_max_desired = TCP_WND_SCALED(pcb);
    pcb->snd_wnd = SND_WND_SCALE(pcb, in_data->tcphdr->wnd);
    pcb->snd_wnd_max = pcb->snd_wnd;
    pcb->ssthresh = pcb->snd_wnd;

    // mss can be changed by tcp_parseopt, need to take the MIN
    UPDATE_PCB_BY_MSS(pcb, LWIP_MIN(pcb->mss, pcb->advtsd_mss));
    rc = pcb->syn_tw_handled_cb(pcb->listen_sock, pcb);
    if (rc != ERR_OK) {
        return rc;
    }
    /* Send a SYN|ACK together with the MSS option. */
    rc = tcp_enqueue_flags(pcb, TCP_SYN | TCP_ACK);
    if (rc != ERR_OK) {
        tcp_abandon(pcb, 0);
        return rc;
    }
    return tcp_output(pcb);
}

/**
 * Called by tcp_input() when a segment arrives for a connection in
 * TIME_WAIT.
 *
 * @param pcb the tcp_pcb for which a segment arrived
 *
 * @note the segment which arrived is saved in global variables, therefore only the pcb
 *       involved is passed as a parameter to this function
 */
static err_t tcp_timewait_input(struct tcp_pcb *pcb, tcp_in_data *in_data)
{
    /* RFC 1337: in TIME_WAIT, ignore RST and ACK FINs + any 'acceptable' segments */
    /* RFC 793 3.9 Event Processing - Segment Arrives:
     * - first check sequence number - we skip that one in TIME_WAIT (always
     *   acceptable since we only send ACKs)
     * - second check the RST bit (... return) */
    if (in_data->flags & TCP_RST) {
        return ERR_OK;
    }
    /* - fourth, check the SYN bit, */
    if ((in_data->flags & (TCP_SYN | TCP_ACK)) == TCP_SYN) {
        bool reusable;

        /* Check whether socket can be reused according to RFC 6191 */
#if LWIP_TCP_TIMESTAMPS
        u16_t opts_len = (TCPH_HDRLEN(in_data->tcphdr) - 5) << 2;
        u32_t tsval = 0;

        /* Whether timestamps are present in SYN packet and previous incarnation */
        reusable = tcp_parseopt_ts((u8_t *)in_data->tcphdr + TCP_HLEN, opts_len, &tsval) &&
            (pcb->flags & TF_TIMESTAMP);
        /* According to the RFC, we can reuse socket:
         * - timestamps are enabled and SYN timestamp is greater than the last seen
         * - timestamps are enabled and SYN timestamp is equal to the last seen and
         *   and seqno of SYN is greater than last seen seqno
         * - timestamps are disabled and seqno of SYN is greater than last seen seqno */
        reusable = (reusable && pcb->ts_recent < tsval) ||
            ((!reusable || pcb->ts_recent == tsval) && TCP_SEQ_GEQ(in_data->seqno, pcb->rcv_nxt));
#else
        reusable = TCP_SEQ_GEQ(in_data->seqno, pcb->rcv_nxt);
#endif
        reusable &= (pcb->syn_tw_handled_cb != NULL);
        if (reusable) {
            return tcp_pcb_reuse(pcb, in_data);
        } else {
            /* RFC 6191: Otherwise, silently drop the incoming SYN segment... */
            return ERR_OK;
        }
    } else if (in_data->flags & TCP_FIN) {
        /* - eighth, check the FIN bit: Remain in the TIME-WAIT state.
             Restart the 2 MSL time-wait timeout.*/
        pcb->tmr = tcp_ticks;
    }

    if ((in_data->tcplen > 0)) {
        if ((in_data->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            /* RST on out of state SYN-ACK */
            tcp_rst(in_data->ackno, in_data->seqno + in_data->tcplen, in_data->tcphdr->dest,
                    in_data->tcphdr->src, pcb);
        } else {
            /* Acknowledge data or FIN */
            pcb->flags |= TF_ACK_NOW;
            return tcp_output(pcb);
        }
    }
    return ERR_OK;
}

/**
 * Implements the TCP state machine. Called by tcp_input. In some
 * states tcp_receive() is called to receive data. The tcp_seg
 * argument will be freed by the caller (tcp_input()) unless the
 * recv_data pointer in the pcb is set.
 *
 * @param pcb the tcp_pcb for which a segment arrived
 *
 * @note the segment which arrived is saved in global variables, therefore only the pcb
 *       involved is passed as a parameter to this function
 */
static err_t tcp_process(struct tcp_pcb *pcb, tcp_in_data *in_data)
{
    struct tcp_seg *rseg;
    u8_t acceptable = 0;
    err_t err;

    /* Process incoming RST segments. */
    if (in_data->flags & TCP_RST) {
        /* First, determine if the reset is acceptable. */
        if (get_tcp_state(pcb) == SYN_SENT) {
            if (in_data->ackno == pcb->snd_nxt) {
                acceptable = 1;
            }
        } else {
            if (TCP_SEQ_BETWEEN(in_data->seqno, pcb->rcv_nxt, pcb->rcv_nxt + pcb->rcv_wnd)) {
                acceptable = 1;
            }
        }

        if (acceptable) {
            LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_process: Connection RESET\n"));
            LWIP_ASSERT("tcp_input: get_tcp_state(pcb) != CLOSED", get_tcp_state(pcb) != CLOSED);
            in_data->recv_flags |= TF_RESET;
            pcb->flags &= ~TF_ACK_DELAY;
            return ERR_RST;
        } else {
            LWIP_DEBUGF(TCP_INPUT_DEBUG,
                        ("tcp_process: unacceptable reset seqno %" U32_F " rcv_nxt %" U32_F "\n",
                         in_data->seqno, pcb->rcv_nxt));
            LWIP_DEBUGF(TCP_DEBUG,
                        ("tcp_process: unacceptable reset seqno %" U32_F " rcv_nxt %" U32_F "\n",
                         in_data->seqno, pcb->rcv_nxt));
            return ERR_OK;
        }
    }

    if ((in_data->flags & TCP_SYN) &&
        (get_tcp_state(pcb) != SYN_SENT && get_tcp_state(pcb) != SYN_RCVD)) {
        /* Cope with new connection attempt after remote end crashed */
        tcp_ack_now(pcb);
        return ERR_OK;
    }

    if ((pcb->flags & TF_RXCLOSED) == 0) {
        /* Update the PCB (in)activity timer unless rx is closed (see tcp_shutdown) */
        pcb->tmr = tcp_ticks;
    }
    pcb->keep_cnt_sent = 0;

    tcp_parseopt(pcb, in_data);

    /* Do different things depending on the TCP state. */
    switch (get_tcp_state(pcb)) {
    case SYN_SENT:
        LWIP_DEBUGF(TCP_INPUT_DEBUG,
                    ("SYN-SENT: ackno %" U32_F " pcb->snd_nxt %" U32_F " unacked %" U32_F "\n",
                     in_data->ackno, pcb->snd_nxt, ntohl(pcb->unacked->tcphdr->seqno)));
        /* received SYN ACK with expected sequence number? */
        if ((in_data->flags & TCP_ACK) && (in_data->flags & TCP_SYN) &&
            in_data->ackno == pcb->unacked->seqno + 1) {
            // pcb->snd_buf++; SND_BUF_FOR_SYN_FIN
            pcb->rcv_nxt = in_data->seqno + 1;
            pcb->rcv_ann_right_edge = pcb->rcv_nxt;
            pcb->lastack = in_data->ackno;
            pcb->snd_wnd = SND_WND_SCALE(
                pcb, in_data->tcphdr->wnd); // Which means: tcphdr->wnd << pcb->snd_scale;
            pcb->snd_wnd_max = pcb->snd_wnd;
            pcb->snd_wl1 = in_data->seqno - 1; /* initialise to seqno - 1 to force window update */
            set_tcp_state(pcb, ESTABLISHED);

#if TCP_CALCULATE_EFF_SEND_MSS
            // mss can be changed by tcp_parseopt, need to take the MIN
            UPDATE_PCB_BY_MSS(pcb, LWIP_MIN(pcb->mss, tcp_send_mss(pcb)));
#endif /* TCP_CALCULATE_EFF_SEND_MSS */

            /* Set ssthresh again after changing pcb->mss (already set in tcp_connect
             * but for the default value of pcb->mss) */
            pcb->ssthresh = pcb->mss * 10;
#if TCP_CC_ALGO_MOD
            cc_conn_init(pcb);
#else
            pcb->cwnd = ((pcb->cwnd == 1) ? (pcb->mss * 2) : pcb->mss);
#endif
            LWIP_ASSERT("pcb->snd_queuelen > 0", (pcb->snd_queuelen > 0));
            --pcb->snd_queuelen;
            LWIP_DEBUGF(
                TCP_QLEN_DEBUG,
                ("tcp_process: SYN-SENT --queuelen %" U16_F "\n", (u16_t)pcb->snd_queuelen));
            rseg = pcb->unacked;
            pcb->unacked = rseg->next;

            /* If there's nothing left to acknowledge, stop the retransmit
               timer, otherwise reset it to start again */
            if (pcb->unacked == NULL) {
                pcb->rtime = -1;
                pcb->ticks_since_data_sent = -1;
            } else {
                pcb->rtime = 0;
                pcb->ticks_since_data_sent = 0;
                pcb->nrtx = 0;
            }

            tcp_tx_seg_free(pcb, rseg);

            /* Call the user specified function to call when sucessfully
             * connected. */
            TCP_EVENT_CONNECTED(pcb, ERR_OK, err);
            if (err == ERR_ABRT) {
                return ERR_ABRT;
            }
            tcp_ack_now(pcb);
        }
        /* received ACK? possibly a half-open connection */
        else if (in_data->flags & TCP_ACK) {
            /* send a RST to bring the other side in a non-synchronized state. */
            tcp_rst(in_data->ackno, in_data->seqno + in_data->tcplen, in_data->tcphdr->dest,
                    in_data->tcphdr->src, pcb);
        }
        break;
    case SYN_RCVD:
        if (in_data->flags & TCP_ACK) {
            /* expected ACK number? */
            if (TCP_SEQ_BETWEEN(in_data->ackno, pcb->lastack + 1, pcb->snd_nxt)) {
                u32_t old_cwnd;
                set_tcp_state(pcb, ESTABLISHED);
                LWIP_DEBUGF(TCP_DEBUG,
                            ("TCP connection established %" U16_F " -> %" U16_F ".\n",
                             in_data->inseg.tcphdr->src, in_data->inseg.tcphdr->dest));
                LWIP_ASSERT("pcb->accept != NULL", pcb->accept != NULL);
                /* Call the accept function. */
                TCP_EVENT_ACCEPT(pcb, ERR_OK, err);
                if (err != ERR_OK) {
                    /* If the accept function returns with an error, we abort
                     * the connection. */
                    /* Already aborted? */
                    if (err != ERR_ABRT) {
                        tcp_abort(pcb);
                    }
                    return ERR_ABRT;
                }
                old_cwnd = pcb->cwnd;
                /* If there was any data contained within this ACK,
                 * we'd better pass it on to the application as well. */
                tcp_receive(pcb, in_data);

                /* Prevent ACK for SYN to generate a sent event */
                if (pcb->acked != 0) {
                    pcb->acked--;
                }
#if TCP_CC_ALGO_MOD
                pcb->cwnd = old_cwnd;
                cc_conn_init(pcb);
#else
                pcb->cwnd = ((old_cwnd == 1) ? (pcb->mss * 2) : pcb->mss);
#endif
                if (in_data->recv_flags & TF_GOT_FIN) {
                    tcp_ack_now(pcb);
                    set_tcp_state(pcb, CLOSE_WAIT);
                }
            } else {
                /* incorrect ACK number, send RST */
                tcp_rst(in_data->ackno, in_data->seqno + in_data->tcplen, in_data->tcphdr->dest,
                        in_data->tcphdr->src, pcb);
            }
        } else if ((in_data->flags & TCP_SYN) && (in_data->seqno == pcb->rcv_nxt - 1)) {
            /* Looks like another copy of the SYN - retransmit our SYN-ACK */
            tcp_rexmit(pcb);
        }
        // Currently received packets with only FIN bit in SYN_RCVD state are ignored.
        break;
    case CLOSE_WAIT:
        /* FALLTHROUGH */
    case ESTABLISHED:
        tcp_receive(pcb, in_data);
        if (in_data->recv_flags & TF_GOT_FIN) { /* passive close */
            tcp_ack_now(pcb);
            set_tcp_state(pcb, CLOSE_WAIT);
        }
        break;
    case FIN_WAIT_1:
        tcp_receive(pcb, in_data);
        if (in_data->recv_flags & TF_GOT_FIN) {
            if ((in_data->flags & TCP_ACK) && (in_data->ackno == pcb->snd_nxt)) {
                LWIP_DEBUGF(TCP_DEBUG,
                            ("TCP connection closed: FIN_WAIT_1 %" U16_F " -> %" U16_F ".\n",
                             in_data->inseg.tcphdr->src, in_data->inseg.tcphdr->dest));
                tcp_ack_now(pcb);
                tcp_pcb_purge(pcb);
                set_tcp_state(pcb, TIME_WAIT);
            } else {
                tcp_ack_now(pcb);
                set_tcp_state(pcb, CLOSING);
            }
        } else if ((in_data->flags & TCP_ACK) && (in_data->ackno == pcb->snd_nxt)) {
            set_tcp_state(pcb, FIN_WAIT_2);
        }
        break;
    case FIN_WAIT_2:
        tcp_receive(pcb, in_data);
        if (in_data->recv_flags & TF_GOT_FIN) {
            LWIP_DEBUGF(TCP_DEBUG,
                        ("TCP connection closed: FIN_WAIT_2 %" U16_F " -> %" U16_F ".\n",
                         in_data->inseg.tcphdr->src, in_data->inseg.tcphdr->dest));
            tcp_ack_now(pcb);
            tcp_pcb_purge(pcb);
            set_tcp_state(pcb, TIME_WAIT);
        }
        break;
    case CLOSING:
        tcp_receive(pcb, in_data);
        if (in_data->flags & TCP_ACK && in_data->ackno == pcb->snd_nxt) {
            LWIP_DEBUGF(TCP_DEBUG,
                        ("TCP connection closed: CLOSING %" U16_F " -> %" U16_F ".\n",
                         in_data->inseg.tcphdr->src, in_data->inseg.tcphdr->dest));
            tcp_pcb_purge(pcb);
            set_tcp_state(pcb, TIME_WAIT);
        }
        break;
    case LAST_ACK:
        tcp_receive(pcb, in_data);
        if (in_data->flags & TCP_ACK && in_data->ackno == pcb->snd_nxt) {
            LWIP_DEBUGF(TCP_DEBUG,
                        ("TCP connection closed: LAST_ACK %" U16_F " -> %" U16_F ".\n",
                         in_data->inseg.tcphdr->src, in_data->inseg.tcphdr->dest));
            /* bugfix #21699: don't set_tcp_state to CLOSED here or we risk leaking segments */
            in_data->recv_flags |= TF_CLOSED;
        }
        break;
    default:
        break;
    }
    return ERR_OK;
}

#if TCP_QUEUE_OOSEQ
/**
 * Insert segment into the list (segments covered with new one will be deleted)
 *
 * Called from tcp_receive()
 */
static void tcp_oos_insert_segment(struct tcp_pcb *pcb, struct tcp_seg *cseg, struct tcp_seg *next,
                                   tcp_in_data *in_data)
{
    struct tcp_seg *old_seg;

    if (TCPH_FLAGS(cseg->tcphdr) & TCP_FIN) {
        /* received segment overlaps all following segments */
        tcp_segs_free(pcb, next);
        next = NULL;
    } else {
        /* delete some following segments
           oos queue may have segments with FIN flag */
        while (next &&
               TCP_SEQ_GEQ((in_data->seqno + cseg->len), (next->tcphdr->seqno + next->len))) {
            /* cseg with FIN already processed */
            if (TCPH_FLAGS(next->tcphdr) & TCP_FIN) {
                TCPH_SET_FLAG(cseg->tcphdr, TCP_FIN);
            }
            old_seg = next;
            next = next->next;
            tcp_seg_free(pcb, old_seg);
        }
        if (next && TCP_SEQ_GT(in_data->seqno + cseg->len, next->tcphdr->seqno)) {
            /* We need to trim the incoming segment. */
            cseg->len = (u32_t)(next->tcphdr->seqno - in_data->seqno);
            pbuf_realloc(cseg->p, cseg->len);
        }
    }
    cseg->next = next;
}
#endif /* TCP_QUEUE_OOSEQ */

/**
 * Called by tcp_output() to shrink TCP segment to lastackno.
 * This call should process retransmitted TSO segment.
 *
 * @param pcb the tcp_pcb for the TCP connection used to send the segment
 * @param seg the tcp_seg to send
 * @param ackqno current ackqno
 * @return number of freed pbufs
 */
static u32_t tcp_shrink_segment(struct tcp_pcb *pcb, struct tcp_seg *seg, u32_t ackno)
{
    struct pbuf *cur_p;
    struct pbuf *p;
    u32_t len;
    u32_t count = 0;
    u8_t optflags = 0;
    u8_t optlen;

    assert(seg != NULL);
    assert(seg->p != NULL);
    assert(!(seg->flags & TF_SEG_OPTS_ZEROCOPY));

#if LWIP_TCP_TIMESTAMPS
    if ((pcb->flags & TF_TIMESTAMP)) {
        optflags |= TF_SEG_OPTS_TS;
    }
#endif /* LWIP_TCP_TIMESTAMPS */

    optlen = LWIP_TCP_OPT_LENGTH(optflags);

    /* Just shrink first pbuf */
    if (TCP_SEQ_GT((seg->seqno + seg->p->len - optlen - TCP_HLEN), ackno)) {
        len = ackno - seg->seqno;
        if (optlen > 0) {
            /* tcp_output_segment() relies on aligned options area */
            len &= 0xfffffffc;
        }

        seg->len -= len;
        seg->seqno += len;
        seg->tcphdr->seqno = htonl(seg->seqno);
        p = seg->p;
        p->tot_len -= len;
        p->len -= len;
        p->payload = (u8_t *)p->payload + len;
        memmove(p->payload, seg->tcphdr, TCP_HLEN);
        seg->tcphdr = p->payload;
        return count;
    }

    cur_p = seg->p->next;

    if (cur_p) {
        /* Process more than first pbuf */
        len = seg->p->len - TCP_HLEN - optlen;
        seg->len -= len;
        seg->seqno += len;
        seg->tcphdr->seqno = htonl(seg->seqno);
        seg->p->tot_len -= len;
        seg->p->len = TCP_HLEN + optlen;
    }

    while (cur_p) {
        if (TCP_SEQ_GT((seg->seqno + cur_p->len), ackno)) {
            break;
        } else {
            seg->len -= cur_p->len;
            seg->seqno += cur_p->len;
            seg->tcphdr->seqno = htonl(seg->seqno);
            seg->p->tot_len -= cur_p->len;
            seg->p->next = cur_p->next;

            p = cur_p;
            cur_p = p->next;
            p->next = NULL;

            if (p->type == PBUF_RAM || p->type == PBUF_ZEROCOPY) {
                external_tcp_tx_pbuf_free(pcb, p);
            } else {
                pbuf_free(p);
            }
            count++;
        }
    }

    if (cur_p) {
        len = ackno - seg->seqno;
        if (optlen > 0) {
            /* tcp_output_segment() relies on aligned options area */
            len &= 0xfffffffc;
        }

        seg->len -= len;
        seg->seqno += len;
        seg->tcphdr->seqno = htonl(seg->seqno);
        cur_p->tot_len -= len - optlen;
        cur_p->len -= len - optlen;
        cur_p->payload = (u8_t *)cur_p->payload + ((s32_t)len - (s32_t)optlen);

        /* Add space for TCP header */
        cur_p->tot_len += TCP_HLEN;
        cur_p->len += TCP_HLEN;
        cur_p->payload = (u8_t *)cur_p->payload - TCP_HLEN;
        memcpy(cur_p->payload, seg->tcphdr, TCP_HLEN);
        seg->tcphdr = cur_p->payload;

        p = seg->p;
        seg->p = cur_p;

        if (p->type == PBUF_RAM || p->type == PBUF_ZEROCOPY) {
            external_tcp_tx_pbuf_free(pcb, p);
        } else {
            pbuf_free(p);
        }
        count++;
    }

#if TCP_TSO_DEBUG
    LWIP_DEBUGF(TCP_TSO_DEBUG | LWIP_DBG_TRACE,
                ("tcp_shrink: count: %-5d unsent %s\n", count, _dump_seg(pcb->unsent)));
#endif /* TCP_TSO_DEBUG */

    return count;
}

/**
 * Called by tcp_output() to shrink TCP segment to lastackno.
 * This call should process retransmitted TSO segment.
 *
 * @param pcb the tcp_pcb for the TCP connection used to send the segment
 * @param seg the tcp_seg to send
 * @param ackqno current ackqno
 * @return number of freed pbufs
 */
static u32_t tcp_shrink_zc_segment(struct tcp_pcb *pcb, struct tcp_seg *seg, u32_t ackno)
{
    struct pbuf *p;
    u32_t count = 0;
    u32_t len;

    assert(seg != NULL);
    assert(seg->p != NULL);
    assert(seg->flags & TF_SEG_OPTS_ZEROCOPY);

    while (TCP_SEQ_GEQ(ackno, seg->seqno + seg->p->len)) {
        p = seg->p;
        seg->len -= p->len;
        seg->seqno += p->len;
        seg->p = p->next;
        assert(seg->p != NULL);
        /* XXX Maybe we will need to free zeropcopy pbuf in different way */
        external_tcp_tx_pbuf_free(pcb, p);
        ++count;
    }
    if (TCP_SEQ_GT(ackno, seg->seqno)) {
        len = ackno - seg->seqno;
        seg->p->payload = (char *)seg->p->payload + len;
        seg->len -= len;
        seg->p->len -= len;
        seg->p->tot_len -= len;
        seg->seqno = ackno;
    }
    seg->tcphdr->seqno = htonl(seg->seqno);

    return count;
}

static void ack_partial_or_whole_segment(struct tcp_pcb *pcb, u32_t ackno, struct tcp_seg **seg)
{
    struct tcp_seg *whole_seg_to_ack;
    while ((*seg) != NULL && TCP_SEQ_GT(ackno, (*seg)->seqno)) {
        if (TCP_SEQ_LT(ackno, (*seg)->seqno + TCP_SEGLEN((*seg)))) {
            if ((*seg)->tcp_flags & TCP_FIN) {
                // Avoid shrinking a segment with the FIN flag not to handle corner cases.
                // The most challenging scenario is ACK for the entire data, but not the FIN. In
                // this case the shrink implementation removes all the pbufs from the segment
                // what leads to a segfault eventually.
                // Let's keep the whole segment and retransmit the duplicate data if needed
                // until the shrink implementations are improved or removed by arch update.
                break;
            }
            // Ack partial TCP segment
            u32_t removed = (*seg)->flags & TF_SEG_OPTS_ZEROCOPY
                ? tcp_shrink_zc_segment(pcb, (*seg), ackno)
                : tcp_shrink_segment(pcb, (*seg), ackno);
            pcb->snd_queuelen -= removed;
            break;
        }

        whole_seg_to_ack = (*seg);
        (*seg) = (*seg)->next;

        /* Prevent ACK for FIN to generate a sent event */
        if ((pcb->acked != 0) && ((whole_seg_to_ack->tcp_flags & TCP_FIN) != 0)) {
            pcb->acked--;
        }

        pcb->snd_queuelen -= pbuf_clen(whole_seg_to_ack->p);
        tcp_tx_seg_free(pcb, whole_seg_to_ack);
    }
}

/**
 * Called by tcp_process. Checks if the given segment is an ACK for outstanding
 * data, and if so frees the memory of the buffered data. Next, is places the
 * segment on any of the receive queues (pcb->recved or pcb->ooseq). If the segment
 * is buffered, the pbuf is referenced by pbuf_ref so that it will not be freed until
 * i it has been removed from the buffer.
 *
 * If the incoming segment constitutes an ACK for a segment that was used for RTT
 * estimation, the RTT is estimated here as well.
 *
 * Called from tcp_process().
 */
static void tcp_receive(struct tcp_pcb *pcb, tcp_in_data *in_data)
{
    struct tcp_seg *next;
#if TCP_QUEUE_OOSEQ
    struct tcp_seg *prev, *cseg;
#endif /* TCP_QUEUE_OOSEQ */
    struct pbuf *p;
    s16_t m;
    u32_t right_wnd_edge;
    u32_t new_tot_len;
    int found_dupack = 0;
    s8_t persist = 0;

    if (in_data->flags & TCP_ACK) {
        if (pcb->unacked) {
            __builtin_prefetch(pcb->unacked->p);
        }
        right_wnd_edge = pcb->snd_wnd + pcb->snd_wl2;

        /* Update window. */
        if (TCP_SEQ_LT(pcb->snd_wl1, in_data->seqno) ||
            (pcb->snd_wl1 == in_data->seqno && TCP_SEQ_LT(pcb->snd_wl2, in_data->ackno)) ||
            (pcb->snd_wl2 == in_data->ackno &&
             SND_WND_SCALE(pcb, in_data->tcphdr->wnd) > pcb->snd_wnd)) {
            pcb->snd_wnd = SND_WND_SCALE(
                pcb, in_data->tcphdr->wnd); // Which means: tcphdr->wnd << pcb->snd_scale;
            /* keep track of the biggest window announced by the remote host to calculate
            the maximum segment size */
            if (pcb->snd_wnd_max < pcb->snd_wnd) {
                pcb->snd_wnd_max = pcb->snd_wnd;
            }
            pcb->snd_wl1 = in_data->seqno;
            pcb->snd_wl2 = in_data->ackno;
            if (pcb->snd_wnd == 0) {
                if (pcb->persist_backoff == 0) {
                    persist = 1;
                }
            } else if (pcb->persist_backoff > 0) {
                /* stop persist timer */
                pcb->persist_backoff = 0;
            }
            LWIP_DEBUGF(TCP_WND_DEBUG, ("tcp_receive: window update %" U16_F "\n", pcb->snd_wnd));
#if TCP_WND_DEBUG
        } else {
            if (pcb->snd_wnd != in_data->tcphdr->wnd) {
                LWIP_DEBUGF(
                    TCP_WND_DEBUG,
                    ("tcp_receive: no window update lastack %" U32_F " ackno %" U32_F " wl1 %" U32_F
                     " seqno %" U32_F " wl2 %" U32_F "\n",
                     pcb->lastack, in_data->ackno, pcb->snd_wl1, in_data->seqno, pcb->snd_wl2));
            }
#endif /* TCP_WND_DEBUG */
        }

        /* (From Stevens TCP/IP Illustrated Vol II, p970.) Its only a
         * duplicate ack if:
         * 1) It doesn't ACK new data
         * 2) length of received packet is zero (i.e. no payload)
         * 3) the advertised window hasn't changed
         * 4) There is outstanding unacknowledged data (retransmission timer running)
         * 5) The ACK is == biggest ACK sequence number so far seen (snd_una)
         *
         * If it passes all five, should process as a dupack:
         * a) dupacks < 3: do nothing
         * b) dupacks == 3: fast retransmit
         * c) dupacks > 3: increase cwnd
         *
         * If it only passes 1-3, should reset dupack counter (and add to
         * stats, which we don't do in lwIP)
         *
         * If it only passes 1, should reset dupack counter
         *
         */

        /* Clause 1 */
        if (TCP_SEQ_LEQ(in_data->ackno, pcb->lastack)) {
            pcb->acked = 0;
            /* Clause 2 */
            if (in_data->tcplen == 0) {
                /* Clause 3 */
                if (pcb->snd_wl2 + pcb->snd_wnd == right_wnd_edge) {
                    /* Clause 4 */
                    if (pcb->rtime >= 0) {
                        /* Clause 5 */
                        if (pcb->lastack == in_data->ackno) {
                            found_dupack = 1;
                            if ((u8_t)(pcb->dupacks + 1) > pcb->dupacks) {
                                ++pcb->dupacks;
                            }
                            if (pcb->dupacks > 3) {
#if TCP_CC_ALGO_MOD
                                cc_ack_received(pcb, CC_DUPACK);
#else
                                /* Inflate the congestion window, but not if it means that
                                   the value overflows. */
                                if ((u32_t)(pcb->cwnd + pcb->mss) > pcb->cwnd) {
                                    pcb->cwnd += pcb->mss;
                                }
#endif // TCP_CC_ALGO_MOD
                            } else if (pcb->dupacks == 3) {
                                /* Do fast retransmit */
                                tcp_rexmit_fast(pcb);
#if TCP_CC_ALGO_MOD
                                cc_ack_received(pcb, 0);
                                // cc_ack_received(pcb, CC_DUPACK);
#endif
                            }
                        }
                    }
                }
            }
            /* If Clause (1) or more is true, but not a duplicate ack, reset
             * count of consecutive duplicate acks */
            if (!found_dupack) {
                pcb->dupacks = 0;
            }
        } else if (TCP_SEQ_BETWEEN(in_data->ackno, pcb->lastack + 1, pcb->snd_nxt)) {
            /* We come here when the ACK acknowledges new data. */

            /* Reset the "IN Fast Retransmit" flag, since we are no longer
               in fast retransmit. Also reset the congestion window to the
               slow start threshold. */
            if (pcb->flags & TF_INFR) {
#if TCP_CC_ALGO_MOD
                cc_post_recovery(pcb);
#else
                pcb->cwnd = pcb->ssthresh;
#endif
                pcb->flags &= ~TF_INFR;
            }

            /* Reset the number of retransmissions. */
            pcb->nrtx = 0;

            /* Reset the retransmission time-out. */
            pcb->rto = (pcb->sa >> 3) + pcb->sv;

            /* Update the send buffer space.*/
            pcb->acked = in_data->ackno - pcb->lastack;

            pcb->snd_buf += pcb->acked;

            /* Reset the fast retransmit variables. */
            pcb->dupacks = 0;
            pcb->lastack = in_data->ackno;

            /* Update the congestion control variables (cwnd and ssthresh). */
            if (get_tcp_state(pcb) >= ESTABLISHED) {
#if TCP_CC_ALGO_MOD
                cc_ack_received(pcb, CC_ACK);
#else
                if (pcb->cwnd < pcb->ssthresh) {
                    if ((u32_t)(pcb->cwnd + pcb->mss) > pcb->cwnd) {
                        pcb->cwnd += pcb->mss;
                    }
                    LWIP_DEBUGF(TCP_CWND_DEBUG,
                                ("tcp_receive: slow start cwnd %" U32_F "\n", pcb->cwnd));
                } else {
                    u32_t new_cwnd = (pcb->cwnd + ((u32_t)pcb->mss * (u32_t)pcb->mss) / pcb->cwnd);
                    if (new_cwnd > pcb->cwnd) {
                        pcb->cwnd = new_cwnd;
                    }
                    LWIP_DEBUGF(TCP_CWND_DEBUG,
                                ("tcp_receive: congestion avoidance cwnd %" U32_F "\n", pcb->cwnd));
                }
#endif // TCP_CC_ALGO_MOD
            }
            LWIP_DEBUGF(
                TCP_INPUT_DEBUG,
                ("tcp_receive: ACK for %" U32_F ", unacked->seqno %" U32_F ":%" U32_F "\n",
                 in_data->ackno, pcb->unacked != NULL ? ntohl(pcb->unacked->tcphdr->seqno) : 0,
                 pcb->unacked != NULL
                     ? ntohl(pcb->unacked->tcphdr->seqno) + TCP_SEGLEN(pcb->unacked)
                     : 0));

            ack_partial_or_whole_segment(pcb, in_data->ackno, &(pcb->unacked));

            /* If there's nothing left to acknowledge, stop the retransmit
               timer, otherwise reset it to start again */
            if (pcb->unacked == NULL) {
                pcb->last_unacked = NULL;
                if (persist) {
                    /* start persist timer */
                    pcb->persist_cnt = 0;
                    pcb->persist_backoff = 1;
                }
                pcb->rtime = -1;
                pcb->ticks_since_data_sent = -1;
            } else {
                pcb->rtime = 0;
                pcb->ticks_since_data_sent = 0;
            }
        } else {
            /* Out of sequence ACK, didn't really ack anything */
            pcb->acked = 0;
            tcp_send_empty_ack(pcb);
        }

        /* We go through the ->unsent list to see if any of the segments
           on the list are acknowledged by the ACK. This may seem
           strange since an "unsent" segment shouldn't be acked. The
           rationale is that lwIP puts all outstanding segments on the
           ->unsent list after a retransmission, so these segments may
           in fact have been sent once. */
        ack_partial_or_whole_segment(pcb, in_data->ackno, &(pcb->unsent));

        if (pcb->unsent == NULL) {
            /* We have sent all pending segments, reflect it in last_unsent */
            pcb->last_unsent = NULL;
        }
        /* End of ACK for new data processing. */

        LWIP_DEBUGF(TCP_RTO_DEBUG,
                    ("tcp_receive: pcb->rttest %" U32_F " rtseq %" U32_F " ackno %" U32_F "\n",
                     pcb->rttest, pcb->rtseq, in_data->ackno));

        /* RTT estimation calculations. This is done by checking if the
           incoming segment acknowledges the segment we use to take a
           round-trip time measurement. */
        if (pcb->rttest && TCP_SEQ_LT(pcb->rtseq, in_data->ackno)) {
            /* diff between this shouldn't exceed 32K since this are tcp timer ticks
               and a round-trip shouldn't be that long... */
#if TCP_CC_ALGO_MOD
            pcb->t_rttupdated++;
#endif
            m = (s16_t)(tcp_ticks - pcb->rttest);

            LWIP_DEBUGF(TCP_RTO_DEBUG,
                        ("tcp_receive: experienced rtt %" U16_F " ticks (%" U16_F " msec).\n", m,
                         m * slow_tmr_interval));

            /* This is taken directly from VJs original code in his paper */
            m = m - (pcb->sa >> 3);
            pcb->sa += m;
            if (m < 0) {
                m = -m;
            }
            m = m - (pcb->sv >> 2);
            pcb->sv += m;
            pcb->rto = (pcb->sa >> 3) + pcb->sv;

            LWIP_DEBUGF(TCP_RTO_DEBUG,
                        ("tcp_receive: RTO %" U16_F " (%" U16_F " milliseconds)\n", pcb->rto,
                         pcb->rto * slow_tmr_interval));

            pcb->rttest = 0;
        }
    }

    /* If the incoming segment contains data, we must process it
       further unless the pcb already received a FIN.
       (RFC 793, chapter 3.9, "SEGMENT ARRIVES" in states CLOSE-WAIT, CLOSING,
       LAST-ACK and TIME-WAIT: "Ignore the segment text.") */
    if ((in_data->tcplen > 0) && (get_tcp_state(pcb) < CLOSE_WAIT)) {
        /* This code basically does three things:

        +) If the incoming segment contains data that is the next
        in-sequence data, this data is passed to the application. This
        might involve trimming the first edge of the data. The rcv_nxt
        variable and the advertised window are adjusted.

        +) If the incoming segment has data that is above the next
        sequence number expected (->rcv_nxt), the segment is placed on
        the ->ooseq queue. This is done by finding the appropriate
        place in the ->ooseq queue (which is ordered by sequence
        number) and trim the segment in both ends if needed. An
        immediate ACK is sent to indicate that we received an
        out-of-sequence segment.

        +) Finally, we check if the first segment on the ->ooseq queue
        now is in sequence (i.e., if rcv_nxt >= ooseq->seqno). If
        rcv_nxt > ooseq->seqno, we must trim the first edge of the
        segment on ->ooseq before we adjust rcv_nxt. The data in the
        segments that are now on sequence are chained onto the
        incoming segment so that we only need to call the application
        once.
        */

        /* First, we check if we must trim the first edge. We have to do
           this if the sequence number of the incoming segment is less
           than rcv_nxt, and the sequence number plus the length of the
           segment is larger than rcv_nxt. */
        /*    if (TCP_SEQ_LT(seqno, pcb->rcv_nxt)){
              if (TCP_SEQ_LT(pcb->rcv_nxt, seqno + tcplen)) {*/
        if (TCP_SEQ_BETWEEN(pcb->rcv_nxt, in_data->seqno + 1,
                            in_data->seqno + in_data->tcplen - 1)) {
            /* Trimming the first edge is done by pushing the payload
               pointer in the pbuf downwards. This is somewhat tricky since
               we do not want to discard the full contents of the pbuf up to
               the new starting point of the data since we have to keep the
               TCP header which is present in the first pbuf in the chain.

               What is done is really quite a nasty hack: the first pbuf in
               the pbuf chain is pointed to by inseg.p. Since we need to be
               able to deallocate the whole pbuf, we cannot change this
               inseg.p pointer to point to any of the later pbufs in the
               chain. Instead, we point the ->payload pointer in the first
               pbuf to data in one of the later pbufs. We also set the
               inseg.data pointer to point to the right place. This way, the
               ->p pointer will still point to the first pbuf, but the
               ->p->payload pointer will point to data in another pbuf.

               After we are done with adjusting the pbuf pointers we must
               adjust the ->data pointer in the seg and the segment
               length.*/

            u32_t off = pcb->rcv_nxt - in_data->seqno;
            p = in_data->inseg.p;
            LWIP_ASSERT("inseg.p != NULL", in_data->inseg.p);
            if (in_data->inseg.p->len < off) {
                LWIP_ASSERT("pbuf too short!", (((s32_t)in_data->inseg.p->tot_len) >= off));
                new_tot_len = in_data->inseg.p->tot_len - off;
                while (p->len < off) {
                    off -= p->len;
                    p->tot_len = new_tot_len;
                    p->len = 0;
                    p = p->next;
                }
                if (pbuf_header(p, -off)) {
                    /* Do we need to cope with this failing?  Assert for now */
                    LWIP_ASSERT("pbuf_header failed", 0);
                }
            } else {
                if (pbuf_header(in_data->inseg.p, -off)) {
                    /* Do we need to cope with this failing?  Assert for now */
                    LWIP_ASSERT("pbuf_header failed", 0);
                }
            }
            in_data->inseg.len -= pcb->rcv_nxt - in_data->seqno;
            in_data->inseg.tcphdr->seqno = in_data->seqno = pcb->rcv_nxt;
        } else {
            if (TCP_SEQ_LT(in_data->seqno, pcb->rcv_nxt)) {
                /* the whole segment is < rcv_nxt */
                /* must be a duplicate of a packet that has already been correctly handled */

                LWIP_DEBUGF(TCP_INPUT_DEBUG,
                            ("tcp_receive: duplicate seqno %" U32_F "\n", in_data->seqno));
                tcp_ack_now(pcb);
            }
        }

        /* The sequence number must be within the window (above rcv_nxt
           and below rcv_nxt + rcv_wnd) in order to be further
           processed. */
        if (TCP_SEQ_BETWEEN(in_data->seqno, pcb->rcv_nxt, pcb->rcv_nxt + pcb->rcv_wnd - 1)) {
            if (pcb->rcv_nxt == in_data->seqno) {
                /* The incoming segment is the next in sequence. We check if
                   we have to trim the end of the segment and update rcv_nxt
                   and pass the data to the application. */
                in_data->tcplen = TCP_TCPLEN(&in_data->inseg);

                if (in_data->tcplen > pcb->rcv_wnd) {
                    LWIP_DEBUGF(TCP_INPUT_DEBUG,
                                ("tcp_receive: other end overran receive window"
                                 "seqno %" U32_F " len %" U16_F " right edge %" U32_F "\n",
                                 in_data->seqno, in_data->tcplen, pcb->rcv_nxt + pcb->rcv_wnd));
                    if (TCPH_FLAGS(in_data->inseg.tcphdr) & TCP_FIN) {
                        /* Must remove the FIN from the header as we're trimming
                         * that byte of sequence-space from the packet */
                        TCPH_FLAGS_SET(in_data->inseg.tcphdr,
                                       TCPH_FLAGS(in_data->inseg.tcphdr) & ~TCP_FIN);
                    }
                    /* Adjust length of segment to fit in the window. */
                    in_data->inseg.len = pcb->rcv_wnd;
                    if (TCPH_FLAGS(in_data->inseg.tcphdr) & TCP_SYN) {
                        in_data->inseg.len -= 1;
                    }
                    pbuf_realloc(in_data->inseg.p, in_data->inseg.len);
                    in_data->tcplen = TCP_TCPLEN(&in_data->inseg);
                    LWIP_ASSERT(
                        "tcp_receive: segment not trimmed correctly to rcv_wnd\n",
                        (in_data->seqno + in_data->tcplen) == (pcb->rcv_nxt + pcb->rcv_wnd));
                }
#if TCP_QUEUE_OOSEQ
                /* Received in-sequence data, adjust ooseq data if:
                   - FIN has been received or
                   - inseq overlaps with ooseq */
                if (pcb->ooseq != NULL) {
                    if (TCPH_FLAGS(in_data->inseg.tcphdr) & TCP_FIN) {
                        LWIP_DEBUGF(TCP_INPUT_DEBUG,
                                    ("tcp_receive: received in-order FIN, binning ooseq queue\n"));
                        /* Received in-order FIN means anything that was received
                         * out of order must now have been received in-order, so
                         * bin the ooseq queue */
                        while (pcb->ooseq != NULL) {
                            struct tcp_seg *old_ooseq = pcb->ooseq;
                            pcb->ooseq = pcb->ooseq->next;
                            tcp_seg_free(pcb, old_ooseq);
                        }
                    } else {
                        next = pcb->ooseq;
                        /* Remove all segments on ooseq that are covered by inseg already.
                         * FIN is copied from ooseq to inseg if present. */
                        while (next &&
                               TCP_SEQ_GEQ(in_data->seqno + in_data->tcplen,
                                           next->tcphdr->seqno + next->len)) {
                            /* inseg cannot have FIN here (already processed above) */
                            if (TCPH_FLAGS(next->tcphdr) & TCP_FIN &&
                                (TCPH_FLAGS(in_data->inseg.tcphdr) & TCP_SYN) == 0) {
                                TCPH_SET_FLAG(in_data->inseg.tcphdr, TCP_FIN);
                                in_data->tcplen = TCP_TCPLEN(&in_data->inseg);
                            }
                            prev = next;
                            next = next->next;
                            tcp_seg_free(pcb, prev);
                        }
                        /* Now trim right side of inseg if it overlaps with the first
                         * segment on ooseq */
                        if (next &&
                            TCP_SEQ_GT(in_data->seqno + in_data->tcplen, next->tcphdr->seqno)) {
                            /* inseg cannot have FIN here (already processed above) */
                            in_data->inseg.len = (u32_t)(next->tcphdr->seqno - in_data->seqno);
                            if (TCPH_FLAGS(in_data->inseg.tcphdr) & TCP_SYN) {
                                in_data->inseg.len -= 1;
                            }
                            pbuf_realloc(in_data->inseg.p, in_data->inseg.len);
                            in_data->tcplen = TCP_TCPLEN(&in_data->inseg);
                            LWIP_ASSERT(
                                "tcp_receive: segment not trimmed correctly to ooseq queue\n",
                                (in_data->seqno + in_data->tcplen) ==
                                    next->in_data->tcphdr->in_data->seqno);
                        }
                        pcb->ooseq = next;
                    }
                }
#endif /* TCP_QUEUE_OOSEQ */

                pcb->rcv_nxt = in_data->seqno + in_data->tcplen;

                /* Update the receiver's (our) window. */
                LWIP_ASSERT("tcp_receive: tcplen > rcv_wnd\n", pcb->rcv_wnd >= in_data->tcplen);
                pcb->rcv_wnd -= in_data->tcplen;

                tcp_update_rcv_ann_wnd(pcb);

                /* If there is data in the segment, we make preparations to
                   pass this up to the application. The ->recv_data variable
                   is used for holding the pbuf that goes to the
                   application. The code for reassembling out-of-sequence data
                   chains its data on this pbuf as well.

                   If the segment was a FIN, we set the TF_GOT_FIN flag that will
                   be used to indicate to the application that the remote side has
                   closed its end of the connection. */
                if (in_data->inseg.p->tot_len > 0) {
                    in_data->recv_data = in_data->inseg.p;
                    /* Since this pbuf now is the responsibility of the
                       application, we delete our reference to it so that we won't
                       (mistakingly) deallocate it. */
                    in_data->inseg.p = NULL;
                }
                if (TCPH_FLAGS(in_data->inseg.tcphdr) & TCP_FIN) {
                    LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_receive: received FIN.\n"));
                    in_data->recv_flags |= TF_GOT_FIN;
                }

#if TCP_QUEUE_OOSEQ
                /* We now check if we have segments on the ->ooseq queue that
                   are now in sequence. */
                while (pcb->ooseq != NULL && pcb->ooseq->tcphdr->seqno == pcb->rcv_nxt) {

                    cseg = pcb->ooseq;
                    in_data->seqno = pcb->ooseq->tcphdr->seqno;

                    pcb->rcv_nxt += TCP_TCPLEN(cseg);
                    LWIP_ASSERT("tcp_receive: ooseq tcplen > rcv_wnd\n",
                                pcb->rcv_wnd >= TCP_TCPLEN(cseg));
                    pcb->rcv_wnd -= TCP_TCPLEN(cseg);

                    tcp_update_rcv_ann_wnd(pcb);

                    if (cseg->p->tot_len > 0) {
                        /* Chain this pbuf onto the pbuf that we will pass to
                           the application. */
                        if (in_data->recv_data) {
                            pbuf_cat(in_data->recv_data, cseg->p);
                        } else {
                            in_data->recv_data = cseg->p;
                        }
                        cseg->p = NULL;
                    }
                    if (TCPH_FLAGS(cseg->tcphdr) & TCP_FIN) {
                        LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_receive: dequeued FIN.\n"));
                        in_data->recv_flags |= TF_GOT_FIN;
                        if (get_tcp_state(pcb) ==
                            ESTABLISHED) { /* force passive close or we can move to active close */
                            set_tcp_state(pcb, CLOSE_WAIT);
                        }
                    }

                    pcb->ooseq = cseg->next;
                    tcp_seg_free(pcb, cseg);
                }
#endif /* TCP_QUEUE_OOSEQ */

                /* Acknowledge the segment(s). */
                if ((in_data->recv_data && in_data->recv_data->next) ||
                    tcp_quickack(pcb, in_data)) {
                    tcp_ack_now(pcb);
                } else {
                    tcp_ack(pcb);
                }

            } else {
                /* We get here if the incoming segment is out-of-sequence. */
                tcp_send_empty_ack(pcb);
#if TCP_QUEUE_OOSEQ
                /* Suppress coverity warning of uninit array during tcp_seg_copy(). */
                memset(in_data->inseg.l2_l3_tcphdr_zc, 0, sizeof(in_data->inseg.l2_l3_tcphdr_zc));
                /* We queue the segment on the ->ooseq queue. */
                if (pcb->ooseq == NULL) {
                    pcb->ooseq = tcp_seg_copy(pcb, &in_data->inseg);
                } else {
                    /* If the queue is not empty, we walk through the queue and
                       try to find a place where the sequence number of the
                       incoming segment is between the sequence numbers of the
                       previous and the next segment on the ->ooseq queue. That is
                       the place where we put the incoming segment. If needed, we
                       trim the second edges of the previous and the incoming
                       segment so that it will fit into the sequence.

                       If the incoming segment has the same sequence number as a
                       segment on the ->ooseq queue, we discard the segment that
                       contains less data. */

                    prev = NULL;
                    for (next = pcb->ooseq; next != NULL; next = next->next) {
                        if (in_data->seqno == next->tcphdr->seqno) {
                            /* The sequence number of the incoming segment is the
                               same as the sequence number of the segment on
                               ->ooseq. We check the lengths to see which one to
                               discard. */
                            if (in_data->inseg.len > next->len) {
                                /* The incoming segment is larger than the old
                                   segment. We replace some segments with the new
                                   one. */
                                cseg = tcp_seg_copy(pcb, &in_data->inseg);
                                if (cseg != NULL) {
                                    if (prev != NULL) {
                                        prev->next = cseg;
                                    } else {
                                        pcb->ooseq = cseg;
                                    }
                                    tcp_oos_insert_segment(pcb, cseg, next, in_data);
                                }
                                break;
                            } else {
                                /* Either the lenghts are the same or the incoming
                                   segment was smaller than the old one; in either
                                   case, we ditch the incoming segment. */
                                break;
                            }
                        } else {
                            if (prev == NULL) {
                                if (TCP_SEQ_LT(in_data->seqno, next->tcphdr->seqno)) {
                                    /* The sequence number of the incoming segment is lower
                                       than the sequence number of the first segment on the
                                       queue. We put the incoming segment first on the
                                       queue. */
                                    cseg = tcp_seg_copy(pcb, &in_data->inseg);
                                    if (cseg != NULL) {
                                        pcb->ooseq = cseg;
                                        tcp_oos_insert_segment(pcb, cseg, next, in_data);
                                    }
                                    break;
                                }
                            } else {
                                /*if (TCP_SEQ_LT(prev->tcphdr->seqno, seqno) &&
                                  TCP_SEQ_LT(seqno, next->tcphdr->seqno)) {*/
                                if (TCP_SEQ_BETWEEN(in_data->seqno, prev->tcphdr->seqno + 1,
                                                    next->tcphdr->seqno - 1)) {
                                    /* The sequence number of the incoming segment is in
                                       between the sequence numbers of the previous and
                                       the next segment on ->ooseq. We trim trim the previous
                                       segment, delete next segments that included in received
                                       segment and trim received, if needed. */
                                    cseg = tcp_seg_copy(pcb, &in_data->inseg);
                                    if (cseg != NULL) {
                                        if (TCP_SEQ_GT(prev->tcphdr->seqno + prev->len,
                                                       in_data->seqno)) {
                                            /* We need to trim the prev segment. */
                                            prev->len =
                                                (u32_t)(in_data->seqno - prev->tcphdr->seqno);
                                            pbuf_realloc(prev->p, prev->len);
                                        }
                                        prev->next = cseg;
                                        tcp_oos_insert_segment(pcb, cseg, next, in_data);
                                    }
                                    break;
                                }
                            }
                            /* If the "next" segment is the last segment on the
                               ooseq queue, we add the incoming segment to the end
                               of the list. */
                            if (next->next == NULL &&
                                TCP_SEQ_GT(in_data->seqno, next->tcphdr->seqno)) {
                                if (TCPH_FLAGS(next->tcphdr) & TCP_FIN) {
                                    /* segment "next" already contains all data */
                                    break;
                                }
                                next->next = tcp_seg_copy(pcb, &in_data->inseg);
                                if (next->next != NULL) {
                                    if (TCP_SEQ_GT(next->tcphdr->seqno + next->len,
                                                   in_data->seqno)) {
                                        /* We need to trim the last segment. */
                                        next->len = (u32_t)(in_data->seqno - next->tcphdr->seqno);
                                        pbuf_realloc(next->p, next->len);
                                    }
                                    /* check if the remote side overruns our receive window */
                                    if (TCP_SEQ_GT(in_data->seqno + in_data->tcplen,
                                                   pcb->rcv_nxt + pcb->rcv_wnd)) {
                                        LWIP_DEBUGF(TCP_INPUT_DEBUG,
                                                    ("tcp_receive: other end overran receive window"
                                                     "seqno %" U32_F " len %" U16_F
                                                     " right edge %" U32_F "\n",
                                                     in_data->seqno, in_data->tcplen,
                                                     pcb->rcv_nxt + pcb->rcv_wnd));
                                        if (TCPH_FLAGS(next->next->tcphdr) & TCP_FIN) {
                                            /* Must remove the FIN from the header as we're trimming
                                             * that byte of sequence-space from the packet */
                                            TCPH_FLAGS_SET(
                                                next->next->tcphdr,
                                                TCPH_FLAGS(next->next->tcphdr) & ~TCP_FIN);
                                        }
                                        /* Adjust length of segment to fit in the window. */
                                        next->next->len =
                                            pcb->rcv_nxt + pcb->rcv_wnd - in_data->seqno;
                                        pbuf_realloc(next->next->p, next->next->len);
                                        in_data->tcplen = TCP_TCPLEN(next->next);
                                        LWIP_ASSERT("tcp_receive: segment not trimmed correctly to "
                                                    "rcv_wnd\n",
                                                    (in_data->seqno + in_data->tcplen) ==
                                                        (pcb->rcv_nxt + pcb->rcv_wnd));
                                    }
                                }
                                break;
                            }
                        }
                        prev = next;
                    }
                }
#endif /* TCP_QUEUE_OOSEQ */
            }
        } else {
            /* The incoming segment is not withing the window. */
            tcp_send_empty_ack(pcb);
        }
    } else {
        /* Segments with length 0 is taken care of here. Segments that
           fall out of the window are ACKed. */
        /*if (TCP_SEQ_GT(pcb->rcv_nxt, seqno) ||
          TCP_SEQ_GEQ(seqno, pcb->rcv_nxt + pcb->rcv_wnd)) {*/
        if (!TCP_SEQ_BETWEEN(in_data->seqno, pcb->rcv_nxt, pcb->rcv_nxt + pcb->rcv_wnd - 1)) {
            tcp_ack_now(pcb);
        }
    }
}

/**
 * Looks for TIMESTAMP option and returns its value.
 *
 * @param opts buffer with TCP options
 * @param opts_len size of the buffer
 * @param tsval TS value is stored by this pointer on success
 * @return true if the option is present and false otherwise
 */
static bool tcp_parseopt_ts(u8_t *opts, u16_t opts_len, u32_t *tsval)
{
#if LWIP_TCP_TIMESTAMPS
    u16_t c;

    for (c = 0; c < opts_len;) {
        switch (opts[c]) {
        case 0x08:
            /* TIMESTAMP */
            if (opts[c + 1] != 0x0A || c + 0x0A > opts_len) {
                /* Bad length */
                LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: bad length\n"));
                return false;
            }
            /* TCP timestamp option with valid length and in host byte order */
            *tsval = read32_be(&opts[c + 2]);
            return true;
        case 0x00:
            /* End of options. */
            return false;
        case 0x01:
            /* NOP option. */
            ++c;
            break;
        default:
            if (opts[c + 1] == 0) {
                LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: bad length\n"));
                /* If the length field is zero, the options are malformed
                   and we don't process them further. */
                return false;
            }
            /* All other options have a length field, so that we easily
               can skip past them. */
            c += opts[c + 1];
        }
    }
#endif
    return false;
}

/**
 * Parses the options contained in the incoming segment.
 *
 * Called from tcp_listen_input(), tcp_process() and tcp_pcb_reuse().
 * Currently, only the MSS, window scaling and TIMESTAMP options are
 * supported!
 *
 * @param pcb the tcp_pcb for which a segment arrived
 */
static void tcp_parseopt(struct tcp_pcb *pcb, tcp_in_data *in_data)
{
    u16_t c, max_c;
    u16_t mss;
    u16_t snd_mss;
    u8_t *opts, opt;
#if LWIP_TCP_TIMESTAMPS
    u32_t tsval;
#endif

    opts = (u8_t *)in_data->tcphdr + TCP_HLEN;

    /* Parse the TCP MSS option, if present. */
    if (TCPH_HDRLEN(in_data->tcphdr) > 0x5) {
        max_c = (TCPH_HDRLEN(in_data->tcphdr) - 5) << 2;
        for (c = 0; c < max_c;) {
            opt = opts[c];
            switch (opt) {
            case 0x00:
                /* End of options. */
                LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: EOL\n"));
                return;
            case 0x01:
                /* NOP option. */
                ++c;
                LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: NOP\n"));
                break;
            case 0x02:
                LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: MSS\n"));
                if (opts[c + 1] != 0x04 || c + 0x04 > max_c) {
                    /* Bad length */
                    LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: bad length\n"));
                    return;
                }
                /* Check if the incoming flag is SYN. */
                if (in_data->flags & TCP_SYN) {
                    /* An MSS option with the right option length. */
                    mss = (opts[c + 2] << 8) | opts[c + 3];
                    /* Limit the mss to the configured TCP_MSS and prevent division by zero */
                    snd_mss = ((mss > pcb->advtsd_mss) || (mss == 0)) ? pcb->advtsd_mss : mss;
                    UPDATE_PCB_BY_MSS(pcb, snd_mss);
                }
                /* Advance to next option */
                c += 0x04;
                break;
            case 0x03:
                LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: WND SCALE\n"));
                if (opts[c + 1] != 0x03 || (c + 0x03 > max_c)) {
                    /* Bad length */
                    LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: bad length\n"));
                    return;
                }
                /* If syn was received with wnd scale option,
                   activate wnd scale opt, but only if this is not a retransmission */
                if (enable_wnd_scale && (in_data->flags & TCP_SYN) &&
                    !(pcb->flags & TF_WND_SCALE)) {
                    pcb->snd_scale = opts[c + 2] > 14U ? 14U : opts[c + 2];
                    pcb->rcv_scale = rcv_wnd_scale;
                    pcb->flags |= TF_WND_SCALE;
                }
                /* Advance to next option */
                c += 0x03;
                break;
#if LWIP_TCP_TIMESTAMPS
            case 0x08:
                LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: TS\n"));
                if (opts[c + 1] != 0x0A || c + 0x0A > max_c) {
                    /* Bad length */
                    LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: bad length\n"));
                    return;
                }
                /* TCP timestamp option with valid length */
                tsval = read32_be(&opts[c + 2]);
                if (in_data->flags & TCP_SYN) {
                    if (pcb->enable_ts_opt) {
                        pcb->ts_recent = tsval;
                        pcb->flags |= TF_TIMESTAMP;
                    }
                } else if (TCP_SEQ_BETWEEN(pcb->ts_lastacksent, in_data->seqno,
                                           in_data->seqno + in_data->tcplen)) {
                    pcb->ts_recent = tsval;
                }
                /* Advance to next option */
                c += 0x0A;
                break;
#endif
            default:
                LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: other\n"));
                if (opts[c + 1] == 0) {
                    LWIP_DEBUGF(TCP_INPUT_DEBUG, ("tcp_parseopt: bad length\n"));
                    /* If the length field is zero, the options are malformed
                       and we don't process them further. */
                    return;
                }
                /* All other options have a length field, so that we easily
                   can skip past them. */
                c += opts[c + 1];
            }
        }
    }
}

/*
 *  mul_events.h: MUL event handling 
 *  Copyright (C) 2012, Dipjyoti Saikia <dipjyoti.saikia@gmail.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef __MUL_EVENTS_H__
#define __MUL_EVENTS_H__
#include "cc_of_lib.h"

/* Cast to struct event */
#define C_EVENT(x) ((struct event *)(x))

void    c_write_event_sched(void *conn_arg);
int     c_worker_event_new_conn(void *ctx_arg, void *msg_arg);
void    c_switch_thread_read(evutil_socket_t fd, short events, void *arg);
void    c_accept(evutil_socket_t listener, short event, void *arg);
void    c_app_accept(evutil_socket_t listener, short event, void *arg);
void    c_aux_app_accept(evutil_socket_t listener, short event, void *arg);
void    c_worker_ipc_read(evutil_socket_t listener, short event, void *arg);
void    c_per_worker_timer_event(evutil_socket_t fd, short event, void *arg);
void    c_switch_thread_write_event(evutil_socket_t fd, short events, void *arg);

#define HAVE_SG_TX 1

#ifdef HAVE_SG_TX
//Kajal: This is also a socket write function: relpace with library call
static inline void
c_thread_sg_tx_sync(void *conn_arg)
{
    c_conn_t *conn = conn_arg;

    c_wr_lock(&conn->conn_lock);
    c_socket_write_nonblock_sg_loop(conn, c_write_event_sched);
    c_wr_unlock(&conn->conn_lock);
}

// Kajal: This is where the packet will get written in the library queue
// Call the library API here, the message is in the cbuf *b
static inline void
c_thread_tx(void *conn, struct cbuf *b, bool only_q, uint64_t datapath_id)
{
	size_t sz = 0;
	c_log_debug("(%s) Sending packet to library %lu\n", 
				__FUNCTION__, datapath_id);

	sz = (size_t)b->len;
    cc_of_send_pkt(datapath_id, 0/*aux-id*/,(void *)b->data, sz);
    //c_switch_t *sw = sw_arg;
    
    // call the library API 
//    cc_of_send_pkt(datapath_id, 0/*aux-id is not supported here*/, 
//                   (void *)b->data, b->len);
/*

    c_wr_lock(&conn->conn_lock);
    if (cbuf_list_queue_len(&conn->tx_q)  > C_TX_BUF_SZ) {
        c_wr_unlock(&conn->conn_lock);
        free_cbuf(b);
        return;
    }

    cbuf_list_queue_tail(&conn->tx_q, b);

    if (!only_q) {
        c_socket_write_nonblock_loop(conn, c_write_event_sched);
    }

    c_wr_unlock(&conn->conn_lock);
    */

}
#else
static inline void
c_thread_sg_tx_sync(void *conn_arg UNUSED)
{
    return;
}

static inline void
c_thread_tx(void *conn_arg, struct cbuf *b, bool only_q UNUSED, uint64_t datapath_id)
{
    //c_switch_t *sw = sw_arg;

    // call the library API 
//    cc_of_send_pkt(sw->datapath_id, 0/*aux-id is not supported here*/, 
//                   (void *)b->data, b->len/* len of the data */);
	c_log_debug("(%s) Sending packet to library %llu\n", 
				__FUNCTION__, datapath_id);

    cc_of_send_pkt(datapath_id, 0/*aux-id is not supported here*/, 
                   (void *)b->data,(size_t)b->len/* len of the data */);
/*
    c_wr_lock(&conn->conn_lock);
    if (cbuf_list_queue_len(&conn->tx_q)  > C_TX_BUF_SZ) {
        c_wr_unlock(&conn->conn_lock);
        free_cbuf(b);
        return;
    }

    cbuf_list_queue_tail(&conn->tx_q, b);

    c_socket_write_nonblock_loop(conn, c_write_event_sched);

    c_wr_unlock(&conn->conn_lock);
*/
}

#endif

#endif

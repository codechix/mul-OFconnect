/*
 *  mul_thread_core.c: MUL threading infrastructure 
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
#include "mul.h"

bool callbk_executed = FALSE;
void *c_thread_main(void *arg);
int  c_vty_thread_run(void *arg);
static int
mul_cc_thread_event_loop_lib_support(struct c_main_ctx *main_ctx);

int
c_set_thread_dfl_affinity(void)
{
    extern ctrl_hdl_t ctrl_hdl;
    cpu_set_t cpu;

    /* Set cpu affinity */
    CPU_ZERO(&cpu);
    CPU_SET(ctrl_hdl.n_threads + ctrl_hdl.n_appthreads, &cpu);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu);

    return 0;
}

/* TODO : Better Algo */
int
c_get_new_switch_worker(struct c_main_ctx *m_ctx) 
{
    m_ctx->switch_lb_hint = (m_ctx->switch_lb_hint + 1) % m_ctx->nthreads;
    return m_ctx->switch_lb_hint;
}

int
c_get_new_app_worker(struct c_main_ctx *m_ctx) 
{
    m_ctx->app_lb_hint = (m_ctx->app_lb_hint + 1) % m_ctx->n_appthreads;
    return m_ctx->app_lb_hint;
}

static void *
c_alloc_thread_ctx(struct thread_alloc_args *args)
{
    void *ctx;

    switch(args->thread_type) {
    case THREAD_MAIN: 
        {
            struct c_main_ctx *m_ctx;

	        c_log_debug("(%s) nthreads:%d", __FUNCTION__, args->nthreads);
            assert(args->n_appthreads >= 0 && 
                   args->n_appthreads <= C_MAX_APP_THREADS);
            m_ctx = calloc(1, sizeof(struct c_main_ctx));      
            assert(m_ctx);

            ctx = m_ctx;
            m_ctx->nthreads = 0; 
            m_ctx->n_appthreads = args->n_appthreads; 
            m_ctx->cmn_ctx.thread_type = args->thread_type; 
	        // In the main thread, the cmn_ctx has the pointer to 
	        // main_ctrl_handler
            m_ctx->cmn_ctx.c_hdl = args->c_hdl;
            break;
        }
    case THREAD_WORKER:
        {
			/* This code should not get executed */
	        c_log_debug("Worker thread creation -- not to be executed !!!");
            struct c_worker_ctx *w_ctx;
            w_ctx = calloc(1, sizeof(struct c_worker_ctx));      
            assert(w_ctx);

            ctx = w_ctx;
            w_ctx->cmn_ctx.thread_type = args->thread_type;    
	        // The handler for the main controller thread is stored
	        // in the worker context 
            w_ctx->cmn_ctx.c_hdl = args->c_hdl;
	        // Each thread has its thread-idx for lookup
            w_ctx->thread_idx = args->thread_idx;
            break;
        }
    case THREAD_VTY:
        {
            struct c_vty_ctx *vty_ctx;
            vty_ctx = calloc(1, sizeof(struct c_vty_ctx));      
            assert(vty_ctx);

            ctx = vty_ctx;
            vty_ctx->cmn_ctx.thread_type = args->thread_type;    
            vty_ctx->cmn_ctx.c_hdl = args->c_hdl;
            break;
        }
    case THREAD_APP:
        {
            struct c_app_ctx *app_ctx;
            app_ctx = calloc(1, sizeof(struct c_app_ctx));      
            assert(app_ctx);

            ctx = app_ctx;
            app_ctx->cmn_ctx.thread_type = args->thread_type;    
            app_ctx->thread_idx = args->thread_idx;    
            app_ctx->cmn_ctx.c_hdl = args->c_hdl;
            break;
        }
    default:
        return NULL;

    }

    return ctx;
}

/*
 * C worker thread is initialized with the w_ctx
 * This logic is no longer needed.
static int
c_worker_thread_final_init(struct c_worker_ctx *w_ctx)
{
    cpu_set_t           cpu;
    char                ipc_path_str[64];
    struct timeval      tv = { C_PER_WORKER_TIMEO, 0 };

    w_ctx->cmn_ctx.base = event_base_new();
    assert(w_ctx->cmn_ctx.base);

    snprintf(ipc_path_str, 63, "%s%d", C_IPC_PATH, w_ctx->thread_idx);
    w_ctx->main_wrk_conn.rd_fd = open(ipc_path_str, O_RDONLY | O_NONBLOCK);
    assert(w_ctx->main_wrk_conn.rd_fd > 0);

    w_ctx->main_wrk_conn.rd_event = event_new(w_ctx->cmn_ctx.base, 
                                         w_ctx->main_wrk_conn.rd_fd,
                                         EV_READ|EV_PERSIST,
                                         c_worker_ipc_read, (void*)w_ctx);
    event_add(w_ctx->main_wrk_conn.rd_event, NULL);

    w_ctx->worker_timer_event = evtimer_new(w_ctx->cmn_ctx.base, 
                                            c_per_worker_timer_event, 
                                            (void *)w_ctx);
    evtimer_add(w_ctx->worker_timer_event, &tv);

    // Set cpu affinity 
    CPU_ZERO(&cpu);
    CPU_SET(w_ctx->thread_idx, &cpu);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu);

    w_ctx->cmn_ctx.run_state = THREAD_STATE_RUNNING;

    return 0;
}
*/

/**
 * This is where all the WORKER / APP / VTY threads get created
 */
static int
c_main_thread_final_init(struct c_main_ctx *m_ctx)
{
    evutil_socket_t             c_listener;
    //struct c_worker_ctx         *w_ctx, **w_ctx_slot;
    struct c_vty_ctx            *vty_ctx;
    struct c_app_ctx            *app_ctx, **app_ctx_slot;
    char                        ipc_path_str[64];
    int                         thread_idx;
    //ctrl_hdl_t                  *ctrl_hdl = m_ctx->cmn_ctx.c_hdl;
    struct thread_alloc_args    t_args = { 0, 0, 
                                           THREAD_WORKER, 
                                           0, 
                                           m_ctx->cmn_ctx.c_hdl };

    /* No event handling for worker threads as they are no-op
	* event handling is required for APP / VTY threads
	* Therefore, setting up event base is required */
    m_ctx->cmn_ctx.base = event_base_new();
    assert(m_ctx->cmn_ctx.base);

	/*
	m_ctx->worker_pool = calloc(m_ctx->nthreads, sizeof(void *));
	assert(m_ctx->worker_pool);

	// Worker thread creation
	for (thread_idx = 0; thread_idx < m_ctx->nthreads; thread_idx++) {
	// Indexing the worker thread in the main_ctx
	w_ctx_slot = c_tid_to_ctx_slot(m_ctx, thread_idx);

	t_args.thread_idx = thread_idx;
	w_ctx = c_alloc_thread_ctx(&t_args);
	assert(w_ctx);

	*w_ctx_slot = w_ctx;
	// Create Named pipes for IPC communication with main thread
	memset(ipc_path_str, 0, sizeof(ipc_path_str));
	snprintf(ipc_path_str, 63, "%s%d", C_IPC_PATH, thread_idx);
	if (mkfifo(ipc_path_str, S_IRUSR | S_IWUSR | S_IWGRP) == -1
	&& errno != EEXIST) {
		perror("");
		assert(0);
	}

	// Worker context is created for each thread, but, we will not
	// be using it.
	pthread_create(&w_ctx->cmn_ctx.thread, NULL, c_thread_main, w_ctx);

	// Save the fd for the named pipes in the worker context
	// Main ctx has the w_ctx saved. So main thread knows which is the
	// fd on which the IPC communication needs to begin
	w_ctx->main_wrk_conn.conn_type = C_CONN_TYPE_FILE;
	w_ctx->main_wrk_conn.fd = open(ipc_path_str, O_WRONLY);
	assert(w_ctx->main_wrk_conn.fd > 0);

		// Worker thread context is saved in the main_ctx and the controller handle
		ctrl_hdl->worker_ctx_list[thread_idx] = (void *)w_ctx;
	}

	// Switch listener
	// Kajal: No need to create a socket to listen on.
	// Library will use the callback defined in MUL to do processing.
	c_listener = c_server_socket_create(INADDR_ANY, C_LISTEN_PORT);

	assert(c_listener > 0);
	m_ctx->c_accept_event = event_new(m_ctx->cmn_ctx.base, -1,
	EV_READ|EV_PERSIST,
	cc_of_new_conn_event_handler, (void*)m_ctx);
	event_add(m_ctx->c_accept_event, NULL);
	*/
	c_log_debug("(%s) testing!!!\n", __FUNCTION__);

    m_ctx->app_pool = calloc(m_ctx->n_appthreads, sizeof(void *));
    assert(m_ctx->app_pool);

    /* Application thread creation */
    for (thread_idx = 0; thread_idx < m_ctx->n_appthreads; thread_idx++) {
        app_ctx_slot = c_tid_to_app_ctx_slot(m_ctx, thread_idx);

        t_args.thread_type = THREAD_APP;
        t_args.thread_idx = thread_idx;
        app_ctx = c_alloc_thread_ctx(&t_args);
        assert(app_ctx);

        *app_ctx_slot = app_ctx;

        memset(ipc_path_str, 0, sizeof(ipc_path_str));
        snprintf(ipc_path_str, 63, "%s%d", C_IPC_APP_PATH, thread_idx);
        if (mkfifo(ipc_path_str, S_IRUSR | S_IWUSR | S_IWGRP) == -1
            && errno != EEXIST) {
            perror("");
            assert(0);
        }

        pthread_create(&app_ctx->cmn_ctx.thread, NULL, c_thread_main, app_ctx);

        app_ctx->main_wrk_conn.conn_type = C_CONN_TYPE_FILE;
        app_ctx->main_wrk_conn.fd = open(ipc_path_str, O_WRONLY);
        assert(app_ctx->main_wrk_conn.fd > 0);
    }

    /* VTY thread creation */
    t_args.thread_type = THREAD_VTY;
    vty_ctx = c_alloc_thread_ctx(&t_args);
    assert(vty_ctx);
    pthread_create(&vty_ctx->cmn_ctx.thread, NULL, c_thread_main, vty_ctx);


    /* Application listener */
    c_listener = c_server_socket_create(INADDR_ANY, C_APP_LISTEN_PORT);
    assert(c_listener);
    m_ctx->c_app_accept_event = event_new(m_ctx->cmn_ctx.base, c_listener,
                                          EV_READ|EV_PERSIST,
                                          c_app_accept, (void*)m_ctx);
    event_add(m_ctx->c_app_accept_event, NULL);

    c_listener = c_server_socket_create(INADDR_ANY, C_APP_AUX_LISTEN_PORT);
    assert(c_listener);
    m_ctx->c_app_aux_accept_event= event_new(m_ctx->cmn_ctx.base, c_listener,
                                          EV_READ|EV_PERSIST,
                                          c_aux_app_accept, (void*)m_ctx);
    event_add(m_ctx->c_app_aux_accept_event, NULL);

    m_ctx->cmn_ctx.run_state = THREAD_STATE_RUNNING;

    c_set_thread_dfl_affinity();

    c_log_debug("%s: running tid(%u)", __FUNCTION__, (unsigned int)pthread_self());
	return 0;

}

// mul_cc_of_accept
// This is a callback which is called when the library does an accept.
// It will pass the controller a dummy dp-id (which is actually the sock-fd)
// This dp-id will be updated when the negitiation is done.
//
// This is where the hello will be triggered
int
mul_cc_of_accept(uint64_t dummy_dpid, uint8_t dummy_auxid,
				 uint32_t switch_ip, uint16_t switch_port)
{
    c_switch_t *new_switch = NULL;
	struct c_main_ctx *c_main_ctx = ctrl_hdl.main_ctx;

	c_log_debug("(%s) Accept received dpid:%lu auxid:0x%x \n", 
				__FUNCTION__, dummy_dpid, dummy_auxid);

    // A new connection needs to be estabilished
    // c_new_conn_to_thread(&ctrl_hdl->m_ctx, 0, true, false); 	

	// Pass the main handler
	// Here the switch state is SW_INIT 
	// Later on it changes state to REGISTERED
	new_switch = of_switch_alloc(c_main_ctx);	
	if(new_switch == NULL)
	{
		c_log_debug("New switch context NOT created !!!\n");
		return CC_OF_EMISC;
	}

	c_log_debug("(%s) New switch context created\n", __FUNCTION__);

	// Add the switch info to the main thread
	// Follow functioning in : c_worker_do_switch_add 
    new_switch->datapath_id = dummy_dpid;			
    new_switch->is_dummy_datapath_id = TRUE;			
	struct in_addr *switch_addr;
	switch_addr = (struct in_addr *)&switch_ip;
	snprintf(new_switch->conn.conn_str, C_CONN_DESC_SZ -1, "%s:%d",
			inet_ntoa(*switch_addr), switch_port);
	c_log_debug("(%s) switch:port str:%s\n", 
				__FUNCTION__, new_switch->conn.conn_str);
	new_switch->c_hdl = &ctrl_hdl;

	// We are inserting the new_switch ctx in the sw_list
	// t_data->sw_list = g_slist_append(t_data->sw_list, new_switch);	

	// Add the switch to the hashtable in the ctrl_hndl
	// The dummy dpid and the real datapath-id should not be same ...
	of_switch_add(new_switch);

	// Send hello
	// This should send the messsage to the library
	of_send_hello(new_switch);

	return 0; 
}

int 
mul_cc_of_delete(uint64_t dpid, uint8_t auxid) 
{
	c_switch_t *sw = NULL;
	// dp-id can be real or the dummy
	// Basically, the connection needs to be terminated
	
	// Lookup for the switch
//	sw = ;
	
	of_switch_del(sw);

	// Do I need to put ?
	of_switch_put(sw);
	
	return 0;
}

// Implementation of CC ONF function
// For now the parameters are based on MUL defines to compile
// These need to be changed when the library is integrated.
//
// Algo:
// As all packets are going to be handled by the main thread, 
// we will insert the packet in the main thread buffer. 
// The main thread will read messages from the buffer and process
// them based on if they are new connection or a data packet. 
//
// Note: Here we might have to check the impact on the control packets,
// if there are any timeouts or response dependencies to the library.
// 
// Also, as the OF-channel key is passed here, we know what channel the 
// message is for. But, I don't think we will be using this parameter.
//
// The main thread does not have c_conn_t struct defined for it. 
// We will have defined a global buffer for the main thread, which can
// be accessed by c_main_buf.

int
mul_cc_recv_pkt(uint64_t dp_id, uint8_t aux_id, void *of_msg, size_t msg_len)
{
    
	size_t len = 0;
    struct cbuf *b = NULL;

	c_log_debug("(%s) Recv received dpid:%lu auxid:%d msg_len:%d\n", 
				__FUNCTION__, dp_id, aux_id, msg_len);
    
	// The msg_len from the library is not working
	// Have a sufficient size buffer
	// Open flow messages are processed based on a valid header
	msg_len = 1024;
    if(cbuf_list_queue_len(&ctrl_hdl.c_main_buf_head) > (1024 * 1024)) 
    {
		// Throw an error
		c_log_err("(%s) Main thread buffer queue is full\n");
    }
    else
    {
		// There is a bug in the library or in the message it receives, 
		// message length is always going to be 0
		b = alloc_cbuf(msg_len);
		// The CBUF_SZ will always be allocated
		if(b == NULL)
		{
			c_log_err("(%s) Buffer node not allocated dp-id:0x%x aux-id:0x%x\n",
					  __FUNCTION__, dp_id, aux_id);
			return CC_OF_ENOMEM;
		}
		else
		{
			// if_msg should be freed by library assuming that 
			// buffer should copy it.
			
			// The dpid can be dummy-id or the real one
			b->dpid = dp_id;
			
			if(msg_len != 0)
			{
				struct cbuf *new = NULL;
				new = alloc_cbuf(msg_len);

				new->dpid = dp_id;

			    memcpy(new->data, of_msg, msg_len);	
				cbuf_put(new, msg_len);

				free(b);
				b = new;
			}

			// Debugging: print length
			//len = cbuf_list_queue_len(&ctrl_hdl.c_main_buf_head);
			//c_log_debug("(%s) BEFORE queue_len:%d\n", __FUNCTION__, len);

			// Insert buffer in queue	
			cbuf_list_queue_tail(&ctrl_hdl.c_main_buf_head, b);

			// Debugging: print length
			//len = cbuf_list_queue_len(&ctrl_hdl.c_main_buf_head);
			//c_log_debug("(%s) AFTER queue_len:%d\n", __FUNCTION__, len);
		}

		//usleep(1);
    }

    return 0;
}

// This loop should keep running
// This is where the messsages are inserted in the buffer queue

// Note:
// As main thread will be responsible for the handling
// Tying up the main thread to the worker context in the main thread
// is the key. The logic for the controller operates on ?
// So, once the information has been put into the ?? , then the logic
// can get the information from the c_switch_t->cmn_ctx where the 
// hashtable stores the DPID and switch information 
static int
mul_cc_thread_event_loop_lib_support(struct c_main_ctx *main_ctx)
{
	uint32_t curr_len = 0;
	uint32_t queue_len = 0;
    c_switch_t *sw = NULL;
    struct cbuf *b = NULL;
	//c_per_thread_dat_t *t_data = &main_ctx->thread_data;
    
    c_log_debug("(%s) tid(%u) \n", 
	            __FUNCTION__, (unsigned int)pthread_self());
    // instead of looping on the socket fd, the main thread
    // will be looping on the cbuf.
    
    // Not considering the application threads for now
    //return event_base_dispatch(cmn_ctx->base);

    // check cbuf
    // get first message from buffer and begin the processing.
    while(1)
    {	
		queue_len = cbuf_list_queue_len(&ctrl_hdl.c_main_buf_head);
		//c_log_debug("(%s) In while loop len:%d", __FUNCTION__, queue_len);	

        if(queue_len != 0)
        {

			// Get the message
			b = cbuf_list_dequeue(&ctrl_hdl.c_main_buf_head);
			curr_len = cbuf_list_queue_len(&ctrl_hdl.c_main_buf_head);
		    c_log_debug("(%s) curr queue len:%d", __FUNCTION__, curr_len);	
			if(b == NULL)
			{
				// Should never happen!!!
				c_log_debug("(%s) Message is NULL curr_queue_len:%d\n", 
							__FUNCTION__, curr_len);
				continue;
			}

			//c_log_debug("(%s) b_len:%d", __FUNCTION__, b->len);

			if(b->len)
			{
				if (!of_hdr_valid(b->data)) 
				{
					//c_log_err("(%s) Corrupted header dpid:%lu b_len:%d queue_len:%d", 
					//			__FUNCTION__, b->dpid, b->len, curr_len);
					continue;
					//return 0; 
				}
				else
				{
					struct ofp_header *h;
					h = (void *)b->data;
					//c_log_debug("(%s) xid:0x%x VALID!!!",__FUNCTION__, h->xid);
				}

				// sw = of_switch_alloc(main_ctx);

				// When the packet is recieved in the queue, it
				// will have a sw struct already allocated
				// Get it from the main thread hashtable
				//c_log_debug("(%s) Get the switch context dp_id:%lu len:%d\n", 
				//			__FUNCTION__, b->dpid, curr_len);

				sw = of_switch_get(&ctrl_hdl, b->dpid);
				if(sw != NULL)
				{
					// Call of_switch_recv	
					of_switch_recv_msg(sw, b);
				}
				else
				{
					// NULL context returned
					//c_log_debug("(%s) could not get switch context for dp_id:%lu\n", 
					//			 __FUNCTION__, b->dpid);
				}	

			}
        }

		// Let execution happen
		usleep(1000);
    }

    return 0;
}

static int
c_thread_event_loop(struct c_cmn_ctx *cmn_ctx)
{
    c_log_debug("%s: tid(%u)", __FUNCTION__, (unsigned int)pthread_self());
    return event_base_dispatch(cmn_ctx->base);
}

static int
c_main_thread_run(struct c_main_ctx *m_ctx)
{

    switch(m_ctx->cmn_ctx.run_state) {
    case THREAD_STATE_PRE_INIT:
	/* This is for spurious signal handling */
        signal(SIGPIPE, SIG_IGN);
        m_ctx->cmn_ctx.run_state = THREAD_STATE_FINAL_INIT;
        break;
    case THREAD_STATE_FINAL_INIT:
        return c_main_thread_final_init(m_ctx);
    case THREAD_STATE_RUNNING:
        return mul_cc_thread_event_loop_lib_support((void *)m_ctx);
    }
    return 0;
}

/*
static int
c_worker_thread_run(struct c_worker_ctx *w_ctx)
{
    switch(w_ctx->cmn_ctx.run_state) {
    case THREAD_STATE_PRE_INIT:
        signal(SIGPIPE, SIG_IGN);
        w_ctx->cmn_ctx.run_state = THREAD_STATE_FINAL_INIT;
        break;
    case THREAD_STATE_FINAL_INIT:
        return c_worker_thread_final_init(w_ctx);
    case THREAD_STATE_RUNNING:
        return c_thread_event_loop((void *)w_ctx);
    default:
        c_log_err("Unknown run state"); 
        break;
    }

    return 0;
}
*/

static int
c_app_thread_pre_init(struct c_app_ctx *app_ctx)
{
    char    ipc_path_str[64];

    signal(SIGPIPE, SIG_IGN);
    app_ctx->cmn_ctx.base = event_base_new();
    assert(app_ctx->cmn_ctx.base);

    snprintf(ipc_path_str, 63, "%s%d", C_IPC_APP_PATH, app_ctx->thread_idx);
    app_ctx->main_wrk_conn.rd_fd = open(ipc_path_str,
                                            O_RDONLY | O_NONBLOCK);
    assert(app_ctx->main_wrk_conn.rd_fd > 0);

    app_ctx->main_wrk_conn.rd_event = event_new(app_ctx->cmn_ctx.base,
                                         app_ctx->main_wrk_conn.rd_fd,
                                         EV_READ|EV_PERSIST,
                                         c_worker_ipc_read, (void*)app_ctx);
    event_add(app_ctx->main_wrk_conn.rd_event, NULL);
    app_ctx->cmn_ctx.run_state = THREAD_STATE_FINAL_INIT;

    return 0;
}

static int
c_app_thread_final_init(struct c_app_ctx *app_ctx)
{
    extern ctrl_hdl_t ctrl_hdl;
    cpu_set_t cpu;

    /* Set cpu affinity */
    CPU_ZERO(&cpu);
    CPU_SET(app_ctx->thread_idx + ctrl_hdl.n_threads, &cpu);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu);

    c_builtin_app_start(app_ctx);
    app_ctx->cmn_ctx.run_state = THREAD_STATE_RUNNING;

    return 0;
}

static int
c_app_thread_run(struct c_app_ctx *app_ctx)
{
    switch(app_ctx->cmn_ctx.run_state) {
    case THREAD_STATE_PRE_INIT:
        return c_app_thread_pre_init(app_ctx);
    case THREAD_STATE_FINAL_INIT:
        return c_app_thread_final_init(app_ctx);
    case THREAD_STATE_RUNNING:
        return c_thread_event_loop((void *)app_ctx);
    }
    return 0;
}

/** 
 * This function is run in a continuous while loop
 * The state machine for:
 * PRE-INIT
 * FINAL-INIT
 * RUNNING STATE
 * is managed here.   
 */
static int
c_thread_run(void *ctx)
{
    struct c_cmn_ctx *cmn_ctx = ctx;
    
    switch (cmn_ctx->thread_type) {
    case THREAD_MAIN:
    /* This is the main ctx */
       return c_main_thread_run(ctx);
    /* Worker threads are not needed any longer   
    case THREAD_WORKER:
       return c_worker_thread_run(ctx); 
	*/
    case THREAD_VTY:
       return c_vty_thread_run(ctx);
    case THREAD_APP:
       return c_app_thread_run(ctx);
    default:
        break;
    }

    return 0;
}

void *
c_thread_main(void *arg)
{
     C_THREAD_RUN(arg);     
}
    
int
c_thread_start(void *hdl, int nthreads, int n_appthreads)
{
    ctrl_hdl_t *ctrl_hdl = hdl;
    struct thread_alloc_args args = { 0/*number of switch threads*/, 
                                      n_appthreads, THREAD_MAIN, 0, hdl };

    // c_main_ctx and c_worker_ctx are different
    // c_main_ctx has the worker pool associated in 
    // struct c_worker_ctx **worker_pool;
    // The main context has the handler to the controller m_ctx->cmn_ctx.c_hdl

    // In this MUL model, we will be operating on the main_ctx and will add the switch
    // information in ctrl_hdl:
    // GHashTable *sw_hash_tbl;
    struct c_main_ctx *main_ctx = c_alloc_thread_ctx(&args);
    ctrl_hdl->main_ctx = (void *)main_ctx;

    /* This is with attribute of pre-init */
    pthread_create(&main_ctx->cmn_ctx.thread, NULL, c_thread_main, main_ctx);
    return 0;
}

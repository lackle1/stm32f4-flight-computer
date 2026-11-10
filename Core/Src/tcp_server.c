#include "my_tcp.h"
#include "tcp_server.h"

#include "lwip.h"
#include "tcp.h"
#include <stdbool.h>
#include <string.h>

enum tcp_server_states
{
    SS_NONE,
    SS_ACCEPTED,    // TCP connection is up, but no data has been received
    SS_RECEIVED,    // Server is receiving data
    SS_CLOSING      // Client has stopped sending, but the server may still have queued data to send
};

typedef struct tcp_server_struct
{
    u8_t state;
    u8_t retries;
    pcb_st *pcb;
} server_st;

static err_t _TCP_server_accept(void *arg, pcb_st *newpcb, err_t err);
static err_t _TCP_server_recv(void *arg, pcb_st *tpcb, struct pbuf *p, err_t err);
static void _TCP_server_close(pcb_st *tpcb, server_st *ss);
static void _TCP_server_send(pcb_st *tpcb, const void *data, u16_t len, bool copy_data, bool wait_for_more_data);
static void _TCP_server_handle(pcb_st *tpcb, struct pbuf* p);
static void _TCP_server_error(void *arg, err_t err);
// static err_t _TCP_server_sent();
// static err_t _TCP_server_poll();

/**
 * @brief  Initialises the TCP server
 *
 * @return Error status
 **/
static err_t _TCP_server_init()
{
    err_t ret_err;

    // Create TCP PCB
    pcb_st *tpcb;
    tpcb = tcp_new();
    if (tpcb == NULL)
    {
        return false;
    }

    // Bind to IP address and port 7
    ip_addr_t my_ip;
    IP_ADDR4(&my_ip, 192, 168, 1, 111);
    ret_err = tcp_bind(tpcb, &my_ip, 7);

    // If binding fails, free the memory
    if (ret_err != ERR_OK)
    {
        memp_free(MEMP_TCP_PCB, tpcb);
        return ret_err;
    }

    // Begin listening for a connection
    tpcb = tcp_listen(tpcb);

    // Register the server accept callback
    tcp_accept(tpcb, _TCP_server_accept);

    return ERR_OK;
}

/**
 * @brief  Public API function for starting the TCP server
 *
 * @return Error status
 **/
void TCP_server_init()
{
    _TCP_server_init();
}

/**
 * @brief  Implementation of the tcp_accept callback
 *
 * @param  arg not used
 * @param  newpcb pointer to newly created tcp connection struct
 * @param  err not used (always ERR_OK)
 * 
 * @return Error status
 **/
static err_t _TCP_server_accept(void *arg, pcb_st *newpcb, err_t err)
{
    err_t ret_err;
    server_st *ss;

    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(err);

    tcp_setprio(newpcb, TCP_PRIO_MIN);

    // Create server struct
    ss = (server_st *)mem_malloc(sizeof(server_st));
    if (ss != NULL)
    {
        ss->state = SS_ACCEPTED;
        ss->retries = 0;
        ss->pcb = newpcb;

        // Pass ss as argument and register callbacks
        tcp_arg(newpcb, ss);
        tcp_recv(newpcb, _TCP_server_recv);
        tcp_err(newpcb, _TCP_server_error);
        // tcp_poll(newpcb, _TCP_server_poll, 2);  // TODO: Once every second I believe, have to check

        ret_err = ERR_OK;
    }
    else
    {
        _TCP_server_close(newpcb, ss);
        ret_err = ERR_MEM;
    }

    return ret_err;
}

/**
 * @brief  Implementation of the tcp_recv callback
 *
 * @param  arg pointer to server struct
 * @param  newpcb pointer to tcp connection
 * @param  p pointer to received pbuf
 * @param  err error information regarding the received pbuf
 * 
 * @return Error status
 **/
static err_t _TCP_server_recv(void *arg, pcb_st *tpcb, struct pbuf *p, err_t err)
{
    server_st *ss;
    err_t ret_err;

    LWIP_ASSERT("arg != NULL", arg != NULL);

    ss = (server_st *)arg;

    // If buffer is empty, the client has closed the connection
    if (p == NULL)
    {
        ss->state = SS_CLOSING;
        _TCP_server_close(tpcb, ss);
        ret_err = ERR_OK;
    }
    // Error has occurred
    else if (err != ERR_OK)
    {
        // Free received pbuf
        pbuf_free(p);
        ret_err = err;
    }
    // Server has not received any data yet
    else if (ss->state == SS_ACCEPTED)
    {
        // Change state
        ss->state = SS_RECEIVED;

        // Inform stack data has been received
        tcp_recved(tpcb, p->tot_len);

        // Process data
        _TCP_server_handle(tpcb, p);
        pbuf_free(p);
        ret_err = ERR_OK;
    }
    // Not the first chunk of data that has been received
    else if (ss->state == SS_RECEIVED)
    {
        // Inform stack data has been received
        tcp_recved(tpcb, p->tot_len);

        // Process data
        _TCP_server_handle(tpcb, p);
        pbuf_free(p);
        ret_err = ERR_OK;
    }
    // Otherwise discard
    else
    {
        tcp_recved(tpcb, p->tot_len);
        pbuf_free(p);
        ret_err = ERR_OK;
    }

    return ret_err;
}

/**
 * @brief  Closes the connection.
 *
 * @param  tpcb pointer to pcb struct
 * @param  ss pointer to server struct
 **/
static void _TCP_server_close(pcb_st *tpcb, server_st *ss)
{
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);

    if (ss != NULL)
    {
        mem_free(ss);
    }

    tcp_close(tpcb);
}

/**
 * @brief  Writes data for sending, then uses tcp_output() to send it immediately.
 *
 * @param  tpcb pointer to pcb struct
 * @param  data pointer to data to send
 * @param  len length of data to send
 * @param  copy_data sets the TCP_WRITE_FLAG_COPY flag, meaning new memory will be allocated for the data to be copied into
 * @param  wait_for_more_data sets the TCP_WRITE_FLAG_MORE, meaning the PSH flag will not be set in the last segment. If the PSH bit is set, the receiving end will not wait 
 *         for the buffer to fill before processing
 **/
static void _TCP_server_send(pcb_st *tpcb, const void *data, u16_t len, bool copy_data, bool wait_for_more_data)
{
    // Wait until there is space on the buffer
    while (len > tcp_sndbuf(tpcb));

    // Set flags
    u8_t flags = (TCP_WRITE_FLAG_COPY & copy_data) | (TCP_WRITE_FLAG_MORE & wait_for_more_data);

    tcp_write(tpcb, data, len, flags);

    tcp_output(tpcb);
}

/**
 * @brief Handles received data and responds
 *
 * @param tpcb pointer to pcb struct
 * @param ss pointer to server struct
 **/
static void _TCP_server_handle(pcb_st *tpcb, struct pbuf *p)
{
    // Allocate a buffer the size of the current packet
    void *buf = mem_malloc(p->tot_len);

    // Retrieve data
    pbuf_copy_partial(p, buf, p->tot_len, 0);
    
    // Process data
    u8_t *str = (u8_t *)buf;
    for (u16_t i = 0; i < p->tot_len; i++)
    {
        str[i] += 1;
    }

    // Send
    _TCP_server_send(tpcb, buf, p->tot_len, true, false);

    mem_free(buf);
}

/**
 * @brief  Implements the tcp_err callback
 *
 * @param  arg pointer to server struct
 * @param  err not used (TODO: will be used in logging later)
 **/
static void _TCP_server_error(void *arg, err_t err)
{
    if (arg != NULL)
    {
        mem_free(arg);
    }
}
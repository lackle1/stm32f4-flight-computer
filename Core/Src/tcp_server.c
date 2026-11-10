#include "tcp_server.h"

#include "lwip.h"
#include "tcp.h"
#include <stdbool.h>
#include <string.h>

enum
{
    SS_NONE,
    SS_ACCEPTED,    // TCP connection is up, but no data has been received
    SS_RECEIVED,    // Server is receiving data
    SS_CLOSING      // Client has stopped sending, but the server may still have queued data to send
};

typedef struct tcp_pcb pcb_t;
typedef struct pbuf pbuf_t;

typedef struct tcp_server_struct
{
    u8_t state;
    u8_t retries;
    pcb_t *pcb;
    pbuf_t *p;
} ss_t;

static err_t _TCP_server_accept(void *arg, pcb_t *newpcb, err_t err);
static err_t _TCP_server_recv(void *arg, pcb_t *tpcb, pbuf_t *p, err_t err);
static void _TCP_server_close(pcb_t *tpcb, ss_t *ss);
static void _TCP_server_send(pcb_t *tpcb, const void *data, u16_t len, bool copy_data, bool wait_for_more_data);
static void _TCP_server_handle_packet(pcb_t *tpcb, void *data, u16_t len);
static void _TCP_server_handle(pcb_t *tpcb, ss_t *ss);
static void _TCP_server_error(void *arg, err_t err);
// static err_t _TCP_server_sent();
// static err_t _TCP_server_poll();

/**
 * @brief  Initialises the TCP server
 *
 * @return true if success, false if failed.
 **/
static err_t _TCP_server_init()
{
    // Create TCP PCB
    pcb_t *tpcb;
    tpcb = tcp_new();
    if (tpcb == NULL)
    {
        return false;
    }

    // Bind to IP address and port 7
    ip_addr_t myIPADDR;
    IP_ADDR4(&myIPADDR, 192, 168, 1, 111);
    err_t err = tcp_bind(tpcb, &myIPADDR, 7);

    // If binding fails, free the memory
    if (err != ERR_OK)
    {
        memp_free(MEMP_TCP_PCB, tpcb);
        return err;
    }

    // Begin listening for a connection
    tpcb = tcp_listen(tpcb);

    // Register the server accept callback
    tcp_accept(tpcb, _TCP_server_accept);

    return ERR_OK;
}

void TCP_server_init()
{
    _TCP_server_init();
}

/**
 * @brief  Implementation of the tcp_accept callback
 *
 * @param  arg not used
 * @param  newpcb pointer to newly created tcp connection struct
 * @param  err not used
 * 
 * @return Error status
 **/
static err_t _TCP_server_accept(void *arg, pcb_t *newpcb, err_t err)
{
    err_t ret_err;
    ss_t *ss;

    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(err);

    tcp_setprio(newpcb, TCP_PRIO_MIN);

    // Create server struct
    ss = (ss_t *)mem_malloc(sizeof(ss_t));
    if (ss != NULL)
    {
        ss->state = SS_ACCEPTED;
        ss->retries = 0;
        ss->pcb = newpcb;
        ss->p = NULL;

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
static err_t _TCP_server_recv(void *arg, pcb_t *tpcb, pbuf_t *p, err_t err)
{
    ss_t *ss;
    err_t ret_err;

    LWIP_ASSERT("argument is not null", arg != NULL);

    ss = (ss_t *)arg;

    // If buffer is empty, the client has closed the connection
    if (p == NULL)
    {
        ss->state = SS_CLOSING;
        _TCP_server_close(tpcb, ss);
        ret_err = ERR_OK;
    }
    // Non-empty frame, but an error has occurred
    else if (err != ERR_OK)
    {
        // Free received pbuf
        ss->p = NULL;
        pbuf_free(p);
        ret_err = err;
    }
    // Server has not received any data yet
    else if (ss->state == SS_ACCEPTED)
    {
        // Change state
        ss->state = SS_RECEIVED;
        ss->p = p;
        _TCP_server_handle(tpcb, ss);
        ret_err = ERR_OK;
    }
    // Not the first chunk of data that has been received
    else if (ss->state == SS_RECEIVED)
    {
        if (ss->p == NULL) {
            // If the previous buffer has already been processed
            ss->p = p;
        }
        else {
            // Otherwise, add the new buffer to the chain
            pbuf_cat(ss->p, p);
        }
        _TCP_server_handle(tpcb, ss);
        ret_err = ERR_OK;
    }
    else
    {
        // Trash data
        tcp_recved(tpcb, p->tot_len);
        ss->p = NULL;
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
static void _TCP_server_close(pcb_t *tpcb, ss_t *ss)
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
static void _TCP_server_send(pcb_t *tpcb, const void *data, u16_t len, bool copy_data, bool wait_for_more_data)
{
    // Wait until there is space on the buffer
    while (len > tcp_sndbuf(tpcb));

    // Set flags
    u8_t flags = (TCP_WRITE_FLAG_COPY & copy_data) | (TCP_WRITE_FLAG_MORE & wait_for_more_data);

    tcp_write(tpcb, data, len, flags);

    tcp_output(tpcb);
}

/**
 * @brief Handles individual packets
 *
 * @param tpcb pointer to pcb struct
 * @param data pointer to data
 * @param len length of data
 **/
static void _TCP_server_handle_packet(pcb_t *tpcb, void *data, u16_t len)
{
    u8_t *str = (u8_t *)data;
    for (u16_t i = 0; i < len; i++)
    {
        str[i] += 1;
    }

    _TCP_server_send(tpcb, data, len, true, false);
}

/**
 * @brief Handles packet buffer
 *
 * @param tpcb pointer to pcb struct
 * @param ss pointer to server struct
 **/
static void _TCP_server_handle(pcb_t *tpcb, ss_t *ss)
{
    // Allocate a buffer the size of the current packet
    void *buf = mem_malloc(ss->p->tot_len);

    pbuf_copy_partial(ss->p, buf, ss->p->tot_len, 0);
    _TCP_server_handle_packet(tpcb, buf, ss->p->tot_len);
    tcp_recved(tpcb, ss->p->tot_len);
    pbuf_free(ss->p);

    ss->p = NULL;
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
    LWIP_UNUSED_ARG(err);

    if (arg != NULL)
    {
        mem_free(arg);
    }
}
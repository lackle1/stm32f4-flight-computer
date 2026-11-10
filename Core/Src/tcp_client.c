#include "my_tcp.h"
#include "tcp_client.h"

#include "lwip.h"
#include "tcp.h"

#include <stdbool.h>
#include <string.h>

enum tcp_client_states
{
    CS_NONE,
    CS_CONNECTED,   // TCP connection is up
    CS_CLOSING      // Client has stopped sending, but the server may still have queued data to send
};

typedef struct tcp_client_struct
{
    u8_t state;
    u8_t retries;
    pcb_st *pcb;
} client_st;

static err_t _TCP_client_connected(void *arg, pcb_st *newpcb, err_t err);
static err_t _TCP_client_recv(void *arg, pcb_st *tpcb, struct pbuf *p, err_t err);
static void _TCP_client_handle(pcb_st *tpcb, struct pbuf *p);
static void _TCP_client_send(pcb_st *tpcb, const void *data, u16_t len, bool copy_data, bool wait_for_more_data);
static void _TCP_client_close(pcb_st *tpcb, client_st *cs);


/**
 * @brief  Initialises TCP client and connects to server
 *
 * @return Error status
 **/
static err_t _TCP_client_init()
{
    err_t ret_err;
    pcb_st *tpcb;
    tpcb = tcp_new();

    if (tpcb != NULL)
    {
        ip_addr_t dest_ip;
        IP_ADDR4(&dest_ip, 192, 168, 1, 18);
        ret_err = tcp_connect(tpcb, &dest_ip, 31, _TCP_client_connected);

        if (ret_err != ERR_OK)
        {
            memp_free(MEMP_TCP_PCB, tpcb);
        }
    }
    else
    {
        ret_err = ERR_MEM;
    }

    return ret_err;
}

void TCP_client_init()
{
    _TCP_client_init();
}

static err_t _TCP_client_connected(void *arg, pcb_st *newpcb, err_t err)
{
    err_t ret_err;
    client_st *cs;

    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(err);
    
    cs = mem_malloc(sizeof(client_st));
    if (cs != NULL)
    {
        cs->state = CS_CONNECTED;
        cs->retries = 0;
        cs->pcb = newpcb;

        // Pass cs to all subsequent callbacks
        tcp_arg(newpcb, cs);

        // Resgister callbacks
        tcp_recv(newpcb, _TCP_client_recv);
        // tcp_poll(newpcb, _TCP_client_poll);
        // tcp_sent(newpcb, _TCP_client_sent);

        // Send first message to server
        const char *msg = "First Message!";
        _TCP_client_send(newpcb, msg, strlen(msg), false, false);

        ret_err = ERR_OK;
    }
    else
    {
        _TCP_client_close(newpcb, NULL);
        ret_err = ERR_MEM;
    }

    return ret_err;
}

static err_t _TCP_client_recv(void *arg, pcb_st *tpcb, struct pbuf *p, err_t err)
{
    client_st *cs;
    err_t ret_err;

    LWIP_ASSERT("arg != NULL", arg != NULL);

    cs = (client_st *)arg;

    // If the buffer is empty, the server has closed the connection
    if (p == NULL)
    {
        cs->state = CS_CLOSING;
        _TCP_client_close(tpcb, cs);
        ret_err = ERR_OK;
    }
    // Error has occurred
    else if (err != ERR_OK)
    {
        // Free the received pbuf
        pbuf_free(p);
        ret_err = err;
    }
    // Connected:
    else if (cs->state == CS_CONNECTED)
    {
        // Tell stack data was received
        tcp_recved(tpcb, p->tot_len);

        // Process data
        _TCP_client_handle(tpcb, p);
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

static void _TCP_client_handle(pcb_st *tpcb, struct pbuf *p)
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
    _TCP_client_send(tpcb, buf, p->tot_len, true, false);

    mem_free(buf);
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
static void _TCP_client_send(pcb_st *tpcb, const void *data, u16_t len, bool copy_data, bool wait_for_more_data)
{
    // Wait until there is space on the buffer
    while (len > tcp_sndbuf(tpcb));

    // Set flags
    u8_t flags = (TCP_WRITE_FLAG_COPY & copy_data) | (TCP_WRITE_FLAG_MORE & wait_for_more_data);

    tcp_write(tpcb, data, len, flags);

    tcp_output(tpcb);
}

/**
 * @brief  Closes the connection.
 *
 * @param  tpcb pointer to pcb struct
 * @param  cs pointer to client struct
 **/
static void _TCP_client_close(pcb_st *tpcb, client_st *cs)
{
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);

    if (cs != NULL)
    {
        mem_free(cs);
    }

    tcp_close(tpcb);
}
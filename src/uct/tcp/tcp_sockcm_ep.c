/**
* Copyright (C) Mellanox Technologies Ltd. 2019.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "tcp_sockcm_ep.h"
#include <ucs/sys/sock.h>
#include <ucs/async/async.h>
#include <ucs/arch/bitops.h>
#include <ucs/sys/string.h>


static UCS_F_ALWAYS_INLINE
uct_tcp_sockcm_t *uct_tcp_sockcm_ep_get_cm(uct_tcp_sockcm_ep_t *cep)
{
    /* return the tcp sockcm connection manager this ep is using */
    return ucs_container_of(cep->super.super.super.iface, uct_tcp_sockcm_t,
                            super.iface);
}

ucs_status_t uct_tcp_sockcm_ep_disconnect(uct_ep_h ep, unsigned flags)
{
    return UCS_ERR_NOT_IMPLEMENTED;
}

static void uct_tcp_sockcm_ep_init_comm_ctx(uct_tcp_sockcm_ep_t *cep)
{
    cep->comm_ctx.offset = 0;
    cep->comm_ctx.length = 0;
}

static void uct_tcp_sockcm_ep_handle_disconnect(uct_tcp_sockcm_ep_t *cep,
                                                ucs_status_t status)
{
    uct_cm_remote_data_t remote_data;

    /* remote peer disconnected */
    ucs_debug("ep %p (fd=%d): remote peer disconnected", cep, cep->fd);
    uct_tcp_sockcm_ep_init_comm_ctx(cep);

    ucs_assert(status != UCS_OK);

    if (cep->state & (UCT_TCP_SOCKCM_EP_CLIENT_CONNECTED_CB_INVOKED |
                      UCT_TCP_SOCKCM_EP_SERVER_NOTIFY_CB_INVOKED)) {
        /* TODO ep is already connected, call disconnect callback */
    } else if (cep->state & UCT_TCP_SOCKCM_EP_ON_CLIENT) {
        remote_data.field_mask = 0;
        uct_cm_ep_client_connect_cb(&cep->super, &remote_data, status);
    } else {
        ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_ON_SERVER);
        /* the server might not have a valid ep yet. in this case the notify_cb
         * is NULL and therefore will not be invoked */
        uct_cm_ep_server_conn_notify_cb(&cep->super, status);
    }
}

static int uct_tcp_sockcm_ep_is_tx_rx_done(uct_tcp_sockcm_ep_t *cep)
{
    ucs_assert((cep->comm_ctx.length != 0));
    return (cep->comm_ctx.offset == cep->comm_ctx.length);
}

/**
 * This function should be called with the lock held.
 */
static ucs_status_t uct_tcp_sockcm_ep_progress_send(uct_tcp_sockcm_ep_t *cep)
{
    ucs_status_t status;
    size_t sent_length;
    int events;

    ucs_assert((ucs_test_all_flags(cep->state, UCT_TCP_SOCKCM_EP_ON_CLIENT      |
                                               UCT_TCP_SOCKCM_EP_PRIV_DATA_PACKED)) ||
               (ucs_test_all_flags(cep->state, UCT_TCP_SOCKCM_EP_ON_SERVER      |
                                               UCT_TCP_SOCKCM_EP_SERVER_CREATED |
                                               UCT_TCP_SOCKCM_EP_DATA_RECEIVED)));

    ucs_assertv(cep->comm_ctx.offset < cep->comm_ctx.length, "ep state %d offset %zu length %zu",
                cep->state, cep->comm_ctx.offset, cep->comm_ctx.length);

    sent_length = cep->comm_ctx.length - cep->comm_ctx.offset;

    status = ucs_socket_send_nb(cep->fd,
                                UCS_PTR_BYTE_OFFSET(cep->comm_ctx.buf,
                                                    cep->comm_ctx.offset),
                                &sent_length, NULL, NULL);
    if ((status != UCS_OK) && (status != UCS_ERR_NO_PROGRESS)) {
        if (status == UCS_ERR_NOT_CONNECTED) {
            uct_tcp_sockcm_ep_handle_disconnect(cep, status);
        } else {
            ucs_error("ep %p failed to send %s's data (len=%zu offset=%zu)",
                      cep, (cep->state & UCT_TCP_SOCKCM_EP_ON_SERVER) ? "server" : "client",
                      cep->comm_ctx.length, cep->comm_ctx.offset);
        }

        goto out;
    }

    cep->comm_ctx.offset += sent_length;
    ucs_assert(cep->comm_ctx.offset <= cep->comm_ctx.length);

    if (uct_tcp_sockcm_ep_is_tx_rx_done(cep)) {
        ucs_assert(status == UCS_OK);
        cep->state |= UCT_TCP_SOCKCM_EP_DATA_SENT;

        /* on the client side - if completed sending a message after the notify
         * call was invoked, then this message is the notify message */
        if (cep->state & UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_CALLED) {
            ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_ON_CLIENT);
            cep->state |= UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_SENT;
        }

        uct_tcp_sockcm_ep_init_comm_ctx(cep);

        /* wait for a message from the peer */
        events = UCS_EVENT_SET_EVREAD;
    } else {
        /* continue the sending when possible, and handle potential disconnect */
        events = UCS_EVENT_SET_EVREAD | UCS_EVENT_SET_EVWRITE;
    }

    status = ucs_async_modify_handler(cep->fd, events);
    if (status != UCS_OK) {
        ucs_error("failed to modify %d event handler to %d: %s",
                  cep->fd, events, ucs_status_string(status));
    }

out:
    return status;
}

static const char *uct_tcp_sockcm_cm_ep_peer_addr_str(uct_tcp_sockcm_ep_t *cep,
                                                      char *buf, size_t max)
{
    struct sockaddr_storage remote_dev_addr = {0}; /* Suppress Clang false-positive */
    socklen_t remote_dev_addr_len;
    ucs_status_t status;

    /* get the device address of the remote peer associated with the connected fd */
    status = ucs_socket_getpeername(cep->fd, &remote_dev_addr, &remote_dev_addr_len);
    if (status != UCS_OK) {
        return NULL;
    }

    return ucs_sockaddr_str((const struct sockaddr*)&remote_dev_addr, buf, max);
}

ucs_status_t uct_tcp_sockcm_cm_ep_conn_notify(uct_ep_h ep)
{
    uct_tcp_sockcm_ep_t *cep                = ucs_derived_of(ep, uct_tcp_sockcm_ep_t);
    uct_tcp_sockcm_t *tcp_sockcm            = uct_tcp_sockcm_ep_get_cm(cep);
    uct_tcp_sockcm_priv_data_hdr_t *hdr;
    char peer_str[UCS_SOCKADDR_STRING_LEN];
    ucs_status_t status;

    UCS_ASYNC_BLOCK(tcp_sockcm->super.iface.worker->async);

    ucs_assert(ucs_test_all_flags(cep->state, UCT_TCP_SOCKCM_EP_ON_CLIENT     |
                                              UCT_TCP_SOCKCM_EP_DATA_SENT     |
                                              UCT_TCP_SOCKCM_EP_DATA_RECEIVED |
                                              UCT_TCP_SOCKCM_EP_CLIENT_CONNECTED_CB_INVOKED));
    ucs_assert(!(cep->state & UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_CALLED));

    hdr = (uct_tcp_sockcm_priv_data_hdr_t*)cep->comm_ctx.buf;

    hdr->length          = 0;   /* sending only the header in the notify message */
    hdr->status          = UCS_OK;
    cep->comm_ctx.length = sizeof(*hdr);

    ucs_trace("ep %p sending conn notification to server: %s", cep,
              uct_tcp_sockcm_cm_ep_peer_addr_str(cep, peer_str, UCS_SOCKADDR_STRING_LEN));

    cep->state |= UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_CALLED;
    status = uct_tcp_sockcm_ep_progress_send(cep);

    UCS_ASYNC_UNBLOCK(tcp_sockcm->super.iface.worker->async);
    return status;
}

static ucs_status_t uct_tcp_sockcm_ep_pack_priv_data(uct_tcp_sockcm_ep_t *cep)
{
    char ifname_str[UCT_DEVICE_NAME_MAX];
    uct_tcp_sockcm_priv_data_hdr_t *hdr;
    size_t priv_data_ret;
    ucs_status_t status;
    uct_cm_ep_priv_data_pack_args_t pack_args;

    /* get interface name associated with the connected client fd */
    status = ucs_sockaddr_get_ifname(cep->fd, ifname_str, sizeof(ifname_str));
    if (UCS_OK != status) {
        goto out;
    }

    hdr                  = (uct_tcp_sockcm_priv_data_hdr_t*)cep->comm_ctx.buf;
    pack_args.field_mask = UCT_CM_EP_PRIV_DATA_PACK_ARGS_FIELD_DEVICE_NAME;
    ucs_strncpy_safe(pack_args.dev_name, ifname_str, UCT_DEVICE_NAME_MAX);

    status = uct_cm_ep_pack_cb(&cep->super, cep->super.user_data, &pack_args,
                               hdr + 1,
                               uct_tcp_sockcm_ep_get_cm(cep)->priv_data_len,
                               &priv_data_ret);
    if (status != UCS_OK) {
        goto out;
    }

    hdr->length          = priv_data_ret;
    hdr->status          = UCS_OK;
    cep->comm_ctx.length = sizeof(*hdr) + hdr->length;
    cep->state          |= UCT_TCP_SOCKCM_EP_PRIV_DATA_PACKED;

out:
    return status;
}

static int uct_tcp_sockcm_ep_skip_event(uct_tcp_sockcm_ep_t *cep)
{
    if (cep->state & UCT_TCP_SOCKCM_EP_ON_SERVER) {
        return cep->state & UCT_TCP_SOCKCM_EP_DATA_SENT;
    } else {
        ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_ON_CLIENT);
        return (cep->state & UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_SENT) ||
               ((cep->state & UCT_TCP_SOCKCM_EP_DATA_SENT) &&
                !(cep->state & UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_CALLED));
    }
}

ucs_status_t uct_tcp_sockcm_ep_send(uct_tcp_sockcm_ep_t *cep)
{
    ucs_status_t status;

    if (uct_tcp_sockcm_ep_skip_event(cep)) {
        return UCS_OK;
    }

    if (!(cep->state & UCT_TCP_SOCKCM_EP_PRIV_DATA_PACKED)) {
        status = uct_tcp_sockcm_ep_pack_priv_data(cep);
        if (status != UCS_OK) {
            return status;
        }
    }

    return uct_tcp_sockcm_ep_progress_send(cep);
}

static ucs_status_t uct_tcp_sockcm_ep_server_invoke_conn_req_cb(uct_tcp_sockcm_ep_t *cep)
{
    uct_tcp_sockcm_priv_data_hdr_t *hdr     = (uct_tcp_sockcm_priv_data_hdr_t *)
                                              cep->comm_ctx.buf;
    struct sockaddr_storage remote_dev_addr = {0}; /* Suppress Clang false-positive */
    uct_cm_listener_conn_request_args_t conn_req_args;
    char peer_str[UCS_SOCKADDR_STRING_LEN];
    char ifname_str[UCT_DEVICE_NAME_MAX];
    uct_cm_remote_data_t remote_data;
    socklen_t remote_dev_addr_len;
    ucs_sock_addr_t client_saddr;
    ucs_status_t status;

    /* get the local interface name associated with the connected fd */
    status = ucs_sockaddr_get_ifname(cep->fd, ifname_str, UCT_DEVICE_NAME_MAX);
    if (UCS_OK != status) {
        return status;
    }

    /* get the device address of the remote peer associated with the connected fd */
    status = ucs_socket_getpeername(cep->fd, &remote_dev_addr, &remote_dev_addr_len);
    if (status != UCS_OK) {
        return status;
    }

    remote_data.field_mask            = UCT_CM_REMOTE_DATA_FIELD_DEV_ADDR        |
                                        UCT_CM_REMOTE_DATA_FIELD_DEV_ADDR_LENGTH |
                                        UCT_CM_REMOTE_DATA_FIELD_CONN_PRIV_DATA  |
                                        UCT_CM_REMOTE_DATA_FIELD_CONN_PRIV_DATA_LENGTH;
    remote_data.dev_addr              = (uct_device_addr_t *)&remote_dev_addr;
    remote_data.dev_addr_length       = remote_dev_addr_len;
    remote_data.conn_priv_data        = hdr + 1;
    remote_data.conn_priv_data_length = hdr->length;

    client_saddr.addr    = (struct sockaddr*)&remote_dev_addr;
    client_saddr.addrlen = remote_dev_addr_len;

    conn_req_args.field_mask     = UCT_CM_LISTENER_CONN_REQUEST_ARGS_FIELD_DEV_NAME     |
                                   UCT_CM_LISTENER_CONN_REQUEST_ARGS_FIELD_CONN_REQUEST |
                                   UCT_CM_LISTENER_CONN_REQUEST_ARGS_FIELD_REMOTE_DATA  |
                                   UCT_CM_LISTENER_CONN_REQUEST_ARGS_FIELD_CLIENT_ADDR;
    conn_req_args.conn_request   = cep;
    conn_req_args.remote_data    = &remote_data;
    conn_req_args.client_address = client_saddr;
    ucs_strncpy_safe(conn_req_args.dev_name, ifname_str, UCT_DEVICE_NAME_MAX);

    ucs_debug("fd %d: remote_data: (field_mask=%zu) dev_addr: %s (length=%zu), "
              "conn_priv_data_length=%zu", cep->fd, remote_data.field_mask,
              ucs_sockaddr_str((const struct sockaddr*)remote_data.dev_addr,
                               peer_str, UCS_SOCKADDR_STRING_LEN),
              remote_data.dev_addr_length, remote_data.conn_priv_data_length);

    /* the endpoint, passed as the conn_request to the callback, will be passed
     * to uct_ep_create() which will be invoked by the user and therefore moving
     * over to its responsibility. */
    ucs_list_del(&cep->list);
    cep->listener->conn_request_cb(&cep->listener->super, cep->listener->user_data,
                                   &conn_req_args);

    return UCS_OK;
}

void uct_tcp_sockcm_ep_client_connect_cb(uct_tcp_sockcm_ep_t *cep,
                                         uct_cm_remote_data_t *remote_data,
                                         ucs_status_t status)
{
    cep->state |= UCT_TCP_SOCKCM_EP_CLIENT_CONNECTED_CB_INVOKED;
    uct_cm_ep_client_connect_cb(&cep->super, remote_data, status);
}

void uct_tcp_sockcm_ep_server_notify_cb(uct_tcp_sockcm_ep_t *cep,
                                        ucs_status_t status)
{
    cep->state |= UCT_TCP_SOCKCM_EP_SERVER_NOTIFY_CB_INVOKED;
    uct_cm_ep_server_conn_notify_cb(&cep->super, status);
}

static ucs_status_t uct_tcp_sockcm_ep_client_invoke_connect_cb(uct_tcp_sockcm_ep_t *cep)
{
    uct_tcp_sockcm_priv_data_hdr_t *hdr     = (uct_tcp_sockcm_priv_data_hdr_t *)
                                              cep->comm_ctx.buf;
    struct sockaddr_storage remote_dev_addr = {0}; /* Suppress Clang false-positive */
    socklen_t remote_dev_addr_len;
    uct_cm_remote_data_t remote_data;
    ucs_status_t status;

    /* get the device address of the remote peer associated with the connected fd */
    status = ucs_socket_getpeername(cep->fd, &remote_dev_addr, &remote_dev_addr_len);
    if (status != UCS_OK) {
        return status;
    }

    remote_data.field_mask            = UCT_CM_REMOTE_DATA_FIELD_DEV_ADDR        |
                                        UCT_CM_REMOTE_DATA_FIELD_DEV_ADDR_LENGTH |
                                        UCT_CM_REMOTE_DATA_FIELD_CONN_PRIV_DATA  |
                                        UCT_CM_REMOTE_DATA_FIELD_CONN_PRIV_DATA_LENGTH;
    remote_data.dev_addr              = (uct_device_addr_t *)&remote_dev_addr;
    remote_data.dev_addr_length       = remote_dev_addr_len;
    remote_data.conn_priv_data        = hdr + 1;
    remote_data.conn_priv_data_length = hdr->length;

    uct_tcp_sockcm_ep_client_connect_cb(cep, &remote_data, (ucs_status_t)hdr->status);

    return status;
}

ucs_status_t uct_tcp_sockcm_ep_server_handle_data_received(uct_tcp_sockcm_ep_t *cep)
{
    uct_tcp_sockcm_priv_data_hdr_t *hdr = (uct_tcp_sockcm_priv_data_hdr_t *)
                                           cep->comm_ctx.buf;
    ucs_status_t status;
    int events;

    if (cep->state & UCT_TCP_SOCKCM_EP_DATA_SENT) {
        ucs_assert(ucs_test_all_flags(cep->state, UCT_TCP_SOCKCM_EP_SERVER_CREATED |
                                                  UCT_TCP_SOCKCM_EP_DATA_RECEIVED));

        ucs_assert(hdr->length == 0);
        uct_tcp_sockcm_ep_server_notify_cb(cep, (ucs_status_t)hdr->status);
    } else if ((cep->state & UCT_TCP_SOCKCM_EP_DATA_RECEIVED) &&
               !(cep->state & UCT_TCP_SOCKCM_EP_SERVER_CREATED)) {
        status = uct_tcp_sockcm_ep_server_invoke_conn_req_cb(cep);
        if (status != UCS_OK) {
            goto out;
        }
    } else {
        ucs_error("unexpected state on the server endpoint: %d", cep->state);
        status = UCS_ERR_IO_ERROR;
        goto out;
    }

    /* server to wait for any notification (disconnect) from the client */
    events = UCS_EVENT_SET_EVREAD;
    /* server to send its private data to the client only if its ep was already created */
    if ((cep->state & UCT_TCP_SOCKCM_EP_SERVER_CREATED) &&
        !(cep->state & UCT_TCP_SOCKCM_EP_DATA_SENT)) {
        ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_DATA_RECEIVED);
        events |= UCS_EVENT_SET_EVWRITE;
    }

    status = ucs_async_modify_handler(cep->fd, events);

out:
    return status;
}

ucs_status_t uct_tcp_sockcm_ep_handle_data_received(uct_tcp_sockcm_ep_t *cep)
{
    ucs_status_t status;

    cep->state |= UCT_TCP_SOCKCM_EP_DATA_RECEIVED;
    /* if the data was received, drop the header_received flag to receive new messages */
    cep->state &= ~UCT_TCP_SOCKCM_EP_HDR_RECEIVED;
    uct_tcp_sockcm_ep_init_comm_ctx(cep);

    if (cep->state & UCT_TCP_SOCKCM_EP_ON_SERVER) {
        status = uct_tcp_sockcm_ep_server_handle_data_received(cep);
        if (status != UCS_OK) {
            goto out;
        }
    } else {
        ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_ON_CLIENT);
        status = uct_tcp_sockcm_ep_client_invoke_connect_cb(cep);
        if (status != UCS_OK) {
            goto out;
        }

        /* next, if the client did not send a connection establishment notification
         * to the server from the connect_cb, he will send it from the main thread */
    }

out:
    return status;
}

static ucs_status_t uct_tcp_sockcm_ep_recv_nb(uct_tcp_sockcm_ep_t *cep)
{
    size_t recv_length;
    ucs_status_t status;

    recv_length = uct_tcp_sockcm_ep_get_cm(cep)->priv_data_len +
                  sizeof(uct_tcp_sockcm_priv_data_hdr_t) - cep->comm_ctx.offset;
    status = ucs_socket_recv_nb(cep->fd, UCS_PTR_BYTE_OFFSET(cep->comm_ctx.buf,
                                                             cep->comm_ctx.offset),
                                &recv_length, NULL, NULL);
    if ((status != UCS_OK) && (status != UCS_ERR_NO_PROGRESS)) {
        if (status == UCS_ERR_NOT_CONNECTED) {
            uct_tcp_sockcm_ep_handle_disconnect(cep, status);
        } else {
            ucs_error("ep %p (fd=%d) failed to recv client's data (offset=%zu)",
                      cep, cep->fd, cep->comm_ctx.offset);
        }
        goto out;
    }

    cep->comm_ctx.offset += recv_length;
    ucs_assertv((cep->comm_ctx.length ?
                 cep->comm_ctx.offset <= cep->comm_ctx.length : 1), "%zu > %zu",
                cep->comm_ctx.offset, cep->comm_ctx.length);

out:
    return status;
}

ucs_status_t uct_tcp_sockcm_ep_recv(uct_tcp_sockcm_ep_t *cep)
{
    uct_tcp_sockcm_priv_data_hdr_t *hdr;
    ucs_status_t status;

    status = uct_tcp_sockcm_ep_recv_nb(cep);
    if (status != UCS_OK) {
        goto out;
    }

    if (!(cep->state & UCT_TCP_SOCKCM_EP_HDR_RECEIVED)) {
        if (cep->comm_ctx.offset < sizeof(*hdr)) {
            goto out;
        }

        hdr                  = (uct_tcp_sockcm_priv_data_hdr_t *)cep->comm_ctx.buf;
        cep->comm_ctx.length = sizeof(*hdr) + hdr->length;
        ucs_assertv(cep->comm_ctx.offset <= cep->comm_ctx.length , "%zu > %zu",
                    cep->comm_ctx.offset, cep->comm_ctx.length);

        cep->state          |= UCT_TCP_SOCKCM_EP_HDR_RECEIVED;
    }

    if (uct_tcp_sockcm_ep_is_tx_rx_done(cep)) {
        status = uct_tcp_sockcm_ep_handle_data_received(cep);
    }

out:
    return (status == UCS_ERR_NO_PROGRESS) ? UCS_OK : status;
}

ucs_status_t uct_tcp_sockcm_ep_set_sockopt(uct_tcp_sockcm_ep_t *ep)
{
    uct_tcp_sockcm_t *tcp_sockcm = uct_tcp_sockcm_ep_get_cm(ep);

    return ucs_socket_set_buffer_size(ep->fd, tcp_sockcm->sockopt_sndbuf,
                                      tcp_sockcm->sockopt_rcvbuf);
}

static ucs_status_t uct_tcp_sockcm_ep_server_init(uct_tcp_sockcm_ep_t *cep,
                                                  const uct_ep_params_t *params)
{
    cep->state |= UCT_TCP_SOCKCM_EP_ON_SERVER;
    return UCS_OK;
}

static ucs_status_t uct_tcp_sockcm_ep_client_init(uct_tcp_sockcm_ep_t *cep,
                                                  const uct_ep_params_t *params)
{
    uct_tcp_sockcm_t *tcp_sockcm = uct_tcp_sockcm_ep_get_cm(cep);
    char ip_port_str[UCS_SOCKADDR_STRING_LEN];
    const struct sockaddr *server_addr;
    ucs_async_context_t *async_ctx;
    ucs_status_t status;

    cep->state |= UCT_TCP_SOCKCM_EP_ON_CLIENT;

    status = uct_cm_ep_client_set_connect_cb(params, &cep->super);
    if (status != UCS_OK) {
        goto err;
    }

    server_addr = params->sockaddr->addr;
    status = ucs_socket_create(server_addr->sa_family, SOCK_STREAM, &cep->fd);
    if (status != UCS_OK) {
        goto err;
    }

    /* Set the fd to non-blocking mode. (so that connect() won't be blocking) */
    status = ucs_sys_fcntl_modfl(cep->fd, O_NONBLOCK, 0);
    if (status != UCS_OK) {
        status = UCS_ERR_IO_ERROR;
        goto err_close_socket;
    }

    status = uct_tcp_sockcm_ep_set_sockopt(cep);
    if (status != UCS_OK) {
        goto err_close_socket;
    }

    /* try to connect to the server */
    status = ucs_socket_connect(cep->fd, server_addr);
    if (UCS_STATUS_IS_ERR(status)) {
        goto err_close_socket;
    }
    ucs_assert((status == UCS_OK) || (status == UCS_INPROGRESS));

    async_ctx = tcp_sockcm->super.iface.worker->async;
    status    = ucs_async_set_event_handler(async_ctx->mode, cep->fd,
                                            UCS_EVENT_SET_EVWRITE,
                                            uct_tcp_sa_data_handler, cep,
                                            async_ctx);
    if (status != UCS_OK) {
        goto err_close_socket;
    }

    ucs_debug("created a TCP SOCKCM endpoint (fd=%d) on tcp cm %p, "
              "remote addr: %s", cep->fd, tcp_sockcm,
              ucs_sockaddr_str(server_addr, ip_port_str, UCS_SOCKADDR_STRING_LEN));

    return status;

err_close_socket:
    ucs_close_fd(&cep->fd);
err:
    return status;
}

UCS_CLASS_INIT_FUNC(uct_tcp_sockcm_ep_t, const uct_ep_params_t *params)
{
    ucs_status_t status;

    UCS_CLASS_CALL_SUPER_INIT(uct_cm_base_ep_t, params);

    uct_tcp_sockcm_ep_init_comm_ctx(self);
    self->state        = 0;
    self->comm_ctx.buf = ucs_calloc(1, uct_tcp_sockcm_ep_get_cm(self)->priv_data_len +
                                    sizeof(uct_tcp_sockcm_priv_data_hdr_t),
                                    "tcp_sockcm priv data");
    if (self->comm_ctx.buf == NULL) {
        ucs_error("failed to allocate memory for the ep's send/recv buf");
        return UCS_ERR_NO_MEMORY;
    }

    if (params->field_mask & UCT_EP_PARAM_FIELD_SOCKADDR) {
        status = uct_tcp_sockcm_ep_client_init(self, params);
    } else {
        status = uct_tcp_sockcm_ep_server_init(self, params);
    }

    if (status == UCS_OK) {
        ucs_debug("created an endpoint on tcp_sockcm %p id: %d state: %d",
                  uct_tcp_sockcm_ep_get_cm(self), self->fd, self->state);
    }

    return status;
}

ucs_status_t uct_tcp_sockcm_ep_create(const uct_ep_params_t *params, uct_ep_h *ep_p)
{
    uct_tcp_sockcm_ep_t *tcp_ep;
    uct_tcp_sockcm_t *tcp_sockcm;
    ucs_status_t status;

    if (params->field_mask & UCT_EP_PARAM_FIELD_SOCKADDR) {
        /* create a new endpoint for the client side */
        return UCS_CLASS_NEW(uct_tcp_sockcm_ep_t, ep_p, params);
    } else if (params->field_mask & UCT_EP_PARAM_FIELD_CONN_REQUEST) {
        tcp_ep     = (uct_tcp_sockcm_ep_t*)params->conn_request;
        tcp_sockcm = uct_tcp_sockcm_ep_get_cm(tcp_ep);
        UCS_ASYNC_BLOCK(tcp_sockcm->super.iface.worker->async);

        /* the server's endpoint was already created by the listener, return it */
        *ep_p  = &tcp_ep->super.super.super;

        /* fill the tcp_ep fields from the caller's params */
        uct_cm_set_common_data(&tcp_ep->super, params);

        status = uct_cm_ep_server_set_notify_cb(params, &tcp_ep->super);
        if (status != UCS_OK) {
            UCS_ASYNC_UNBLOCK(tcp_sockcm->super.iface.worker->async);
            return status;
        }

        tcp_ep->state |= UCT_TCP_SOCKCM_EP_SERVER_CREATED;

        UCS_ASYNC_UNBLOCK(tcp_sockcm->super.iface.worker->async);

        /* now that the server's ep was created, can try to send data */
        ucs_async_modify_handler(tcp_ep->fd, UCS_EVENT_SET_EVWRITE | UCS_EVENT_SET_EVREAD);
        return UCS_OK;
    } else {
        ucs_error("either UCT_EP_PARAM_FIELD_SOCKADDR or UCT_EP_PARAM_FIELD_CONN_REQUEST "
                  "has to be provided");
        return UCS_ERR_INVALID_PARAM;
    }
}

UCS_CLASS_CLEANUP_FUNC(uct_tcp_sockcm_ep_t)
{
    uct_tcp_sockcm_t *tcp_sockcm = uct_tcp_sockcm_ep_get_cm(self);

    UCS_ASYNC_BLOCK(tcp_sockcm->super.iface.worker->async);

    ucs_free(self->comm_ctx.buf);

    ucs_async_remove_handler(self->fd, 1);

    ucs_close_fd(&self->fd);
    UCS_ASYNC_UNBLOCK(tcp_sockcm->super.iface.worker->async);
}

UCS_CLASS_DEFINE(uct_tcp_sockcm_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_tcp_sockcm_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_tcp_sockcm_ep_t, uct_ep_t);

/*****************************************************************************\
 *  slurm_protocol_api.c - high-level slurm communication functions
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Kevin Tew <tew1@llnl.gov>, et. al.
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

/* GLOBAL INCLUDES */

#ifdef WITH_PTHREADS
#  include <pthread.h>
#endif /* WITH_PTHREADS */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PROJECT INCLUDES */
#include "src/common/macros.h"
#include "src/common/pack.h"
#include "src/common/parse_spec.h"
#include "src/common/read_config.h"
#include "src/common/slurm_auth.h"
#include "src/common/slurm_protocol_interface.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_common.h"
#include "src/common/slurm_protocol_pack.h"
#include "src/common/xmalloc.h"
#include "src/common/log.h"

/* EXTERNAL VARIABLES */

/* #DEFINES */
#define _DEBUG        0
#define SLURM_DEFAULT_TIMEOUT 2000

/* STATIC VARIABLES */
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;
static slurm_protocol_config_t proto_conf_default;
static slurm_protocol_config_t *proto_conf = &proto_conf_default;
static slurm_ctl_conf_t slurmctld_conf;

/**********************************************************************\
 * protocol configuration functions                
\**********************************************************************/
/* slurm_set_api_config
 * sets the slurm_protocol_config object
 * NOT THREAD SAFE
 * IN protocol_conf                -  slurm_protocol_config object
 *
 * XXX: Why isn't the "config_lock" mutex used here?
 */
int slurm_set_api_config(slurm_protocol_config_t * protocol_conf)
{
        proto_conf = protocol_conf;
        return SLURM_SUCCESS;
}

/* slurm_get_api_config
 * returns a pointer to the current slurm_protocol_config object
 * RET slurm_protocol_config_t  - current slurm_protocol_config object
 */
slurm_protocol_config_t *slurm_get_api_config()
{
        return proto_conf;
}

/* slurm_api_set_default_config
 *        called by the send_controller_msg function to insure that at least 
 *        the compiled in default slurm_protocol_config object is initialized
 * RET int                 - return code
 */
int slurm_api_set_default_config()
{
        int rc = SLURM_SUCCESS;

        slurm_mutex_lock(&config_lock);
        if ((slurmctld_conf.control_addr != NULL) &&
            (slurmctld_conf.slurmctld_port != 0))
                goto cleanup;

        read_slurm_conf_ctl(&slurmctld_conf);
        if ((slurmctld_conf.control_addr == NULL) ||
            (slurmctld_conf.slurmctld_port == 0)) {
                error("Unable to establish control machine or port");
                rc =SLURM_ERROR;
                goto cleanup;
        }

        slurm_set_addr(&proto_conf_default.primary_controller,
                       slurmctld_conf.slurmctld_port,
                       slurmctld_conf.control_addr);
        if (proto_conf_default.primary_controller.sin_port == 0) {
                error("Unable to establish control machine address");
                rc =SLURM_ERROR;
                goto cleanup;
        }

        if (slurmctld_conf.backup_addr) {
                slurm_set_addr(&proto_conf_default.secondary_controller,
                               slurmctld_conf.slurmctld_port,
                               slurmctld_conf.backup_addr);
        }
        proto_conf = &proto_conf_default;

      cleanup:
        slurm_mutex_unlock(&config_lock);
        return rc;
}

/* slurm_get_slurmd_port
 * returns slurmd port from slurmctld_conf object
 * RET short int        - slurmd port
 */
short int slurm_get_slurmd_port(void)
{
        if (slurmctld_conf.slurmd_port == 0)  /* ==0 if config unread */
                slurm_api_set_default_config();

        return slurmctld_conf.slurmd_port;
}

/* slurm_get_slurm_user_id
 * returns slurmd uid from slurmctld_conf object
 * RET uint32_t        - slurm user id
 */
uint32_t slurm_get_slurm_user_id(void)
{
        if (slurmctld_conf.slurmd_port == 0)  /* ==0 if config unread */
                slurm_api_set_default_config();

        return slurmctld_conf.slurm_user_id;
}

/**********************************************************************\
 * general message management functions used by slurmctld, slurmd
\**********************************************************************/

/* 
 *  Initialize a slurm server at port "port"
 * 
 * IN  port     - port to bind the msg server to
 * RET slurm_fd - file descriptor of the connection created
 */
slurm_fd slurm_init_msg_engine_port(uint16_t port)
{
        slurm_addr addr;

        slurm_set_addr_any(&addr, port);
        return _slurm_init_msg_engine(&addr);
}

/* 
 *  Same as above, but initialize using a slurm address "addr"
 *
 * IN  addr     - slurm_addr to bind the msg server to 
 * RET slurm_fd - file descriptor of the connection created
 */
slurm_fd slurm_init_msg_engine(slurm_addr *addr)
{
        return _slurm_init_msg_engine(addr);
}

/* 
 *  Close an established message engine.
 *    Returns SLURM_SUCCESS or SLURM_FAILURE.
 *
 * IN  fd  - an open file descriptor to close
 * RET int - the return code
 */
int slurm_shutdown_msg_engine(slurm_fd fd)
{
        return _slurm_close(fd);
}

/* 
 *   Close an established message connection.
 *     Returns SLURM_SUCCESS or SLURM_FAILURE.
 *
 * IN  fd  - an open file descriptor to close
 * RET int - the return code
 */
int slurm_shutdown_msg_conn(slurm_fd fd)
{
        return _slurm_close(fd);
}

/**********************************************************************\
 * msg connection establishment functions used by msg clients
\**********************************************************************/

/* In the bsd socket implementation it creates a SOCK_STREAM socket  
 *        and calls connect on it a SOCK_DGRAM socket called with connect   
 *        is defined to only receive messages from the address/port pair  
 *        argument of the connect call slurm_address - for now it is  
 *        really just a sockaddr_in
 * IN slurm_address         - slurm_addr of the connection destination
 * RET slurm_fd                - file descriptor of the connection created
 */
slurm_fd slurm_open_msg_conn(slurm_addr * slurm_address)
{
        return _slurm_open_msg_conn(slurm_address);
}

/* calls connect to make a connection-less datagram connection to the 
 *        primary or secondary slurmctld message engine
 * RET slurm_fd        - file descriptor of the connection created
 */
slurm_fd slurm_open_controller_conn()
{
        slurm_fd fd;

        if (slurm_api_set_default_config() < 0)
                return SLURM_FAILURE;

        if ((fd = slurm_open_msg_conn(&proto_conf->primary_controller)) >= 0)
                return fd;
        
        debug("Failed to contact primary controller: %m");

        if (!slurmctld_conf.backup_controller) 
                goto fail;

        if ((fd = slurm_open_msg_conn(&proto_conf->secondary_controller)) >= 0)
                return fd;

        debug("Failed to contact secondary controller: %m");

    fail:
        slurm_seterrno_ret(SLURM_COMMUNICATIONS_CONNECTION_ERROR);
}

/* calls connect to make a connection-less datagram connection to the 
 *        primary or secondary slurmctld message engine
 * RET slurm_fd        - file descriptor of the connection created
 * IN dest         - controller to contact, primary or secondary
 */
slurm_fd slurm_open_controller_conn_spec(enum controller_id dest)
{
        slurm_addr *addr;

        if (slurm_api_set_default_config() < 0) {
                debug3("Error: Unable to set default config");
                return SLURM_ERROR;
        }
                
        addr = (dest == PRIMARY_CONTROLLER) ? 
                  &proto_conf->primary_controller : 
                  &proto_conf->secondary_controller;

        if (!addr) return SLURM_ERROR;

        return slurm_open_msg_conn(addr);
}

/* In the bsd implmentation maps directly to a accept call 
 * IN open_fd                - file descriptor to accept connection on
 * OUT slurm_address         - slurm_addr of the accepted connection
 * RET slurm_fd                - file descriptor of the connection created
 */
slurm_fd slurm_accept_msg_conn(slurm_fd open_fd,
                               slurm_addr * slurm_address)
{
        return _slurm_accept_msg_conn(open_fd, slurm_address);
}

/* In the bsd implmentation maps directly to a close call, to close 
 *        the socket that was accepted
 * IN open_fd                - an open file descriptor to close
 * RET int                - the return code
 */
int slurm_close_accepted_conn(slurm_fd open_fd)
{
        return _slurm_close_accepted_conn(open_fd);
}

/**********************************************************************\
 * receive message functions
\**********************************************************************/

/*
 * NOTE: memory is allocated for the returned msg and must be freed at 
 *        some point using the slurm_free_functions
 * IN open_fd                 - file descriptor to receive msg on
 * OUT msg                 - a slurm_msg struct to be filled in by the function
 * RET int                - size of msg received in bytes before being unpacked
 */
int slurm_receive_msg(slurm_fd fd, slurm_msg_t * msg, int timeout)
{
        char *buf = NULL;
        size_t buflen = 0;
        
        header_t header;
        int rc;
        void *auth_cred;
        Buf buffer;

        xassert(fd >= 0);

        if ((timeout*=1000) == 0)
                timeout = SLURM_DEFAULT_TIMEOUT;

        /*
         * Receive a msg. slurm_msg_recvfrom() will read the message
         *  length and allocate space on the heap for a buffer containing
         *  the message. 
         */
        if (_slurm_msg_recvfrom_timeout(fd, &buf, &buflen, 0, timeout) < 0) {
                error("recvfrom_timeout: %m");
                return SLURM_ERROR;
        }

#if        _DEBUG
         _print_data (buftemp,rc);
#endif
        buffer = create_buf(buf, buflen);

        unpack_header(&header, buffer);

        if (check_header_version(&header) < 0) {
                free_buf(buffer);
                slurm_seterrno_ret(SLURM_PROTOCOL_VERSION_ERROR);
        }

        if ((auth_cred = g_slurm_auth_unpack(buffer)) == NULL) {
                error( "authentication: %s ",
                        g_slurm_auth_errstr(g_slurm_auth_errno(NULL)));
                free_buf(buffer);
                slurm_seterrno_ret(ESLURM_PROTOCOL_INCOMPLETE_PACKET);
        }

        rc = g_slurm_auth_verify( auth_cred, NULL, 2 );

        if (rc != SLURM_SUCCESS) {
                error( "authentication: %s ",
                       g_slurm_auth_errstr(g_slurm_auth_errno(auth_cred)));
                (void) g_slurm_auth_destroy(auth_cred);
                free_buf(buffer);
                slurm_seterrno_ret(SLURM_PROTOCOL_AUTHENTICATION_ERROR);
        }        

        /*
         * Unpack message body 
         */
        msg->msg_type = header.msg_type;
        if ( (header.body_length > remaining_buf(buffer)) 
           || (unpack_msg(msg, buffer) != SLURM_SUCCESS)  ) {
                (void) g_slurm_auth_destroy(auth_cred);
                free_buf(buffer);
                slurm_seterrno_ret(ESLURM_PROTOCOL_INCOMPLETE_PACKET);
        }

        msg->cred = (void *) auth_cred;

        free_buf(buffer);
        return SLURM_SUCCESS;
}

/**********************************************************************\
 * send message functions
\**********************************************************************/

/*
 *  Do the wonderful stuff that needs be done to pack msg
 *  and hdr into buffer
 */
static void
_pack_msg(slurm_msg_t *msg, header_t *hdr, Buf buffer)
{
        unsigned int tmplen, msglen;

        tmplen = get_buf_offset(buffer);
        pack_msg(msg, buffer);
        msglen = get_buf_offset(buffer) - tmplen;

        /* update header with correct cred and msg lengths */
        update_header(hdr, msglen);
        
        /* repack updated header */
        tmplen = get_buf_offset(buffer);
        set_buf_offset(buffer, 0);
        pack_header(hdr, buffer);
        set_buf_offset(buffer, tmplen);
}


/* 
 *  Send a slurm message over an open file descriptor `fd'
 *    Returns the size of the message sent in bytes, or -1 on failure.
 */
int slurm_send_node_msg(slurm_fd fd, slurm_msg_t * msg)
{
        header_t header;
        Buf      buffer;
        int      rc;
        void *   auth_cred;

        /* 
         * Initialize header with Auth credential and message type.
         */
        auth_cred = g_slurm_auth_create(NULL, 2);
        if (auth_cred == NULL) {
                error("authentication: %s",
                       g_slurm_auth_errstr(g_slurm_auth_errno(NULL)) );
                slurm_seterrno_ret(SLURM_PROTOCOL_AUTHENTICATION_ERROR);
        }

        init_header(&header, msg->msg_type, SLURM_PROTOCOL_NO_FLAGS);

        /*
         * Pack header into buffer for transmission
         */
        buffer = init_buf(0);
        pack_header(&header, buffer);

        /* 
         * Pack auth credential
         */
        if (g_slurm_auth_pack(auth_cred, buffer)) {
                error("authentication: %s",
                       g_slurm_auth_errstr(g_slurm_auth_errno(auth_cred)));
        }
        (void) g_slurm_auth_destroy(auth_cred);

        
        /*
         * Pack message into buffer
         */
        _pack_msg(msg, &header, buffer);

#if        _DEBUG
        _print_data (get_buf_data(buffer),get_buf_offset(buffer));
#endif
        /*
         * Send message
         */
        rc = _slurm_msg_sendto( fd, get_buf_data(buffer), 
                                get_buf_offset(buffer),
                                SLURM_PROTOCOL_NO_SEND_RECV_FLAGS );

        if (rc < 0) 
                error("slurm_msg_sendto: %m");

        free_buf(buffer);
        return rc;
}

/**********************************************************************\
 * stream functions
\**********************************************************************/

/* slurm_listen_stream
 * opens a stream server and listens on it
 * IN slurm_address         - slurm_addr to bind the server stream to
 * RET slurm_fd                - file descriptor of the stream created
 */
slurm_fd slurm_listen_stream(slurm_addr * slurm_address)
{
        return _slurm_listen_stream(slurm_address);
}

/* slurm_accept_stream
 * accepts a incomming stream connection on a stream server slurm_fd 
 * IN open_fd                - file descriptor to accept connection on
 * OUT slurm_address         - slurm_addr of the accepted connection
 * RET slurm_fd                - file descriptor of the accepted connection 
 */
slurm_fd slurm_accept_stream(slurm_fd open_fd, slurm_addr * slurm_address)
{
        return _slurm_accept_stream(open_fd, slurm_address);
}

/* slurm_open_stream
 * opens a client connection to stream server
 * IN slurm_address         - slurm_addr of the connection destination
 * RET slurm_fd         - file descriptor of the connection created
 */
slurm_fd slurm_open_stream(slurm_addr * slurm_address)
{
        return _slurm_open_stream(slurm_address);
}

/* slurm_write_stream
 * writes a buffer out a stream file descriptor
 * IN open_fd                - file descriptor to write on
 * IN buffer                - buffer to send
 * IN size                - size of buffer send
 * IN timeout                - how long to wait in milliseconds
 * RET size_t                - bytes sent , or -1 on errror
 */
size_t slurm_write_stream(slurm_fd open_fd, char *buffer, size_t size)
{
        return _slurm_send_timeout(open_fd, buffer, size,
                                   SLURM_PROTOCOL_NO_SEND_RECV_FLAGS,
                                   SLURM_MESSGE_TIMEOUT_MSEC_STATIC);
}
size_t slurm_write_stream_timeout(slurm_fd open_fd, char *buffer,
                                  size_t size, int timeout)
{
        return _slurm_send_timeout(open_fd, buffer, size,
                                   SLURM_PROTOCOL_NO_SEND_RECV_FLAGS,
                                   timeout);
}

/* slurm_read_stream
 * read into buffer grom a stream file descriptor
 * IN open_fd                - file descriptor to read from
 * OUT buffer        - buffer to receive into
 * IN size                - size of buffer
 * IN timeout                - how long to wait in milliseconds
 * RET size_t                - bytes read , or -1 on errror
 */
size_t slurm_read_stream(slurm_fd open_fd, char *buffer, size_t size)
{
        return _slurm_recv_timeout(open_fd, buffer, size,
                                   SLURM_PROTOCOL_NO_SEND_RECV_FLAGS,
                                   SLURM_MESSGE_TIMEOUT_MSEC_STATIC);
}
size_t slurm_read_stream_timeout(slurm_fd open_fd, char *buffer,
                                 size_t size, int timeout)
{
        return _slurm_recv_timeout(open_fd, buffer, size,
                                   SLURM_PROTOCOL_NO_SEND_RECV_FLAGS,
                                   timeout);
}

/* slurm_get_stream_addr
 * esentially a encapsilated get_sockname  
 * IN open_fd                 - file descriptor to retreive slurm_addr for
 * OUT address                - address that open_fd to bound to
 */
int slurm_get_stream_addr(slurm_fd open_fd, slurm_addr * address)
{
        return _slurm_get_stream_addr(open_fd, address);
}

/* slurm_close_stream
 * closes either a server or client stream file_descriptor
 * IN open_fd        - an open file descriptor to close
 * RET int        - the return code
 */
int slurm_close_stream(slurm_fd open_fd)
{
        return _slurm_close_stream(open_fd);
}

/* make an open slurm connection blocking or non-blocking
 *        (i.e. wait or do not wait for i/o completion )
 * IN open_fd        - an open file descriptor to change the effect
 * RET int        - the return code
 */
int slurm_set_stream_non_blocking(slurm_fd open_fd)
{
        return _slurm_set_stream_non_blocking(open_fd);
}
int slurm_set_stream_blocking(slurm_fd open_fd)
{
        return _slurm_set_stream_blocking(open_fd);
}

/**********************************************************************\
 * address conversion and management functions
\**********************************************************************/

/* slurm_set_addr_uint
 * initializes the slurm_address with the supplied port and ip_address
 * OUT slurm_address        - slurm_addr to be filled in
 * IN port                - port in host order
 * IN ip_address        - ipv4 address in uint32 host order form
 */
void slurm_set_addr_uint(slurm_addr * slurm_address, uint16_t port,
                         uint32_t ip_address)
{
        _slurm_set_addr_uint(slurm_address, port, ip_address);
}

/* slurm_set_addr_any
 * initialized the slurm_address with the supplied port on INADDR_ANY
 * OUT slurm_address        - slurm_addr to be filled in
 * IN port                - port in host order
 */
void slurm_set_addr_any(slurm_addr * slurm_address, uint16_t port)
{
        _slurm_set_addr_uint(slurm_address, port, SLURM_INADDR_ANY);
}

/* slurm_set_addr
 * initializes the slurm_address with the supplied port and host name
 * OUT slurm_address        - slurm_addr to be filled in
 * IN port                - port in host order
 * IN host                - hostname or dns name 
 */
void slurm_set_addr(slurm_addr * slurm_address, uint16_t port, char *host)
{
        _slurm_set_addr_char(slurm_address, port, host);
}

/* reset_slurm_addr
 * resets the address field of a slurm_addr, port and family unchanged
 * OUT slurm_address        - slurm_addr to be reset in
 * IN new_address        - source of address to write into slurm_address
 */
void reset_slurm_addr(slurm_addr * slurm_address, slurm_addr new_address)
{
        _reset_slurm_addr(slurm_address, new_address);
}

/* slurm_set_addr_char
 * initializes the slurm_address with the supplied port and host
 * OUT slurm_address        - slurm_addr to be filled in
 * IN port                - port in host order
 * IN host                - hostname or dns name 
 */
void slurm_set_addr_char(slurm_addr * slurm_address, uint16_t port,
                         char *host)
{
        _slurm_set_addr_char(slurm_address, port, host);
}

/* slurm_get_addr 
 * given a slurm_address it returns its port and hostname
 * IN slurm_address        - slurm_addr to be queried
 * OUT port                - port number
 * OUT host                - hostname
 * IN buf_len                - length of hostname buffer
 */
void slurm_get_addr(slurm_addr * slurm_address, uint16_t * port,
                    char *host, unsigned int buf_len)
{
        _slurm_get_addr(slurm_address, port, host, buf_len);
}

/* slurm_get_peer_addr
 * get the slurm address of the peer connection, similar to getpeeraddr
 * IN fd                - an open connection
 * OUT slurm_address        - place to park the peer's slurm_addr
 */
int slurm_get_peer_addr(slurm_fd fd, slurm_addr * slurm_address)
{
        struct sockaddr name;
        socklen_t namelen = (socklen_t) sizeof(struct sockaddr);
        int rc;

        if ((rc = _slurm_getpeername((int) fd, &name, &namelen)))
                return rc;
        memcpy(slurm_address, &name, sizeof(slurm_addr));
        return 0;
}

/* slurm_print_slurm_addr
 * prints a slurm_addr into a buf
 * IN address                - slurm_addr to print
 * IN buf                - space for string representation of slurm_addr
 * IN n                        - max number of bytes to write (including NUL)
 */
void slurm_print_slurm_addr(slurm_addr * address, char *buf, size_t n)
{
        _slurm_print_slurm_addr(address, buf, n);
}

/**********************************************************************\
 * slurm_addr pack routines
\**********************************************************************/

/* slurm_pack_slurm_addr
 * packs a slurm_addr into a buffer to serialization transport
 * IN slurm_address        - slurm_addr to pack
 * IN/OUT buffer        - buffer to pack the slurm_addr into
 */
void slurm_pack_slurm_addr(slurm_addr * slurm_address, Buf buffer)
{
        _slurm_pack_slurm_addr(slurm_address, buffer);
}

/* slurm_pack_slurm_addr
 * unpacks a buffer into a slurm_addr after serialization transport
 * OUT slurm_address        - slurm_addr to unpack to
 * IN/OUT buffer        - buffer to upack the slurm_addr from
 * returns                 - SLURM error code
 */
int slurm_unpack_slurm_addr_no_alloc(slurm_addr * slurm_address,
                                     Buf buffer)
{
        return _slurm_unpack_slurm_addr_no_alloc(slurm_address, buffer);
}

/**********************************************************************\
 * simplified communication routines 
 * They open a connection do work then close the connection all within 
 * the function
\**********************************************************************/

/* slurm_send_rc_msg
 * given the original request message this function sends a 
 *        slurm_return_code message back to the client that made the request
 * IN request_msg        - slurm_msg the request msg
 * IN rc                 - the return_code to send back to the client
 */
void slurm_send_rc_msg(slurm_msg_t *msg, int rc)
{
        slurm_msg_t resp_msg;
        return_code_msg_t rc_msg;

        rc_msg.return_code = rc;

        resp_msg.address  = msg->address;
        resp_msg.msg_type = RESPONSE_SLURM_RC;
        resp_msg.data     = &rc_msg;

        /* send message */
        slurm_send_node_msg(msg->conn_fd, &resp_msg);
}

/*
 * Send and recv a slurm request and response on the open slurm descriptor
 */
static int 
_send_and_recv_msg(slurm_fd fd, slurm_msg_t *req, slurm_msg_t *resp,
                   int timeout)
{
        int err = SLURM_SUCCESS;

        if (  (slurm_send_node_msg(fd, req) < 0)
           || (slurm_receive_msg(fd, resp, timeout)  < 0) ) 
                err = errno;

        /* 
         *  Attempt to close an open connection
         */
        if (slurm_shutdown_msg_conn(fd) < 0)
                return SLURM_ERROR;

        if (err) slurm_seterrno_ret(err);

        return SLURM_SUCCESS;
}

/* slurm_send_recv_controller_msg
 * opens a connection to the controller, sends the controller a message, 
 * listens for the response, then closes the connection
 * IN request_msg        - slurm_msg request
 * OUT response_msg        - slurm_msg response
 * RET int                 - return code
 */
int slurm_send_recv_controller_msg(slurm_msg_t *req, slurm_msg_t *resp)
{
        slurm_fd fd = -1;

        if ((fd = slurm_open_controller_conn()) < 0) 
                return SLURM_SOCKET_ERROR;

        return _send_and_recv_msg(fd, req, resp, 0);
}

/* slurm_send_recv_node_msg
 * opens a connection to node, sends the node a message, listens 
 * for the response, then closes the connection
 * IN request_msg        - slurm_msg request
 * OUT response_msg        - slurm_msg response
 * RET int                 - return code
 */
int slurm_send_recv_node_msg(slurm_msg_t *req, slurm_msg_t *resp, int timeout)
{
        slurm_fd fd = -1;

        if ((fd = slurm_open_msg_conn(&req->address)) < 0)
                return SLURM_SOCKET_ERROR;

        return _send_and_recv_msg(fd, req, resp, timeout);

}

/* slurm_send_only_controller_msg
 * opens a connection to the controller, sends the controller a 
 * message then, closes the connection
 * IN request_msg        - slurm_msg request
 * RET int                 - return code
 */
int slurm_send_only_controller_msg(slurm_msg_t *req)
{
        int      rc = SLURM_SUCCESS;
        slurm_fd fd = -1;

        /*
         *  Open connection to SLURM controller:
         */
        if ((fd = slurm_open_controller_conn()) < 0)
                return SLURM_SOCKET_ERROR;

        rc = slurm_send_node_msg(fd, req);

        if (slurm_shutdown_msg_conn(fd) < 0)
                return SLURM_SOCKET_ERROR;

        return rc;

}

/* 
 *  Open a connection to the "address" specified in the slurm msg `req'
 *   Then, immediately close the connection w/out waiting for a reply.
 *
 *   Returns SLURM_SUCCESS on success SLURM_FAILURE (< 0) for failure.
 */
int slurm_send_only_node_msg(slurm_msg_t *req)
{
        int      rc = SLURM_SUCCESS;
        slurm_fd fd = -1;

        if ((fd = slurm_open_msg_conn(&req->address)) < 0)
                return SLURM_SOCKET_ERROR;

        rc = slurm_send_node_msg(fd, req);

        if (slurm_shutdown_msg_conn(fd) < 0)
                return SLURM_SOCKET_ERROR;

        return rc;
}


/*
 *  Send message and recv "return code" message on an already open
 *    slurm file descriptor
 */
static int _send_recv_rc_msg(slurm_fd fd, slurm_msg_t *req, int *rc,
                             int timeout)
{
        int                retval = SLURM_SUCCESS;
        slurm_msg_t        msg;
 
        retval = _send_and_recv_msg(fd, req, &msg, timeout);

        if (retval != SLURM_SUCCESS) 
                goto done;

        if (msg.msg_type != RESPONSE_SLURM_RC) 
                slurm_seterrno_ret(SLURM_UNEXPECTED_MSG_ERROR);

        *rc = ((return_code_msg_t *) msg.data)->return_code;
        slurm_free_return_code_msg(msg.data);

     done:
        return retval;
}

/*
 *  Open a connection to the "address" specified in the the slurm msg "req"
 *    Then read back an "rc" message returning the "return_code" specified
 *    in the response in the "rc" parameter.
 */
int slurm_send_recv_rc_msg(slurm_msg_t *req, int *rc, int timeout)
{
        slurm_fd fd = -1;

        if ((fd = slurm_open_msg_conn(&req->address)) < 0) 
                return SLURM_SOCKET_ERROR;

        return _send_recv_rc_msg(fd, req, rc, timeout);
}

/*
 *  Same as above, but send message to controller
 */
int slurm_send_recv_controller_rc_msg(slurm_msg_t *req, int *rc)
{
        slurm_fd fd = -1;

        if ((fd = slurm_open_controller_conn()) < 0)
                return SLURM_SOCKET_ERROR;

        return _send_recv_rc_msg(fd, req, rc, 0);
}

/*
 * Free a slurm message
 */
void slurm_free_msg(slurm_msg_t * msg)
{
        (void) g_slurm_auth_destroy(msg->cred);
        xfree(msg);
}


#if _DEBUG

static void _print_data(char *data, int len)
{
        int i;
        for (i = 0; i < len; i++) {
                if ((i % 10 == 0) && (i != 0))
                        printf("\n");
                printf("%2.2x ", ((int) data[i] & 0xff));
                if (i >= 200)
                        break;
        }
        printf("\n\n");
}

#endif


/*
 * vi: shiftwidth=8 tabstop=8 expandtab
 */

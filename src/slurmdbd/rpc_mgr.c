/*****************************************************************************\
 *  rpc_mgr.h - functions for processing RPCs.
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://computing.llnl.gov/linux/slurm/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/time.h>

#include "src/common/fd.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_accounting_storage.h"
#include "src/common/slurmdbd_defs.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"
#include "src/slurmdbd/proc_req.h"
#include "src/slurmdbd/read_config.h"
#include "src/slurmdbd/rpc_mgr.h"
#include "src/slurmdbd/slurmdbd.h"

#define MAX_THREAD_COUNT 50

/* Local functions */
static bool   _fd_readable(slurm_fd fd);
static void   _free_server_thread(pthread_t my_tid);
static int    _send_resp(slurm_fd fd, Buf buffer);
static void * _service_connection(void *arg);
static void   _sig_handler(int signal);
static int    _tot_wait (struct timeval *start_time);
static int    _wait_for_server_thread(void);
static void   _wait_for_thread_fini(void);

/* Local variables */
static pthread_t       master_thread_id = 0, slave_thread_id[MAX_THREAD_COUNT];
static int             thread_count = 0;
static pthread_mutex_t thread_count_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  thread_count_cond = PTHREAD_COND_INITIALIZER;


/* Process incoming RPCs. Meant to execute as a pthread */
extern void *rpc_mgr(void *no_data)
{
	pthread_attr_t thread_attr_rpc_req;
	slurm_fd sockfd, newsockfd;
	int i, retry_cnt, sigarray[] = {SIGUSR1, 0};
	slurm_addr_t cli_addr;
	slurmdbd_conn_t *conn_arg = NULL;

	slurm_mutex_lock(&thread_count_lock);
	master_thread_id = pthread_self();
	slurm_mutex_unlock(&thread_count_lock);

	(void) pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	(void) pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	/* threads to process individual RPC's are detached */
	slurm_attr_init(&thread_attr_rpc_req);
	if (pthread_attr_setdetachstate
	    (&thread_attr_rpc_req, PTHREAD_CREATE_DETACHED))
		fatal("pthread_attr_setdetachstate %m");

	/* initialize port for RPCs */
	if ((sockfd = slurm_init_msg_engine_port(get_dbd_port()))
	    == SLURM_SOCKET_ERROR)
		fatal("slurm_init_msg_engine_port error %m");

	/* Prepare to catch SIGUSR1 to interrupt accept().
	 * This signal is generated by the slurmdbd signal
	 * handler thread upon receipt of SIGABRT, SIGINT,
	 * or SIGTERM. That thread does all processing of
	 * all signals. */
	xsignal(SIGUSR1, _sig_handler);
	xsignal_unblock(sigarray);

	/*
	 * Process incoming RPCs until told to shutdown
	 */
	while ((i = _wait_for_server_thread()) >= 0) {
		/*
		 * accept needed for stream implementation is a no-op in
		 * message implementation that just passes sockfd to newsockfd
		 */
		if ((newsockfd = slurm_accept_msg_conn(sockfd,
						       &cli_addr)) ==
		    SLURM_SOCKET_ERROR) {
			_free_server_thread((pthread_t) 0);
			if (errno != EINTR)
				error("slurm_accept_msg_conn: %m");
			continue;
		}
		fd_set_nonblocking(newsockfd);

		conn_arg = xmalloc(sizeof(slurmdbd_conn_t));
		conn_arg->newsockfd = newsockfd;
		slurm_get_ip_str(&cli_addr, &conn_arg->orig_port,
				 conn_arg->ip, sizeof(conn_arg->ip));
		retry_cnt = 0;
		while (pthread_create(&slave_thread_id[i],
				      &thread_attr_rpc_req,
				      _service_connection,
				      (void *) conn_arg)) {
			if (retry_cnt > 0) {
				error("pthread_create failure, "
				      "aborting RPC: %m");
				close(newsockfd);
				break;
			}
			error("pthread_create failure: %m");
			retry_cnt++;
			usleep(1000);	/* retry in 1 msec */
		}
	}

	debug3("rpc_mgr shutting down");
	slurm_attr_destroy(&thread_attr_rpc_req);
	(void) slurm_shutdown_msg_engine(sockfd);
	_wait_for_thread_fini();
	pthread_exit((void *) 0);
	return NULL;
}

/* Wake up the RPC manager and all spawned threads so they can exit */
extern void rpc_mgr_wake(void)
{
	int i;

	slurm_mutex_lock(&thread_count_lock);
	if (master_thread_id)
		pthread_kill(master_thread_id, SIGUSR1);
	for (i=0; i<MAX_THREAD_COUNT; i++) {
		if (slave_thread_id[i])
			pthread_kill(slave_thread_id[i], SIGUSR1);
	}
	slurm_mutex_unlock(&thread_count_lock);
}

static void * _service_connection(void *arg)
{
	slurmdbd_conn_t *conn = (slurmdbd_conn_t *) arg;
	uint32_t nw_size = 0, msg_size = 0, uid = NO_VAL;
	char *msg = NULL;
	ssize_t msg_read = 0, offset = 0;
	bool fini = false, first = true;
	Buf buffer = NULL;
	int rc = SLURM_SUCCESS;

	debug2("Opened connection %d from %s", conn->newsockfd, conn->ip);

	while (!fini) {
		if (!_fd_readable(conn->newsockfd))
			break;		/* problem with this socket */
		msg_read = read(conn->newsockfd, &nw_size, sizeof(nw_size));
		if (msg_read == 0)	/* EOF */
			break;
		if (msg_read != sizeof(nw_size)) {
			error("Could not read msg_size from "
			      "connection %d(%s) uid(%d)",
			      conn->newsockfd, conn->ip, uid);
			break;
		}
		msg_size = ntohl(nw_size);
		if ((msg_size < 2) || (msg_size > 1000000)) {
			error("Invalid msg_size (%u) from "
			      "connection %d(%s) uid(%d)",
			      msg_size, conn->newsockfd, conn->ip, uid);
			break;
		}

		msg = xmalloc(msg_size);
		offset = 0;
		while (msg_size > offset) {
			if (!_fd_readable(conn->newsockfd))
				break;		/* problem with this socket */
			msg_read = read(conn->newsockfd, (msg + offset),
					(msg_size - offset));
			if (msg_read <= 0) {
				error("read(%d): %m", conn->newsockfd);
				break;
			}
			offset += msg_read;
		}
		if (msg_size == offset) {
			rc = proc_req(
				conn, msg, msg_size, first, &buffer, &uid);
			first = false;
			if (rc != SLURM_SUCCESS && rc != ACCOUNTING_FIRST_REG) {
				error("Processing last message from "
				      "connection %d(%s) uid(%d)",
				      conn->newsockfd, conn->ip, uid);
				if (rc == ESLURM_ACCESS_DENIED
				    || rc == SLURM_PROTOCOL_VERSION_ERROR)
					fini = true;
			}
		} else {
			buffer = make_dbd_rc_msg(conn->rpc_version,
						 SLURM_ERROR, "Bad offset", 0);
			fini = true;
		}

		rc = _send_resp(conn->newsockfd, buffer);
		xfree(msg);
	}

	acct_storage_g_close_connection(&conn->db_conn);
	if (slurm_close_accepted_conn(conn->newsockfd) < 0)
		error("close(%d): %m(%s)",  conn->newsockfd, conn->ip);
	else
		debug2("Closed connection %d uid(%d)", conn->newsockfd, uid);
	xfree(conn->cluster_name);
	xfree(conn);
	_free_server_thread(pthread_self());
	return NULL;
}

/* Return a buffer containing a DBD_RC (return code) message
 * caller must free returned buffer */
extern Buf make_dbd_rc_msg(uint16_t rpc_version,
			   int rc, char *comment, uint16_t sent_type)
{
	Buf buffer;

	dbd_rc_msg_t msg;
	buffer = init_buf(1024);
	pack16((uint16_t) DBD_RC, buffer);
	msg.return_code  = rc;
	msg.comment  = comment;
	msg.sent_type  = sent_type;
	slurmdbd_pack_rc_msg(&msg, rpc_version, buffer);
	return buffer;
}

static int _send_resp(slurm_fd fd, Buf buffer)
{
	uint32_t msg_size, nw_size;
	ssize_t msg_wrote;
	char *out_buf;

	if ((fd < 0) || (!fd_writeable(fd)))
		goto io_err;

	msg_size = get_buf_offset(buffer);
	nw_size = htonl(msg_size);
	if (!fd_writeable(fd))
		goto io_err;
	msg_wrote = write(fd, &nw_size, sizeof(nw_size));
	if (msg_wrote != sizeof(nw_size))
		goto io_err;

	out_buf = get_buf_data(buffer);
	while (msg_size > 0) {
		if (!fd_writeable(fd))
			goto io_err;
		msg_wrote = write(fd, out_buf, msg_size);
		if (msg_wrote <= 0)
			goto io_err;
		out_buf  += msg_wrote;
		msg_size -= msg_wrote;
	}
	free_buf(buffer);
	return SLURM_SUCCESS;

io_err:
	free_buf(buffer);
	return SLURM_ERROR;
}

/* Return time in msec since "start time" */
static int _tot_wait (struct timeval *start_time)
{
	struct timeval end_time;
	int msec_delay;

	gettimeofday(&end_time, NULL);
	msec_delay =   (end_time.tv_sec  - start_time->tv_sec ) * 1000;
	msec_delay += ((end_time.tv_usec - start_time->tv_usec + 500) / 1000);
	return msec_delay;
}

/* Wait until a file is readable, return false if can not be read */
static bool _fd_readable(slurm_fd fd)
{
	struct pollfd ufds;
	int rc;

	ufds.fd     = fd;
	ufds.events = POLLIN;
	while (1) {
		rc = poll(&ufds, 1, -1);
		if (shutdown_time)
			return false;
		if (rc == -1) {
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			error("poll: %m");
			return false;
		}
		if (ufds.revents & POLLHUP) {
			debug3("Read connection %d closed", fd);
			return false;
		}
		if (ufds.revents & POLLNVAL) {
			error("Connection %d is invalid", fd);
			return false;
		}
		if (ufds.revents & POLLERR) {
			error("Connection %d experienced an error", fd);
			return false;
		}
		if ((ufds.revents & POLLIN) == 0) {
			error("Connection %d events %d", fd, ufds.revents);
			return false;
		}
		break;
	}
	return true;
}

/* Wait until a file is writeable,
 * RET false if can not be written to within 5 seconds */
extern bool fd_writeable(slurm_fd fd)
{
	struct pollfd ufds;
	int msg_timeout = 5000;
	int rc, time_left;
	struct timeval tstart;
	char temp[2];

	ufds.fd     = fd;
	ufds.events = POLLOUT;
	gettimeofday(&tstart, NULL);
	while (shutdown_time == 0) {
		time_left = msg_timeout - _tot_wait(&tstart);
		rc = poll(&ufds, 1, time_left);
		if (shutdown_time)
			return false;
		if (rc == -1) {
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			error("poll: %m");
			return false;
		}
		if (rc == 0) {
			debug2("write timeout");
			return false;
		}

		/*
		 * Check here to make sure the socket really is there.
		 * If not then exit out and notify the sender.  This
 		 * is here since a write doesn't always tell you the
		 * socket is gone, but getting 0 back from a
		 * nonblocking read means just that.
		 */
		if (ufds.revents & POLLHUP || (recv(fd, &temp, 1, 0) == 0)) {
			debug3("Write connection %d closed", fd);
			return false;
		}
		if (ufds.revents & POLLNVAL) {
			error("Connection %d is invalid", fd);
			return false;
		}
		if (ufds.revents & POLLERR) {
			error("Connection %d experienced an error", fd);
			return false;
		}
		if ((ufds.revents & POLLOUT) == 0) {
			error("Connection %d events %d", fd, ufds.revents);
			return false;
		}
		break;
	}
	return true;
}

/* Increment thread_count and don't return until its value is no larger
 *	than MAX_THREAD_COUNT,
 * RET index of free index in slave_pthread_id or -1 to exit */
static int _wait_for_server_thread(void)
{
	bool print_it = true;
	int i, rc = -1;

	slurm_mutex_lock(&thread_count_lock);
	while (1) {
		if (shutdown_time)
			break;

		if (thread_count < MAX_THREAD_COUNT) {
			thread_count++;
			for (i=0; i<MAX_THREAD_COUNT; i++) {
				if (slave_thread_id[i])
					continue;
				rc = i;
				break;
			}
			if (rc == -1) {
				/* thread_count and slave_thread_id
				 * out of sync */
				fatal("No free slave_thread_id");
			}
			break;
		} else {
			/* wait for state change and retry,
			 * just a delay and not an error.
			 * This can happen when the epilog completes
			 * on a bunch of nodes at the same time, which
			 * can easily happen for highly parallel jobs. */
			if (print_it) {
				static time_t last_print_time = 0;
				time_t now = time(NULL);
				if (difftime(now, last_print_time) > 2) {
					verbose("thread_count over "
						"limit (%d), waiting",
						thread_count);
					last_print_time = now;
				}
				print_it = false;
			}
			pthread_cond_wait(&thread_count_cond,
			                  &thread_count_lock);
		}
	}
	slurm_mutex_unlock(&thread_count_lock);
	return rc;
}

/* my_tid IN - Thread ID of spawned thread, 0 if no thread spawned */
static void _free_server_thread(pthread_t my_tid)
{
	int i;

	slurm_mutex_lock(&thread_count_lock);
	if (thread_count > 0)
		thread_count--;
	else
		error("thread_count underflow");

	if (my_tid) {
		for (i=0; i<MAX_THREAD_COUNT; i++) {
			if (slave_thread_id[i] != my_tid)
				continue;
			slave_thread_id[i] = (pthread_t) 0;
			break;
		}
		if (i >= MAX_THREAD_COUNT)
			error("Could not find slave_thread_id");
	}

	pthread_cond_broadcast(&thread_count_cond);
	slurm_mutex_unlock(&thread_count_lock);
}

/* Wait for all RPC handler threads to exit.
 * After one second, start sending SIGKILL to the threads. */
static void _wait_for_thread_fini(void)
{
	int i, j;

	if (thread_count == 0)
		return;
	usleep(500000);	/* Give the threads 500 msec to clean up */

	/* Interupt any hung I/O */
	slurm_mutex_lock(&thread_count_lock);
	for (j=0; j<MAX_THREAD_COUNT; j++) {
		if (slave_thread_id[j] == 0)
			continue;
		pthread_kill(slave_thread_id[j], SIGUSR1);
	}
	slurm_mutex_unlock(&thread_count_lock);
	usleep(100000);	/* Give the threads 100 msec to clean up */

	for (i=0; ; i++) {
		if (thread_count == 0)
			return;

		slurm_mutex_lock(&thread_count_lock);
		for (j=0; j<MAX_THREAD_COUNT; j++) {
			if (slave_thread_id[j] == 0)
				continue;
			info("rpc_mgr sending SIGKILL to thread %u",
			     slave_thread_id[j]);
			if (pthread_kill(slave_thread_id[j], SIGKILL)) {
				slave_thread_id[j] = 0;
				if (thread_count > 0)
					thread_count--;
				else
					error("thread_count underflow");
			}
		}
		slurm_mutex_unlock(&thread_count_lock);
		sleep(1);
	}
}

static void _sig_handler(int signal)
{
}

/*****************************************************************************\
 *  con.c - definitions for connection handlers in connection manager
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
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
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#define _GNU_SOURCE
#include <limits.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__)
#include <sys/param.h>
#include <sys/ucred.h>
#endif

#if defined(__linux__)
#include <sys/sysmacros.h>
#endif /* __linux__ */

#include "slurm/slurm.h"

#include "src/common/fd.h"
#include "src/common/macros.h"
#include "src/common/net.h"
#include "src/common/pack.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_socket.h"
#include "src/common/util-net.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/conmgr/conmgr.h"
#include "src/conmgr/mgr.h"
#include "src/conmgr/poll.h"

#define T(type) { type, XSTRINGIFY(type) }
static const struct {
	conmgr_con_type_t type;
	const char *string;
} con_types[] = {
	T(CON_TYPE_RAW),
	T(CON_TYPE_RPC),
};
#undef T

typedef struct {
	conmgr_events_t events;
	void *arg;
	conmgr_con_type_t type;
	int rc;
} socket_listen_init_t;

typedef struct {
#define MAGIC_RECEIVE_FD 0xeba8bae0
	int magic; /* MAGIC_RECEIVE_FD */
	conmgr_con_type_t type;
	conmgr_events_t events;
	void *arg;
} receive_fd_args_t;

typedef struct {
#define MAGIC_SEND_FD 0xfbf8e2e0
	int magic; /* MAGIC_SEND_FD */
	int fd; /* fd to send over con */
} send_fd_args_t;

static void _validate_pctl_type(pollctl_fd_type_t type)
{
	xassert(type > PCTL_TYPE_INVALID);
	xassert(type < PCTL_TYPE_INVALID_MAX);
}

extern const char *conmgr_con_type_string(conmgr_con_type_t type)
{
	for (int i = 0; i < ARRAY_SIZE(con_types); i++)
		if (con_types[i].type == type)
			return con_types[i].string;

	fatal_abort("invalid type");
}

/*
 * Close all connections (for_each)
 * NOTE: must hold mgr.mutex
 */
static int _close_con_for_each(void *x, void *arg)
{
	conmgr_fd_t *con = x;
	close_con(true, con);
	return 1;
}

/* mgr.mutex must be locked when calling this function */
extern void close_all_connections(void)
{
	/* close all connections */
	list_for_each(mgr.connections, _close_con_for_each, NULL);
	list_for_each(mgr.listen_conns, _close_con_for_each, NULL);
}

/*
 * Stop reading from connection but write out the remaining buffer and finish
 * any queued work
 */
extern void close_con(bool locked, conmgr_fd_t *con)
{
	if (!locked)
		slurm_mutex_lock(&mgr.mutex);

	if (con->input_fd < 0) {
		xassert(con->read_eof);
		xassert(!con->can_read);
		log_flag(CONMGR, "%s: [%s] ignoring duplicate close request",
			 __func__, con->name);
		goto cleanup;
	}

	log_flag(CONMGR, "%s: [%s] closing input", __func__, con->name);

	/* unlink listener sockets to avoid leaving ghost socket */
	if (con->is_listen && con->unix_socket &&
	    (unlink(con->unix_socket) == -1))
		error("%s: unable to unlink %s: %m",
		      __func__, con->unix_socket);

	/*
	 * Stop polling read/write to input fd to allow handle_connection() to
	 * select what needs to be monitored
	 */
	con_set_polling(con, PCTL_TYPE_NONE, __func__);

	/* mark it as EOF even if it hasn't */
	con->read_eof = true;
	con->can_read = false;

	/* drop any unprocessed input buffer */
	if (con->in)
		set_buf_offset(con->in, 0);

	if (con->is_listen) {
		if (close(con->input_fd) == -1)
			log_flag(CONMGR, "%s: [%s] unable to close listen fd %d: %m",
				 __func__, con->name, con->output_fd);
		xassert(con->output_fd <= 0);
	} else if (con->input_fd != con->output_fd) {
		/* different input FD, we can close it now */
		if (close(con->input_fd) == -1)
			log_flag(CONMGR, "%s: [%s] unable to close input fd %d: %m",
				 __func__, con->name, con->output_fd);
	} else if (con->is_socket && shutdown(con->input_fd, SHUT_RD) == -1) {
		/* shutdown input on sockets */
		log_flag(CONMGR, "%s: [%s] unable to shutdown read: %m",
			 __func__, con->name);
	}

	/* forget the now invalid FD */
	con->input_fd = -1;

	EVENT_SIGNAL(&mgr.watch_sleep);
cleanup:
	if (!locked)
		slurm_mutex_unlock(&mgr.mutex);
}

static char *_resolve_tty_name(int fd)
{
	char buf[PATH_MAX] = {0};

	if (ttyname_r(fd, buf, (sizeof(buf) - 1))) {
		log_flag(CONMGR, "%s: unable to resolve tty at fd:%d: %m",
			 __func__, fd);
		return NULL;
	}

	return xstrdup(buf);
}

static char *_resolve_fd(int fd, struct stat *stat_ptr)
{
	char *name = NULL;

	if (S_ISSOCK(stat_ptr->st_mode)) {
		slurm_addr_t addr = {0};

		if (!slurm_get_stream_addr(fd, &addr) &&
		    (addr.ss_family != AF_UNSPEC) &&
		    (name = sockaddr_to_string(&addr, sizeof(addr))))
			return name;
	}

	if ((name = fd_resolve_path(fd)))
		return name;

	if (S_ISFIFO(stat_ptr->st_mode))
		return xstrdup_printf("pipe");

	if (S_ISCHR(stat_ptr->st_mode)) {
		if (isatty(fd) && (name = _resolve_tty_name(fd)))
			return name;

#if defined(__linux__)
		return xstrdup_printf("device:%u.%u", major(stat_ptr->st_dev),
				      minor(stat_ptr->st_dev));
#else /* !__linux__ */
		return xstrdup_printf("device:0x%x", stat_ptr->st_dev);
#endif /* !__linux__ */
	}

#if defined(__linux__)
	if (S_ISBLK(stat_ptr->st_mode))
		return xstrdup_printf("block:%u.%u", major(stat_ptr->st_dev),
				      minor(stat_ptr->st_dev));
#endif /* __linux__ */

	return NULL;
}

/* set connection name if one was not resolved already */
static void _set_connection_name(conmgr_fd_t *con, struct stat *in_stat,
				 struct stat *out_stat)
{
	xassert(con);
	xassert(!con->name);

	char *in_str = NULL, *out_str = NULL;
	const bool has_in = (con->input_fd >= 0);
	const bool has_out = (con->output_fd >= 0);
	bool is_same = (con->input_fd == con->output_fd);

	if (!has_in && !has_out) {
		con->name = xstrdup("INVALID");
		return;
	}

	/* grab socket peer if possible */
	if (con->is_socket && has_out)
		out_str = fd_resolve_peer(con->output_fd);

	if (has_out && !out_str)
		out_str = _resolve_fd(con->output_fd, out_stat);
	if (has_in)
		in_str = _resolve_fd(con->input_fd, in_stat);

	/* avoid "->" syntax if same on both sides */
	if (in_str && out_str && !xstrcmp(in_str, out_str)) {
		is_same = true;
		xfree(out_str);
	}

	if (is_same) {
		xstrfmtcat(con->name, "%s(fd:%d)", in_str,
			   con->input_fd);
	} else if (has_in && has_out) {
		xstrfmtcat(con->name, "%s(fd:%d)->%s(fd:%d)", in_str,
			   con->input_fd, out_str, con->output_fd);
	} else if (has_in && !has_out) {
		xstrfmtcat(con->name, "%s(fd:%d)->()", in_str, con->input_fd);
	} else if (!has_in && has_out) {
		xstrfmtcat(con->name, "()->%s(fd:%d)", out_str, con->output_fd);
	} else {
		xassert(false);
	}

	xfree(out_str);
	xfree(in_str);
}

static void _check_con_type(conmgr_fd_t *con, conmgr_con_type_t type)
{
#ifndef NDEBUG
	if (type == CON_TYPE_RAW) {
		/* must have on_data() defined */
		xassert(con->events.on_data);
	} else if (type == CON_TYPE_RPC) {
		/* must have on_msg() defined */
		xassert(con->events.on_msg);
	} else {
		fatal_abort("invalid type");
	}
#endif /* !NDEBUG */
}

extern int fd_change_mode(conmgr_fd_t *con, conmgr_con_type_t type)
{
	xassert(con->magic == MAGIC_CON_MGR_FD);

	_check_con_type(con, type);

	if (con->type == type) {
		log_flag(CONMGR, "%s: [%s] ignoring unchanged type: %s",
			 __func__, con->name, conmgr_con_type_string(type));
		return SLURM_SUCCESS;
	}

	log_flag(CONMGR, "%s: [%s] changing type: %s->%s pending_reads=%u pending_writes=%u",
		 __func__, con->name, conmgr_con_type_string(con->type),
		 conmgr_con_type_string(type), get_buf_offset(con->in),
		 list_count(con->out));

	con->type = type;

	return SLURM_SUCCESS;
}

extern int conmgr_fd_change_mode(conmgr_fd_t *con, conmgr_con_type_t type)
{
	int rc;

	slurm_mutex_lock(&mgr.mutex);
	rc = fd_change_mode(con, type);

	/* wake up watch() to send along any pending data */
	EVENT_SIGNAL(&mgr.watch_sleep);
	slurm_mutex_unlock(&mgr.mutex);

	return rc;
}

extern int add_connection(conmgr_con_type_t type,
			  conmgr_fd_t *source, int input_fd,
			  int output_fd,
			  const conmgr_events_t events,
			  const slurm_addr_t *addr,
			  socklen_t addrlen, bool is_listen,
			  const char *unix_socket_path, void *arg)
{
	struct stat in_stat = { 0 };
	struct stat out_stat = { 0 };
	conmgr_fd_t *con = NULL;
	bool set_keep_alive, is_socket;
	const bool has_in = (input_fd >= 0);
	const bool has_out = (output_fd >= 0);
	const bool is_same = (input_fd == output_fd);

	/* verify FD is valid and still open */
	if (has_in && fstat(input_fd, &in_stat)) {
		log_flag(CONMGR, "%s: invalid fd:%d: %m", __func__, input_fd);
		return SLURM_ERROR;
	}
	if (has_out && fstat(output_fd, &out_stat)) {
		log_flag(CONMGR, "%s: invalid fd:%d: %m", __func__, output_fd);
		return SLURM_ERROR;
	}

	is_socket = (has_in && S_ISSOCK(in_stat.st_mode)) ||
		    (has_out && S_ISSOCK(out_stat.st_mode));

	set_keep_alive = !unix_socket_path && is_socket && !is_listen;

	/* all connections are non-blocking */
	if (has_in) {
		if (set_keep_alive)
			net_set_keep_alive(input_fd);
		fd_set_nonblocking(input_fd);
	}
	if (!is_same && has_out) {
		fd_set_nonblocking(output_fd);

		if (set_keep_alive)
			net_set_keep_alive(output_fd);
	}

	con = xmalloc(sizeof(*con));
	*con = (conmgr_fd_t){
		.magic = MAGIC_CON_MGR_FD,

		.input_fd = input_fd,
		.read_eof = !has_in,
		.output_fd = output_fd,
		.events = events,
		/* save socket type to avoid calling fstat() again */
		.is_socket = is_socket,
		.mss = NO_VAL,
		.is_listen = is_listen,
		.work = list_create(NULL),
		.write_complete_work = list_create(NULL),
		.new_arg = arg,
		.type = type,
		.polling_input_fd = PCTL_TYPE_NONE,
		.polling_output_fd = PCTL_TYPE_NONE,
	};

	if (!is_listen) {
		con->in = create_buf(xmalloc(BUFFER_START_SIZE),
				     BUFFER_START_SIZE);
		con->out = list_create((ListDelF) free_buf);
	}

	/* listen on unix socket */
	if (unix_socket_path) {
		xassert(con->is_socket);
		xassert(addr->ss_family == AF_LOCAL);
		con->unix_socket = xstrdup(unix_socket_path);
	}

#ifndef NDEBUG
	if (source && source->unix_socket && con->unix_socket)
		xassert(!xstrcmp(source->unix_socket, con->unix_socket));
#endif

	if (source && source->unix_socket && !con->unix_socket)
		con->unix_socket = xstrdup(source->unix_socket);

	if (is_socket && (addrlen > 0) && addr)
		memcpy(&con->address, addr, addrlen);

	_set_connection_name(con, &in_stat, &out_stat);

	_check_con_type(con, type);

	log_flag(CONMGR, "%s: [%s] new connection input_fd=%u output_fd=%u",
		 __func__, con->name, input_fd, output_fd);

	slurm_mutex_lock(&mgr.mutex);
	if (is_listen) {
		xassert(con->output_fd <= 0);
		list_append(mgr.listen_conns, con);
	} else {
		list_append(mgr.connections, con);
	}

	/* interrupt poll () and wake up watch() to examine new connection */
	pollctl_interrupt(__func__);
	EVENT_SIGNAL(&mgr.watch_sleep);
	slurm_mutex_unlock(&mgr.mutex);

	return SLURM_SUCCESS;
}

extern void wrap_on_connection(conmgr_callback_args_t conmgr_args, void *arg)
{
	conmgr_fd_t *con = conmgr_args.con;

	log_flag(CONMGR, "%s: [%s] BEGIN func=0x%"PRIxPTR,
		 __func__, con->name,
		 (uintptr_t) con->events.on_connection);

	arg = con->events.on_connection(con, con->new_arg);

	log_flag(CONMGR, "%s: [%s] END func=0x%"PRIxPTR" arg=0x%"PRIxPTR,
		 __func__, con->name,
		 (uintptr_t) con->events.on_connection,
		 (uintptr_t) arg);

	if (!arg) {
		error("%s: [%s] closing connection due to NULL return from on_connection",
		      __func__, con->name);
		close_con(false, con);
		return;
	}

	slurm_mutex_lock(&mgr.mutex);
	con->arg = arg;

	EVENT_SIGNAL(&mgr.watch_sleep);
	slurm_mutex_unlock(&mgr.mutex);
}

extern int conmgr_process_fd(conmgr_con_type_t type, int input_fd,
			     int output_fd, const conmgr_events_t events,
			     const slurm_addr_t *addr, socklen_t addrlen,
			     void *arg)
{
	return add_connection(type, NULL, input_fd, output_fd, events, addr,
			      addrlen, false, NULL, arg);
}

extern int conmgr_process_fd_listen(int fd, conmgr_con_type_t type,
				    const conmgr_events_t events,
				    const slurm_addr_t *addr,
				    socklen_t addrlen, void *arg)
{
	return add_connection(type, NULL, fd, -1, events, addr, addrlen, true,
			      NULL, arg);
}

extern int conmgr_process_fd_unix_listen(conmgr_con_type_t type, int fd,
					  const conmgr_events_t events,
					  const slurm_addr_t *addr,
					  socklen_t addrlen, const char *path,
					  void *arg)
{
	return add_connection(type, NULL, fd, -1, events, addr, addrlen, true,
			      path, arg);
}

static void _receive_fd(conmgr_callback_args_t conmgr_args, void *arg)
{
	receive_fd_args_t *args = arg;
	conmgr_fd_t *src = conmgr_args.con;
	int fd = -1;

	xassert(args->magic == MAGIC_RECEIVE_FD);
	xassert(src->magic == MAGIC_CON_MGR_FD);

	if (conmgr_args.status == CONMGR_WORK_STATUS_CANCELLED) {
		log_flag(CONMGR, "%s: [%s] Canceled receive new file descriptor",
			 __func__, src->name);
	} else if (src->read_eof) {
		log_flag(CONMGR, "%s: [%s] Unable to receive new file descriptor on SHUT_RD input_fd=%d",
			 __func__, src->name, src->input_fd);
	} else if (src->input_fd < 0) {
		log_flag(CONMGR, "%s: [%s] Unable to receive new file descriptor on invalid input_fd=%d",
			 __func__, src->name, src->input_fd);
	} else if ((fd = receive_fd_over_socket(src->input_fd)) < 0) {
		log_flag(CONMGR, "%s: [%s] Unable to receive new file descriptor on input_fd=%d",
			 __func__, src->name, src->input_fd);
		/*
		 * Close source as receive_fd_over_socket() failed and
		 * connection is now in an unknown state
		 */
		close_con(false, src);
	} else if (add_connection(args->type, NULL, fd, fd,
				  args->events, NULL, 0, false, NULL,
				  args->arg) != SLURM_SUCCESS) {
		/*
		 * Error already logged by add_connection() and there is no
		 * reason to assume that failing is due to the state of src.
		 */
	}

	args->magic = ~MAGIC_RECEIVE_FD;
	xfree(args);
}

extern int conmgr_queue_receive_fd(conmgr_fd_t *src, conmgr_con_type_t type,
				   const conmgr_events_t events, void *arg)
{
	int rc = SLURM_ERROR;

	slurm_mutex_lock(&mgr.mutex);

	xassert(src->magic == MAGIC_CON_MGR_FD);
	xassert(type > CON_TYPE_INVALID);
	xassert(type < CON_TYPE_MAX);

	/* Reject obviously invalid states immediately */

	if (!src->is_socket) {
		log_flag(CONMGR, "%s: [%s] Unable to receive new file descriptor on non-socket",
			 __func__, src->name);
		rc = EAFNOSUPPORT;
	} else if (src->read_eof) {
		log_flag(CONMGR, "%s: [%s] Unable to receive new file descriptor on SHUT_RD input_fd=%d",
			 __func__, src->name, src->input_fd);
		rc = SLURM_COMMUNICATIONS_MISSING_SOCKET_ERROR;
	} else if (src->input_fd < 0) {
		log_flag(CONMGR, "%s: [%s] Unable to receive new file descriptor on invalid input_fd=%d",
			 __func__, src->name, src->input_fd);
		rc = SLURM_COMMUNICATIONS_MISSING_SOCKET_ERROR;
	} else {
		receive_fd_args_t *args = xmalloc_nz(sizeof(*args));
		*args = (receive_fd_args_t) {
			.magic = MAGIC_RECEIVE_FD,
			.type = type,
			.events = events,
			.arg = arg,
		};
		add_work(true, src, (conmgr_callback_t) {
				.func = _receive_fd,
				.func_name = XSTRINGIFY(_receive_fd),
				.arg = args,
			 }, (conmgr_work_control_t) {0}, 0, __func__);
		rc = SLURM_SUCCESS;
	}

	slurm_mutex_unlock(&mgr.mutex);

	return rc;
}

static void _send_fd(conmgr_callback_args_t conmgr_args, void *arg)
{
	send_fd_args_t *args = arg;
	conmgr_fd_t *con = conmgr_args.con;
	int fd = args->fd;

	xassert(args->magic == MAGIC_SEND_FD);
	xassert(con->magic == MAGIC_CON_MGR_FD);

	if (conmgr_args.status == CONMGR_WORK_STATUS_CANCELLED) {
		log_flag(CONMGR, "%s: [%s] Canceled sending file descriptor %d.",
			 __func__, con->name, fd);
	} else if (con->output_fd < 0) {
		log_flag(CONMGR, "%s: [%s] Unable to send file descriptor %d over invalid output_fd=%d",
			 __func__, con->name, fd, con->output_fd);
	} else {
		send_fd_over_socket(con->output_fd, fd);
		log_flag(CONMGR, "%s: [%s] Sent file descriptor %d over output_fd=%d",
				 __func__, con->name, fd, con->output_fd);
	}

	/* always close the file descriptor in this process to avoid leaking */
	fd_close(&fd);

	args->magic = ~MAGIC_SEND_FD;
	xfree(args);
}

extern int conmgr_queue_send_fd(conmgr_fd_t *con, int fd)
{
	int rc = SLURM_ERROR;

	slurm_mutex_lock(&mgr.mutex);

	xassert(con->magic == MAGIC_CON_MGR_FD);

	if (fd < 0) {
		log_flag(CONMGR, "%s: [%s] Unable to send invalid file descriptor %d",
			 __func__, con->name, fd);
		rc = EINVAL;
	} else if (!con->is_socket) {
		log_flag(CONMGR, "%s: [%s] Unable to send file descriptor %d over non-socket",
			 __func__, con->name, fd);
		rc = EAFNOSUPPORT;
	} else if (con->output_fd < 0) {
		log_flag(CONMGR, "%s: [%s] Unable to send file descriptor %d over invalid output_fd=%d",
			 __func__, con->name, fd, con->output_fd);
		rc = SLURM_COMMUNICATIONS_MISSING_SOCKET_ERROR;
	} else {
		send_fd_args_t *args = xmalloc_nz(sizeof(*args));
		*args = (send_fd_args_t) {
			.magic = MAGIC_SEND_FD,
			.fd = fd,
		};
		add_work(true, con, (conmgr_callback_t) {
				.func = _send_fd,
				.func_name = XSTRINGIFY(_send_fd),
				.arg = args,
			 }, (conmgr_work_control_t) {0}, 0, __func__);
		rc = SLURM_SUCCESS;
	}

	slurm_mutex_unlock(&mgr.mutex);

	return rc;
}

static void _deferred_close_fd(conmgr_callback_args_t conmgr_args, void *arg)
{
	conmgr_fd_t *con = conmgr_args.con;

	slurm_mutex_lock(&mgr.mutex);
	if (con->work_active) {
		slurm_mutex_unlock(&mgr.mutex);
		conmgr_queue_close_fd(con);
	} else {
		close_con(true, con);
		slurm_mutex_unlock(&mgr.mutex);
	}
}

extern void conmgr_queue_close_fd(conmgr_fd_t *con)
{
	xassert(con->magic == MAGIC_CON_MGR_FD);

	slurm_mutex_lock(&mgr.mutex);
	if (!con->work_active) {
		/*
		 * Defer request to close connection until connection is no
		 * longer actively doing work as closing connection would change
		 * several variables guarenteed to not change while work is
		 * active.
		 */
		add_work_con_fifo(true, con, _deferred_close_fd, con);
	} else {
		close_con(true, con);
	}
	slurm_mutex_unlock(&mgr.mutex);
}

static int _match_socket_address(void *x, void *key)
{
	conmgr_fd_t *con = x;
	const slurm_addr_t *addr1 = key;
	const slurm_addr_t *addr2 = &con->address;

	xassert(con->magic == MAGIC_CON_MGR_FD);

	if (addr1->ss_family != addr2->ss_family)
		return 0;

	switch (addr1->ss_family) {
		case AF_INET:
		{
			const struct sockaddr_in *a1 =
				(const struct sockaddr_in *) addr1;
			const struct sockaddr_in *a2 =
				(const struct sockaddr_in *) addr2;

			if (a1->sin_port != a2->sin_port)
				return 0;

			return !memcmp(&a1->sin_addr.s_addr,
				       &a2->sin_addr.s_addr,
				       sizeof(a2->sin_addr.s_addr));
		}
		case AF_INET6:
		{
			const struct sockaddr_in6 *a1 =
				(const struct sockaddr_in6 *) addr1;
			const struct sockaddr_in6 *a2 =
				(const struct sockaddr_in6 *) addr2;

			if (a1->sin6_port != a2->sin6_port)
				return 0;
			if (a1->sin6_scope_id != a2->sin6_scope_id)
				return 0;
			return !memcmp(&a1->sin6_addr.s6_addr,
				       &a2->sin6_addr.s6_addr,
				       sizeof(a2->sin6_addr.s6_addr));
		}
		case AF_UNIX:
		{
			const struct sockaddr_un *a1 =
				(const struct sockaddr_un *) addr1;
			const struct sockaddr_un *a2 =
				(const struct sockaddr_un *) addr2;

			return !xstrcmp(a1->sun_path, a2->sun_path);
		}
		default:
		{
			fatal_abort("Unexpected ss family type %u",
				    (uint32_t) addr1->ss_family);
		}
	}
	/* Unreachable */
	fatal_abort("This should never happen");
}

static bool _is_listening(const slurm_addr_t *addr, socklen_t addrlen)
{
	/* use address to ensure memory size is correct */
	slurm_addr_t address = {0};

	memcpy(&address, addr, addrlen);

	if (list_find_first_ro(mgr.listen_conns, _match_socket_address,
			       &address))
		return true;

	return false;
}

extern int conmgr_create_listen_socket(conmgr_con_type_t type,
					const char *listen_on,
					conmgr_events_t events, void *arg)
{
	static const char UNIX_PREFIX[] = "unix:";
	const char *unixsock = xstrstr(listen_on, UNIX_PREFIX);
	int rc = SLURM_SUCCESS;
	struct addrinfo *addrlist = NULL;
	parsed_host_port_t *parsed_hp;
	conmgr_callbacks_t callbacks;

	slurm_mutex_lock(&mgr.mutex);
	callbacks = mgr.callbacks;
	slurm_mutex_unlock(&mgr.mutex);

	/* check for name local sockets */
	if (unixsock) {
		slurm_addr_t addr = {0};
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

		if (fd < 0)
			fatal("%s: socket() failed: %m", __func__);

		unixsock += sizeof(UNIX_PREFIX) - 1;
		if (unixsock[0] == '\0')
			fatal("%s: [%s] Invalid UNIX socket",
			      __func__, listen_on);

		addr = sockaddr_from_unix_path(unixsock);

		if (addr.ss_family != AF_UNIX)
			fatal("%s: [%s] Invalid Unix socket path: %s",
			      __func__, listen_on, unixsock);

		log_flag(CONMGR, "%s: [%pA] attempting to bind() and listen() UNIX socket",
			 __func__, &addr);

		if (unlink(unixsock) && (errno != ENOENT))
			error("Error unlink(%s): %m", unixsock);

		/* bind() will EINVAL if socklen=sizeof(addr) */
		if ((rc = bind(fd, (const struct sockaddr *) &addr,
			       sizeof(struct sockaddr_un))))
			fatal("%s: [%s] Unable to bind UNIX socket: %m",
			      __func__, listen_on);

		fd_set_oob(fd, 0);

		rc = listen(fd, SLURM_DEFAULT_LISTEN_BACKLOG);
		if (rc < 0)
			fatal("%s: [%s] unable to listen(): %m",
			      __func__, listen_on);

		return conmgr_process_fd_unix_listen(type, fd, events, &addr,
						     sizeof(addr), unixsock,
						     arg);
	} else {
		/* split up host and port */
		if (!(parsed_hp = callbacks.parse(listen_on)))
			fatal("%s: Unable to parse %s", __func__, listen_on);

		/* resolve out the host and port if provided */
		if (!(addrlist = xgetaddrinfo(parsed_hp->host,
					      parsed_hp->port)))
			fatal("Unable to listen on %s", listen_on);
	}

	/*
	 * Create a socket for every address returned
	 * ipv6 clone of net_stream_listen_ports()
	 */
	for (struct addrinfo *addr = addrlist; !rc && addr != NULL;
	     addr = addr->ai_next) {
		/* clone the address since it will be freed at
		 * end of this loop
		 */
		int fd;
		int one = 1;

		if (_is_listening((const slurm_addr_t *) addr->ai_addr,
				  addr->ai_addrlen)) {
			verbose("%s: ignoring duplicate listen request for %pA",
				__func__, (const slurm_addr_t *) addr->ai_addr);
			continue;
		}

		fd = socket(addr->ai_family, addr->ai_socktype | SOCK_CLOEXEC,
			    addr->ai_protocol);
		if (fd < 0)
			fatal("%s: [%s] Unable to create socket: %m",
			      __func__, addrinfo_to_string(addr));

		/*
		 * activate socket reuse to avoid annoying timing issues
		 * with daemon restarts
		 */
		if (setsockopt(fd, addr->ai_socktype, SO_REUSEADDR,
			       &one, sizeof(one)))
			fatal("%s: [%s] setsockopt(SO_REUSEADDR) failed: %m",
			      __func__, addrinfo_to_string(addr));

		if (bind(fd, addr->ai_addr, addr->ai_addrlen) != 0)
			fatal("%s: [%s] Unable to bind socket: %m",
			      __func__, addrinfo_to_string(addr));

		fd_set_oob(fd, 0);

		rc = listen(fd, SLURM_DEFAULT_LISTEN_BACKLOG);
		if (rc < 0)
			fatal("%s: [%s] unable to listen(): %m",
			      __func__, addrinfo_to_string(addr));

		rc = conmgr_process_fd_listen(fd, type, events,
			(const slurm_addr_t *) addr->ai_addr, addr->ai_addrlen,
			arg);
	}

	freeaddrinfo(addrlist);
	callbacks.free_parse(parsed_hp);

	return rc;
}

static int _setup_listen_socket(void *x, void *arg)
{
	const char *hostport = (const char *)x;
	socket_listen_init_t *init = arg;

	init->rc = conmgr_create_listen_socket(init->type, hostport,
					       init->events, init->arg);

	return (init->rc ? SLURM_ERROR : SLURM_SUCCESS);
}

extern int conmgr_create_listen_sockets(conmgr_con_type_t type,
					list_t *hostports,
					conmgr_events_t events, void *arg)
{
	socket_listen_init_t init = {
		.events = events,
		.arg = arg,
		.type = type,
	};

	(void) list_for_each(hostports, _setup_listen_socket, &init);
	return init.rc;
}

extern int conmgr_create_connect_socket(conmgr_con_type_t type,
					slurm_addr_t *addr, socklen_t addrlen,
					conmgr_events_t events, void *arg)
{
	int fd = -1, rc = SLURM_ERROR;
	//socklen_t bindlen = 0;

	if (addr->ss_family == AF_UNIX) {
		fd = socket(addr->ss_family, (SOCK_STREAM | SOCK_CLOEXEC), 0);
		//bindlen = sizeof(struct sockaddr_un);
	} else if ((addr->ss_family == AF_INET) ||
		   (addr->ss_family == AF_INET6)) {
		fd = socket(addr->ss_family, (SOCK_STREAM | SOCK_CLOEXEC),
			    IPPROTO_TCP);
		//bindlen = addrlen;
	} else {
		return EAFNOSUPPORT;
	}

	if (fd < 0) {
		rc = errno;
		log_flag(NET, "%s: [%pA] socket() failed: %s",
			 __func__, addr, slurm_strerror(rc));
		return rc;
	}

	/* Set socket as non-blocking to avoid connect() blocking */
	fd_set_nonblocking(fd);

	log_flag(CONMGR, "%s: [%pA(fd:%d)] attempting to connect() new socket",
		 __func__, addr, fd);

again:
	if ((rc = connect(fd, (const struct sockaddr *) addr, addrlen))) {
		rc = errno;

		if (rc == EINTR) {
			bool shutdown;

			slurm_mutex_lock(&mgr.mutex);
			xassert(mgr.initialized);
			shutdown = mgr.shutdown_requested;
			slurm_mutex_unlock(&mgr.mutex);

			if (shutdown) {
				log_flag(CONMGR, "%s: [%pA(fd:%d)] connect() interrupted during shutdown. Closing connection.",
					 __func__, addr, fd);
				fd_close(&fd);
				return SLURM_SUCCESS;
			}

			log_flag(CONMGR, "%s: [%pA(fd:%d)] connect() interrupted. Retrying.",
				 __func__, addr, fd);
			goto again;
		}

		if ((rc != EINPROGRESS) && (rc != EAGAIN) &&
		    (rc != EWOULDBLOCK)) {
			log_flag(NET, "%s: [%pA(fd:%d)] connect() failed: %s",
				 __func__, addr, fd, slurm_strerror(rc));
			fd_close(&fd);
			return rc;
		}

		/* delayed connect() completion is expected */
	}

	return add_connection(type, NULL, fd, fd, events, addr, addrlen,
			      false, NULL, arg);
}

extern int conmgr_get_fd_auth_creds(conmgr_fd_t *con,
				     uid_t *cred_uid, gid_t *cred_gid,
				     pid_t *cred_pid)
{
	int fd, rc = ESLURM_NOT_SUPPORTED;

	xassert(cred_uid);
	xassert(cred_gid);
	xassert(cred_pid);

	if (!con || !cred_uid || !cred_gid || !cred_pid)
		return EINVAL;

	xassert(con->magic == MAGIC_CON_MGR_FD);

	if (((fd = con->input_fd) == -1) && ((fd = con->output_fd) == -1))
		return SLURMCTLD_COMMUNICATIONS_CONNECTION_ERROR;

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__NetBSD__)
	struct ucred cred = { 0 };
	socklen_t len = sizeof(cred);
	if (!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len)) {
		*cred_uid = cred.uid;
		*cred_gid = cred.gid;
		*cred_pid = cred.pid;
		return SLURM_SUCCESS;
	} else {
		rc = errno;
	}
#else
	struct xucred cred = { 0 };
	socklen_t len = sizeof(cred);
	if (!getsockopt(fd, 0, LOCAL_PEERCRED, &cred, &len)) {
		*cred_uid = cred.cr_uid;
		*cred_gid = cred.cr_groups[0];
		*cred_pid = cred.cr_pid;
		return SLURM_SUCCESS;
	} else {
		rc = errno;
	}
#endif

	return rc;
}

extern const char *conmgr_fd_get_name(const conmgr_fd_t *con)
{
	xassert(con->magic == MAGIC_CON_MGR_FD);
	xassert(con->name && con->name[0]);
	return con->name;
}

extern conmgr_fd_status_t conmgr_fd_get_status(conmgr_fd_t *con)
{
	conmgr_fd_status_t status = {
		.is_socket = con->is_socket,
		.unix_socket = con->unix_socket,
		.is_listen = con->is_listen,
		.read_eof = con->read_eof,
		.is_connected = con->is_connected,
	};

	xassert(con->magic == MAGIC_CON_MGR_FD);
	xassert(con->work_active);
	return status;
}

/*
 * Find by matching fd to connection
 */
static int _find_by_fd(void *x, void *key)
{
	conmgr_fd_t *con = x;
	int fd = *(int *)key;
	return (con->input_fd == fd) || (con->output_fd == fd);
}

extern conmgr_fd_t *con_find_by_fd(int fd)
{
	conmgr_fd_t *con;

	if ((con = list_find_first(mgr.connections, _find_by_fd, &fd)))
		return con;

	if ((con = list_find_first(mgr.listen_conns, _find_by_fd, &fd)))
		return con;

	/* mgr.complete_conns don't have input_fd or output_fd */

	return NULL;
}

extern void con_close_on_poll_error(conmgr_fd_t *con, int fd)
{
	if (con->is_socket) {
		/* Ask kernel for socket error */
		int rc = SLURM_ERROR, err = SLURM_ERROR;

		if ((rc = fd_get_socket_error(fd, &err)))
			error("%s: [%s] error while getting socket error: %s",
			      __func__, con->name, slurm_strerror(rc));
		else if (err)
			error("%s: [%s] socket error encountered while polling: %s",
			      __func__, con->name, slurm_strerror(err));
	}

	/*
	 * Socket must not continue to be considered valid to avoid a
	 * infinite calls to poll() which will immediately fail. Close
	 * the relavent file descriptor and remove from connection.
	 */
	close_con(true, con);
}

static int _set_fd_polling(int fd, pollctl_fd_type_t old, pollctl_fd_type_t new,
			   const char *con_name, const char *caller)
{
	if (old == PCTL_TYPE_UNSUPPORTED)
		return PCTL_TYPE_UNSUPPORTED;

	if (old == new)
		return new;

	if (new == PCTL_TYPE_NONE) {
		if (old != PCTL_TYPE_NONE)
			pollctl_unlink_fd(fd, con_name, caller);
		return new;
	}

	if (old != PCTL_TYPE_NONE) {
		pollctl_relink_fd(fd, new, con_name, caller);
		return new;
	} else {
		int rc = pollctl_link_fd(fd, new, con_name, caller);

		if (!rc)
			return new;
		else if (rc == EPERM)
			return PCTL_TYPE_UNSUPPORTED;
		else
			fatal("%s->%s: [%s] Unable to start polling: %s",
			      caller, __func__, con_name, slurm_strerror(rc));
	}
}

static void _log_set_polling(conmgr_fd_t *con, bool has_in, bool has_out,
			     pollctl_fd_type_t type, pollctl_fd_type_t in_type,
			     pollctl_fd_type_t out_type, const char *caller)
{
	char *log = NULL, *at = NULL;
	const char *op = "maintain";

	if (!(slurm_conf.debug_flags & DEBUG_FLAG_CONMGR))
		return;

	if (has_in) {
		const char *old, *new;

		old = pollctl_type_to_string(con->polling_input_fd);
		new = pollctl_type_to_string(in_type);

		xstrfmtcatat(log, &at, " in[%d]:%s", con->input_fd, old);

		if (in_type != con->polling_input_fd) {
			xstrfmtcatat(log, &at, "->%s", new);
			op = "changing";
		}
	}

	if (has_out) {
		const char *old, *new;

		old = pollctl_type_to_string(con->polling_output_fd);
		new = pollctl_type_to_string(out_type);

		xstrfmtcatat(log, &at, " out[%d]:%s", con->output_fd, old);

		if (out_type != con->polling_output_fd) {
			xstrfmtcatat(log, &at, "->%s", new);
			op = "changing";
		}
	}

	log_flag(CONMGR, "%s->%s: [%s] %s polling:%s%s",
		 caller, XSTRINGIFY(con_set_polling), con->name, op,
		 pollctl_type_to_string(type), (log ? log : ""));

	xfree(log);
}

extern void con_set_polling(conmgr_fd_t *con, pollctl_fd_type_t type,
			    const char *caller)
{
	int has_in, has_out, in, out, is_same;
	pollctl_fd_type_t in_type = PCTL_TYPE_NONE, out_type = PCTL_TYPE_NONE;

	_validate_pctl_type(type);
	_validate_pctl_type(con->polling_input_fd);
	_validate_pctl_type(con->polling_output_fd);

	in = con->input_fd;
	has_in = (in >= 0);
	out = con->output_fd;
	has_out = (out >= 0);
	is_same = (con->input_fd == con->output_fd);

	xassert(has_in || has_out);

	/*
	 * Map type to type per in/out. The in/out types are initialized to
	 * PCTL_TYPE_NONE above.
	 */
	switch (type) {
	case PCTL_TYPE_UNSUPPORTED:
		fatal_abort("should never happen");
	case PCTL_TYPE_NONE:
		break;
	case PCTL_TYPE_CONNECTED:
		if (is_same) {
			in_type = PCTL_TYPE_CONNECTED;
		} else {
			in_type = PCTL_TYPE_CONNECTED;
			out_type = PCTL_TYPE_CONNECTED;
		}
		break;
	case PCTL_TYPE_READ_ONLY:
		in_type = PCTL_TYPE_READ_ONLY;
		break;
	case PCTL_TYPE_READ_WRITE:
		if (is_same) {
			in_type = PCTL_TYPE_READ_WRITE;
		} else {
			in_type = PCTL_TYPE_READ_ONLY;
			out_type = PCTL_TYPE_WRITE_ONLY;
		}
		break;
	case PCTL_TYPE_WRITE_ONLY:
		if (is_same) {
			in_type = PCTL_TYPE_WRITE_ONLY;
		} else {
			out_type = PCTL_TYPE_WRITE_ONLY;
		}
		break;
	case PCTL_TYPE_LISTEN:
		xassert(con->is_listen);
		in_type = PCTL_TYPE_LISTEN;
		break;
	case PCTL_TYPE_INVALID:
	case PCTL_TYPE_INVALID_MAX:
		fatal_abort("should never execute");
	}

	if (con->polling_output_fd == PCTL_TYPE_UNSUPPORTED)
		out_type = PCTL_TYPE_UNSUPPORTED;
	if (con->polling_input_fd == PCTL_TYPE_UNSUPPORTED)
		in_type = PCTL_TYPE_UNSUPPORTED;

	_log_set_polling(con, has_in, has_out, type, in_type, out_type, caller);

	if (is_same) {
		/* same never link output_fd */
		xassert(con->polling_output_fd == PCTL_TYPE_NONE);

		con->polling_input_fd =
			_set_fd_polling(in, con->polling_input_fd, in_type,
					con->name, caller);
	} else {
		if (has_in)
			con->polling_input_fd =
				_set_fd_polling(in, con->polling_input_fd,
						in_type, con->name, caller);
		if (has_out)
			con->polling_output_fd =
				_set_fd_polling(out, con->polling_output_fd,
						out_type, con->name, caller);
	}
}

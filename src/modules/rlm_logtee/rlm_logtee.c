/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_logtee.c
 * @brief Add an additional log destination for any given request.
 *
 * @author Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 *
 * @copyright 2017 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/server/connection.h>

#ifdef HAVE_FCNTL_H
#  include <fcntl.h>
#endif

#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif

#ifdef HAVE_GRP_H
#  include <grp.h>
#endif

#ifdef HAVE_SYSLOG_H
#  include <syslog.h>
#  ifndef LOG_INFO
#    define LOG_INFO (0)
#  endif
#endif

#include <sys/uio.h>

typedef enum {
	LOGTEE_DST_INVALID = 0,
	LOGTEE_DST_FILE,				//!< Log to a file.
	LOGTEE_DST_UNIX,				//!< Log via Unix socket.
	LOGTEE_DST_UDP,					//!< Log via UDP.
	LOGTEE_DST_TCP,					//!< Log via TCP.
} logtee_dst_t;

static fr_table_num_sorted_t const logtee_dst_table[] = {
	{ L("file"),		LOGTEE_DST_FILE	},
	{ L("tcp"),		LOGTEE_DST_TCP	},
	{ L("udp"),		LOGTEE_DST_UDP	},
	{ L("unix"),		LOGTEE_DST_UNIX	}
};
static size_t logtee_dst_table_len = NUM_ELEMENTS(logtee_dst_table);

typedef struct {
	fr_ipaddr_t		dst_ipaddr;		//!< Network server.
	fr_ipaddr_t		src_ipaddr;		//!< Send requests from a given src_ipaddr.
	uint16_t		port;			//!< Network port.
} logtee_net_t;

/** logtee module instance
 */
typedef struct {
	char const		*name;			//!< Module instance name.

	char const		*delimiter;		//!< Line termination string (usually \n).
	size_t			delimiter_len;		//!< Length of line termination string.

	tmpl_t		*log_fmt;		//!< Source of log messages.

	logtee_dst_t		log_dst;		//!< Logging destination.
	char const		*log_dst_str;		//!< Logging destination string.

	size_t			buffer_depth;		//!< How big our circular buffer should be.

	struct {
		char const		*name;			//!< File to write to.
		uint32_t		permissions;		//!< Permissions to use when creating new files.
		char const		*group_str;		//!< Group to set on new files.
		gid_t			group;			//!< Resolved gid.
		bool			escape;			//!< Do filename escaping, yes / no.
		xlat_escape_legacy_t		escape_func;		//!< Escape function.
	} file;

	struct {
		char const		*path;		//!< Where the UNIX socket lives.
	} unix_sock;					// Lowercase unix is a macro on some systems?!

	logtee_net_t		tcp;			//!< TCP server.
	logtee_net_t		udp;			//!< UDP server.

	fr_time_delta_t		connection_timeout;	//!< How long to wait to open a socket.
	fr_time_delta_t		reconnection_delay;	//!< How long to wait to retry.
} rlm_logtee_t;

/** Per-thread instance data
 *
 * Contains buffers and connection handles specific to the thread.
 */
typedef struct {
	rlm_logtee_t const	*inst;			//!< Instance of logtee.
	fr_event_list_t		*el;			//!< This thread's event list.
	connection_t		*conn;			//!< Connection to our log destination.

	fr_fring_t		*fring;			//!< Circular buffer used to batch up messages.

	bool			pending;		//!< We have pending messages to write.

	TALLOC_CTX		*msg_pool;		//!< A 1k talloc pool to hold the log message whilst
							//!< it's being expanded.
	fr_pair_t		*msg;			//!< Temporary value pair holding the message value.
	fr_pair_t		*type;			//!< Temporary value pair holding the message type.
	fr_pair_t		*lvl;			//!< Temporary value pair holding the log lvl.
} rlm_logtee_thread_t;


static const conf_parser_t file_config[] = {
	{ FR_CONF_OFFSET_FLAGS("filename", CONF_FLAG_FILE_WRITABLE | CONF_FLAG_XLAT, rlm_logtee_t, file.name) },
	{ FR_CONF_OFFSET("permissions", rlm_logtee_t, file.permissions), .dflt = "0600" },
	{ FR_CONF_OFFSET("group", rlm_logtee_t, file.group_str) },
	{ FR_CONF_OFFSET("escape_filenames", rlm_logtee_t, file.escape), .dflt = "no" },
	CONF_PARSER_TERMINATOR
};

static const conf_parser_t unix_config[] = {
	{ FR_CONF_OFFSET_FLAGS("filename", CONF_FLAG_FILE_READABLE, rlm_logtee_t, unix_sock.path) },
	CONF_PARSER_TERMINATOR
};

static const conf_parser_t udp_config[] = {
	{ FR_CONF_OFFSET_TYPE_FLAGS("server", FR_TYPE_COMBO_IP_ADDR, 0, logtee_net_t, dst_ipaddr) },
	{ FR_CONF_OFFSET("port", logtee_net_t, port) },
	CONF_PARSER_TERMINATOR
};

static const conf_parser_t tcp_config[] = {
	{ FR_CONF_OFFSET_TYPE_FLAGS("server", FR_TYPE_COMBO_IP_ADDR, 0, logtee_net_t, dst_ipaddr) },
	{ FR_CONF_OFFSET("port", logtee_net_t, port) },

	CONF_PARSER_TERMINATOR
};

static const conf_parser_t module_config[] = {
	{ FR_CONF_OFFSET_FLAGS("destination", CONF_FLAG_REQUIRED, rlm_logtee_t, log_dst_str) },
	{ FR_CONF_OFFSET_TYPE_FLAGS("buffer_depth", FR_TYPE_SIZE, 0, rlm_logtee_t, buffer_depth), .dflt = "10000" },

	{ FR_CONF_OFFSET("delimiter", rlm_logtee_t, delimiter), .dflt = "\n" },
	{ FR_CONF_OFFSET("format", rlm_logtee_t, log_fmt), .dflt = "%n - %s", .quote = T_DOUBLE_QUOTED_STRING },

	/*
	 *	Log destinations
	 */
	{ FR_CONF_POINTER("file", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = (void const *) file_config },
	{ FR_CONF_POINTER("unix", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = (void const *) unix_config },
	{ FR_CONF_OFFSET_SUBSECTION("tcp", 0, rlm_logtee_t, tcp, tcp_config) },
	{ FR_CONF_OFFSET_SUBSECTION("udp", 0, rlm_logtee_t, udp, udp_config) },

	{ FR_CONF_OFFSET("connection_timeout", rlm_logtee_t, connection_timeout), .dflt = "1.0" },
	{ FR_CONF_OFFSET("reconnection_delay", rlm_logtee_t, reconnection_delay), .dflt = "1.0" },

	CONF_PARSER_TERMINATOR
};

static fr_dict_t const *dict_freeradius;

extern fr_dict_autoload_t rlm_logtee_dict[];
fr_dict_autoload_t rlm_logtee_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_log_level;
static fr_dict_attr_t const *attr_log_message;
static fr_dict_attr_t const *attr_log_type;

extern fr_dict_attr_autoload_t rlm_logtee_dict_attr[];
fr_dict_attr_autoload_t rlm_logtee_dict_attr[] = {
	{ .out = &attr_log_level, .name = "Log-Level", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_log_message, .name = "Log-Message", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_log_type, .name = "Log-Type", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ NULL }
};

static void logtee_fd_idle(rlm_logtee_thread_t *t);

static void logtee_fd_active(rlm_logtee_thread_t *t);

static void logtee_it(fr_log_type_t type, fr_log_lvl_t lvl, request_t *request,
		      char const *file, int line,
		      char const *fmt, va_list ap, void *uctx)
		      CC_HINT(format (printf, 6, 0)) CC_HINT(nonnull (3, 6));

static unlang_action_t mod_insert_logtee(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request) CC_HINT(nonnull);

/** Connection errored
 *
 */
static void _logtee_conn_error(UNUSED fr_event_list_t *el, int sock, UNUSED int flags, int fd_errno, void *uctx)
{
	rlm_logtee_thread_t	*t = talloc_get_type_abort(uctx, rlm_logtee_thread_t);

	ERROR("Connection failed (%i): %s", sock, fr_syserror(fd_errno));

	/*
	 *	Something bad happened... Fix it...
	 */
	connection_signal_reconnect(t->conn, CONNECTION_FAILED);
}

/** Drain any data we received
 *
 * We don't care about this data, we just don't want the kernel to
 * signal the other side that our read buffer's full.
 */
static void _logtee_conn_read(UNUSED fr_event_list_t *el, int sock, UNUSED int flags, void *uctx)
{
	rlm_logtee_thread_t	*t = talloc_get_type_abort(uctx, rlm_logtee_thread_t);
	ssize_t				slen;
	uint8_t				buffer[1024];

	slen = read(sock, buffer, sizeof(buffer));
	if (slen < 0) {
		switch (errno) {
		case EAGAIN:		/* We drained all the data */
		case EINTR:
			return;

		case ECONNRESET:	/* Connection was reset */
		case ETIMEDOUT:
		case EIO:
		case ENXIO:
			connection_signal_reconnect(t->conn, CONNECTION_FAILED);
			return;

		/*
		 *	Shouldn't be any other errors
		 *	If there are, investigate why we get them...
		 */
		default:
			fr_assert(0);
		}
	}
}

/** There's space available to write data, so do that...
 *
 */
static void _logtee_conn_writable(UNUSED fr_event_list_t *el, int sock, UNUSED int flags, void *uctx)
{
	rlm_logtee_thread_t	*t = talloc_get_type_abort(uctx, rlm_logtee_thread_t);
	char			*msg;

	/*
	 *	Fixme in general...
	 */
	while ((msg = fr_fring_next(t->fring))) {
		ssize_t slen;

		slen = write(sock, msg, talloc_array_length(msg) - 1) ;
	write_error:
		if (slen < 0) {
			switch (errno) {
			case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
			case EINTR:
			case ENOBUFS:
				return;

			case ECONNRESET:
			case EDESTADDRREQ:
			case EIO:
			case ENXIO:
			case EPIPE:
			case ENETDOWN:
				connection_signal_reconnect(t->conn, CONNECTION_FAILED);
				return;

			/*
			 *	Shouldn't be any other errors
			 *	If there are, investigate why we get them...
			 */
			default:
				fr_assert(0);
			}
		}

		slen = write(sock, t->inst->delimiter, t->inst->delimiter_len);
		if (slen < 0) goto write_error;
	}

	logtee_fd_idle(t);
}

/** Set the socket to idle
 *
 * If the other side is sending back garbage, we want to drain it so our buffer doesn't fill up.
 *
 * @param[in] t		Thread instance containing the connection.
 */
static void logtee_fd_idle(rlm_logtee_thread_t *t)
{
	int fd = *((int *)t->conn->h);

	DEBUG3("Marking socket (%i) as idle", fd);
	if (fr_event_fd_insert(t->conn, NULL, t->el, fd,
			       _logtee_conn_read,
			       NULL,
			       _logtee_conn_error,
			       t) < 0) {
		PERROR("Failed inserting FD event");
	}
}

/** Set the socket to active
 *
 * We have messages we want to send, so need to know when the socket is writable.
 *
 * @param[in] t		Thread instance containing the connection.
 */
static void logtee_fd_active(rlm_logtee_thread_t *t)
{
	int fd = *((int *)t->conn->h);

	DEBUG3("Marking socket (%i) as active - Draining requests", fd);
	if (fr_event_fd_insert(t->conn, NULL, t->el, fd,
			       _logtee_conn_read,
			       _logtee_conn_writable,
			       _logtee_conn_error,
			       t) < 0) {
		PERROR("Failed inserting FD event");
	}
}

/** Shutdown/close a file descriptor
 *
 */
static void _logtee_conn_close(UNUSED fr_event_list_t *el, void *h, UNUSED void *uctx)
{
	int	fd = *((int *)h);

	DEBUG3("Closing socket (%i)", fd);
	if (shutdown(fd, SHUT_RDWR) < 0) DEBUG3("Shutdown on socket (%i) failed: %s", fd, fr_syserror(errno));
	if (close(fd) < 0) DEBUG3("Closing socket (%i) failed: %s", fd, fr_syserror(errno));
}

/** Process notification that fd is open
 *
 */
static connection_state_t _logtee_conn_open(UNUSED fr_event_list_t *el, UNUSED void *h, void *uctx)
{
	rlm_logtee_thread_t	*t = talloc_get_type_abort(uctx, rlm_logtee_thread_t);

	DEBUG2("Socket connected");

	/*
	 *	If we have data pending, add the writable event immediately
	 */
	if (t->pending) {
		logtee_fd_active(t);
	} else {
		logtee_fd_idle(t);
	}

	return CONNECTION_STATE_CONNECTED;
}

/** Initialise a new outbound connection
 *
 * @param[out] h_out	Where to write the new file descriptor.
 * @param[in] conn	being initialised.
 * @param[in] uctx	A #rlm_logtee_thread_t.
 */
CC_NO_UBSAN(function) /* UBSAN: false positive - public vs private connection_t trips --fsanitize=function*/
static connection_state_t _logtee_conn_init(void **h_out, connection_t *conn, void *uctx)
{
	rlm_logtee_thread_t	*t = talloc_get_type_abort(uctx, rlm_logtee_thread_t);
	rlm_logtee_t const	*inst = t->inst;
	int			fd = -1;
	int			*fd_s;

	switch (inst->log_dst) {
	case LOGTEE_DST_UNIX:
		DEBUG2("Opening UNIX socket at \"%s\"", inst->unix_sock.path);
		fd = fr_socket_client_unix(inst->unix_sock.path, true);
		if (fd < 0) return CONNECTION_STATE_FAILED;
		break;

	case LOGTEE_DST_TCP:
		DEBUG2("Opening TCP connection to %pV:%u",
		       fr_box_ipaddr(inst->tcp.dst_ipaddr), inst->tcp.port);
		fd = fr_socket_client_tcp(NULL, NULL, &inst->tcp.dst_ipaddr, inst->tcp.port, true);
		if (fd < 0) return CONNECTION_STATE_FAILED;
		break;

	case LOGTEE_DST_UDP:
		DEBUG2("Opening UDP connection to %pV:%u",
		       fr_box_ipaddr(inst->udp.dst_ipaddr), inst->udp.port);
		fd = fr_socket_client_udp(NULL, NULL, NULL, &inst->udp.dst_ipaddr, inst->udp.port, true);
		if (fd < 0) return CONNECTION_STATE_FAILED;
		break;

	/*
	 *	Are not connection oriented destinations
	 */
	case LOGTEE_DST_INVALID:
	case LOGTEE_DST_FILE:
		fr_assert(0);
		return CONNECTION_STATE_FAILED;
	}

	/*
	 *	Avoid pointer/integer assignments
	 */
	MEM(fd_s = talloc(conn, int));
	*fd_s = fd;
	*h_out = fd_s;

	connection_signal_on_fd(conn, fd);

	return CONNECTION_STATE_CONNECTING;
}

/** Logging callback to write log messages to a destination
 *
 * This allows the logging destination to be customised on a per request basis.
 *
 * @note Function does not write log output immediately
 *
 * @param[in] type	What type of message this is (error, warn, info, debug).
 * @param[in] lvl	At what logging level this message should be output.
 * @param[in] request	The current request.
 * @param[in] file	src file the log message was generated in.
 * @param[in] line	number the log message was generated on.
 * @param[in] fmt	sprintf style fmt string.
 * @param[in] ap	Arguments for the fmt string.
 * @param[in] uctx	Context data for the log function.
 */
static void logtee_it(fr_log_type_t type, fr_log_lvl_t lvl, request_t *request,
		      UNUSED char const *file, UNUSED int line,
		      char const *fmt, va_list ap, void *uctx)
{
	rlm_logtee_thread_t	*t = talloc_get_type_abort(uctx, rlm_logtee_thread_t);
	rlm_logtee_t const	*inst = t->inst;
	char			*msg, *exp;
	fr_dcursor_t		cursor;
	fr_pair_t		*vp;
	log_dst_t		*dst;

	fr_assert(t->msg->vp_length == 0);	/* Should have been cleared before returning */

	/*
	 *	None of this should involve mallocs unless msg > 1k
	 */
	msg = talloc_typed_vasprintf(t->msg, fmt, ap);
	fr_value_box_bstrdup_buffer_shallow(NULL, &t->msg->data, attr_log_message, msg, true);

	t->type->vp_uint32 = (uint32_t) type;
	t->lvl->vp_uint32 = (uint32_t) lvl;

	fr_pair_dcursor_init(&cursor, &request->request_pairs);
	fr_dcursor_prepend(&cursor, t->msg);
	fr_dcursor_prepend(&cursor, t->type);
	fr_dcursor_prepend(&cursor, t->lvl);
	fr_dcursor_head(&cursor);

	/*
	 *	Now expand our fmt string to encapsulate the
	 *	message and any metadata
	 *
	 *	Fixme: Would be better to call tmpl_expand
	 *	into a variable length ring buffer.
	 */
	dst = request->log.dst;
	request->log.dst = NULL;
	if (tmpl_aexpand(t, &exp, request, inst->log_fmt, NULL, NULL) < 0) goto finish;
	request->log.dst = dst;

	fr_fring_overwrite(t->fring, exp);	/* Insert it into the buffer */

	if (!t->pending) {
		t->pending = true;
		logtee_fd_active(t);		/* Listen for when the fd is writable */
	}

finish:
	/*
	 *	Don't free, we reuse the fr_pair_ts for the next message
	 */
	vp = fr_dcursor_remove(&cursor);
	if (!fr_cond_assert(vp == t->lvl)) fr_dcursor_append(&cursor, vp);

	vp = fr_dcursor_remove(&cursor);
	if (!fr_cond_assert(vp == t->type)) fr_dcursor_append(&cursor, vp);

	vp = fr_dcursor_remove(&cursor);
	if (!fr_cond_assert(vp == t->msg)) fr_dcursor_append(&cursor, vp);

	fr_value_box_clear(&t->msg->data);		/* Clear message data */
}

/** Add our logging destination to the linked list of logging destinations (if it doesn't already exist)
 *
 * @param[in] p_result	the result of the module call:
 *			- #RLM_MODULE_NOOP	if log destination already exists.
 *			- #RLM_MODULE_OK	if we added a new destination.
 * @param[in] mctx	Module calling ctx.
 * @param[in] request	request to add our log destination to.
 */
static unlang_action_t mod_insert_logtee(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	log_dst_t	*dst, **last = NULL;

	for (dst = request->log.dst; dst; dst = dst->next) {
		if (dst->uctx == mctx->thread) {
			RETURN_UNLANG_NOOP;
		}

		last = &(dst->next);
	}

	if (!last) RETURN_UNLANG_NOOP;

	dst = talloc_zero(request, log_dst_t);
	dst->func = logtee_it;
	dst->uctx = mctx->thread;

	*last = dst;

	RETURN_UNLANG_OK;
}

/** Create thread-specific connections and buffers
 *
 * @param[in] mctx	specific data.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_thread_instantiate(module_thread_inst_ctx_t const *mctx)
{
	rlm_logtee_t		*inst = talloc_get_type_abort(mctx->mi->data, rlm_logtee_t);
	rlm_logtee_thread_t	*t = talloc_get_type_abort(mctx->thread, rlm_logtee_thread_t);

	MEM(t->fring = fr_fring_alloc(t, inst->buffer_depth, false));

	t->inst = inst;
	t->el = mctx->el;

	/*
	 *	Pre-allocate temporary attributes
	 */
	MEM(t->msg_pool = talloc_pool(t, 1024));
	MEM(t->msg = fr_pair_afrom_da(t->msg_pool, attr_log_message));
	MEM(t->type = fr_pair_afrom_da(t, attr_log_type));
	MEM(t->lvl = fr_pair_afrom_da(t, attr_log_level));

	/*
	 *	This opens the outbound connection
	 */
	t->conn = connection_alloc(t, t->el,
				      &(connection_funcs_t){
					.init = _logtee_conn_init,
				   	.open = _logtee_conn_open,
				   	.close = _logtee_conn_close
				      },
				      &(connection_conf_t){
					.connection_timeout = inst->connection_timeout,
				   	.reconnection_delay = inst->reconnection_delay
				      },
				      inst->name, t);
	if (t->conn == NULL) return -1;

	connection_signal_init(t->conn);

	return 0;
}

/*
 *	Instantiate the module.
 */
static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_logtee_t	*inst = talloc_get_type_abort(mctx->mi->data, rlm_logtee_t);
	CONF_SECTION    *conf = mctx->mi->conf;
	char		prefix[100];

	/*
	 *	Escape filenames only if asked.
	 */
	if (inst->file.escape) {
		inst->file.escape_func = rad_filename_escape;
	} else {
		inst->file.escape_func = rad_filename_make_safe;
	}

	inst->log_dst = fr_table_value_by_str(logtee_dst_table, inst->log_dst_str, LOGTEE_DST_INVALID);
	if (inst->log_dst == LOGTEE_DST_INVALID) {
		cf_log_err(conf, "Invalid log destination \"%s\"", inst->log_dst_str);
		return -1;
	}

	snprintf(prefix, sizeof(prefix), "rlm_logtee (%s)", inst->name);

	FR_SIZE_BOUND_CHECK("buffer_depth", inst->buffer_depth, >=, (size_t)1);
	FR_SIZE_BOUND_CHECK("buffer_depth", inst->buffer_depth, <=, (size_t)1000000);	/* 1 Million messages */

	/*
	 *	Setup the logging destination
	 */
	switch (inst->log_dst) {
	case LOGTEE_DST_FILE:
		cf_log_err(conf, "Teeing to files NYI");
		return -1;

	case LOGTEE_DST_UNIX:
#ifndef HAVE_SYS_UN_H
		cf_log_err(conf, "Unix sockets are not supported on this system");
		return -1;
#endif

	case LOGTEE_DST_UDP:
		break;

	case LOGTEE_DST_TCP:
		break;

	case LOGTEE_DST_INVALID:
		fr_assert(0);
		break;
	}

	inst->delimiter_len = talloc_array_length(inst->delimiter) - 1;

	return 0;
}


/*
 *	Externally visible module definition.
 */
extern module_rlm_t rlm_logtee;
module_rlm_t rlm_logtee = {
	.common = {
		.magic			= MODULE_MAGIC_INIT,
		.name			= "logtee",
		.inst_size		= sizeof(rlm_logtee_t),
		.thread_inst_size	= sizeof(rlm_logtee_thread_t),
		.config			= module_config,
		.instantiate		= mod_instantiate,
		.thread_instantiate	= mod_thread_instantiate,
	},
	.method_group = {
		.bindings = (module_method_binding_t[]){
			{ .section = SECTION_NAME(CF_IDENT_ANY, CF_IDENT_ANY), .method = mod_insert_logtee },
			MODULE_BINDING_TERMINATOR
		}
	}
};

/*
 *   This program is free software; you can redistribute it and/or modify
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
 * @file rlm_kafka.c
 * @brief Asynchronous Kafka producer module.
 *
 * Each worker thread owns its own librdkafka producer handle, so delivery
 * report callbacks always dispatch on the worker that initiated the produce.
 * The producer's main queue is wired to a self-pipe via
 * rd_kafka_queue_io_event_enable(); the pipe's read end sits in the worker
 * event loop and drains callbacks (delivery reports, broker errors) as they
 * arrive.
 *
 * Topics are declared at module configuration time:
 *
 * @code
 *     kafka {
 *         producer {
 *             server = "broker1:9092"
 *             topic {
 *                 "accounting" {
 *                     request_required_acks = -1
 *                 }
 *             }
 *         }
 *     }
 * @endcode
 *
 * and invoked by name:
 *
 * @code
 *     kafka.produce accounting {
 *         key   = &User-Name
 *         value = "%{request:json}"
 *     }
 *
 *     reply.Kafka-Offset := %kafka.produce('accounting', "%{request:json}")
 * @endcode
 *
 * Unknown topics are rejected at config-parse time rather than created on
 * the fly, so per-topic settings (acks, compression, partitioner) always
 * match the declared configuration.
 *
 * @copyright 2022,2026 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 */
RCSID("$Id$")
USES_APPLE_DEPRECATED_API

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/unlang/call_env.h>
#include <freeradius-devel/unlang/module.h>
#include <freeradius-devel/unlang/xlat.h>
#include <freeradius-devel/unlang/xlat_func.h>
#include <freeradius-devel/util/dlist.h>
#include <freeradius-devel/util/misc.h>
#include <freeradius-devel/util/rb.h>
#include <freeradius-devel/kafka/base.h>

#include <fcntl.h>
#include <unistd.h>

/** Per-module declared topic.
 *
 * Populated at instantiate time by walking `producer.topic.<name> { ... }`
 * subsections.  Each entry owns the librdkafka topic conf produced by the
 * kafka base parser; workers dup it at thread_instantiate to build their
 * own rd_kafka_topic_t handles.
 */
typedef struct {
	char const		*name;		//!< topic name as it appears in config
	fr_kafka_topic_conf_t	*topic_conf;	//!< wrapper around rd_kafka_topic_conf_t
	fr_rb_node_t		node;
} rlm_kafka_topic_decl_t;

/** Bootstrap-phase data - mutable during bootstrap, read-only afterward.
 *
 * virtual_servers_instantiate (which drives call_env parsing) runs before
 * mod_instantiate, so any state the call_env parser needs must be built in
 * bootstrap and stashed on mi->boot.  The topic declarations live here.
 */
typedef struct {
	fr_rb_tree_t		*topics;	//!< rlm_kafka_topic_decl_t, keyed by name
} rlm_kafka_boot_t;

typedef struct {
	CONF_SECTION			*producer_cs;	//!< "producer" subsection of our conf
	fr_kafka_conf_t			*kconf;		//!< parsed producer conf (shared, dup'd per thread)

	rlm_kafka_boot_t const		*boot;		//!< alias to mi->boot, captured at thread_instantiate

	fr_time_delta_t			flush_timeout;	//!< librdkafka flush timeout on thread teardown
} rlm_kafka_t;

/** Per-thread topic handle.
 *
 * One per declared topic, created at thread_instantiate.  rd_kafka_topic_t
 * is per-producer so each worker thread has its own.
 */
typedef struct {
	char const		*name;
	rd_kafka_topic_t	*rkt;
	fr_rb_node_t		node;
} rlm_kafka_topic_handle_t;

typedef struct rlm_kafka_thread_s rlm_kafka_thread_t;

/** Per produce() invocation context.
 *
 * Lives on the heap (not the request) because the delivery report callback
 * must be able to find it via librdkafka's opaque pointer even if the
 * request has been cancelled and freed.  Reused as the rctx for both
 * module-method and xlat invocations so we don't need a separate wrapper.
 */
typedef struct {
	fr_dlist_t		entry;		//!< linked into rlm_kafka_thread_t::inflight
	request_t		*request;	//!< NULL once cancelled; dr_msg_cb checks before resuming
	rlm_kafka_thread_t	*t;		//!< for inflight list removal from dr_msg_cb
	rd_kafka_resp_err_t	err;		//!< stashed by dr_msg_cb for resume
	int32_t			partition;
	int64_t			offset;
} kafka_produce_ctx_t;

struct rlm_kafka_thread_s {
	rlm_kafka_t const	*inst;
	fr_event_list_t		*el;

	rd_kafka_t		*rk;		//!< producer handle
	rd_kafka_queue_t	*main_q;	//!< producer main queue (delivery + error events)

	int			wake_pipe[2];	//!< self-pipe, [0]=read, [1]=write
	fr_event_fd_t		*ev_fd;		//!< event for wake_pipe[0]

	fr_rb_tree_t		*topics;	//!< rlm_kafka_topic_handle_t per declared topic

	fr_dlist_head_t		inflight;	//!< outstanding kafka_produce_ctx_t
};

/** Call env for `kafka.produce { topic = <name> ... }`.
 *
 * Topic is a literal pair at parse time; we resolve it to the declared
 * topic entry via a custom parser so unknown topics fail at parse time
 * rather than waiting for a runtime kafka lookup.
 */
typedef struct {
	rlm_kafka_topic_decl_t const	*topic;	//!< resolved from 'topic = <name>' at parse time
	fr_value_box_t			*key;	//!< optional message key
	fr_value_box_t			*value;	//!< message payload
} rlm_kafka_produce_env_t;

/* ---- topic trees ---- */

/** @param[in] a  rlm_kafka_topic_decl_t or rlm_kafka_topic_handle_t.
 *  @param[in] b  same.
 *  @return @c strcmp ordering of `a->name` and `b->name`. */
static int8_t topic_name_cmp(void const *a, void const *b)
{
	char const *const	*na = a;	/* both types have `name` as first field */
	char const *const	*nb = b;
	return CMP(strcmp(*na, *nb), 0);
}

/** Destructor for declared topic entries.  Frees the cached topic conf. */
static int _topic_decl_free(rlm_kafka_topic_decl_t *decl)
{
	/*
	 *	topic_conf is stored as cf_data on the declared topic's
	 *	CONF_SECTION and has its own destructor; we just drop the
	 *	reference.
	 */
	decl->topic_conf = NULL;
	return 0;
}

/** Destructor for per-thread topic handles.  Releases the rd_kafka_topic_t. */
static int _topic_handle_free(rlm_kafka_topic_handle_t *h)
{
	if (h->rkt) rd_kafka_topic_destroy(h->rkt);
	return 0;
}

/** Look up a per-thread topic handle by name.
 *
 * @param[in] t    thread instance.
 * @param[in] name topic name (must have been declared at config time).
 * @return @c rd_kafka_topic_t if found, @c NULL otherwise.
 */
static rd_kafka_topic_t *kafka_topic_handle(rlm_kafka_thread_t *t, char const *name)
{
	rlm_kafka_topic_handle_t	key = { .name = name };
	rlm_kafka_topic_handle_t	*h;

	h = fr_rb_find(t->topics, &key);
	return h ? h->rkt : NULL;
}

/* ---- librdkafka callbacks ---- */

/** Broker-level error callback (connection failures etc).
 *
 * Not delivery reports - those come via dr_msg_cb.
 *
 * @param[in] rk     UNUSED.
 * @param[in] err    librdkafka error code.
 * @param[in] reason human-readable description.
 * @param[in] opaque thread instance pointer we passed to rd_kafka_conf_set_opaque().
 */
static void kafka_error_cb(UNUSED rd_kafka_t *rk, int err, char const *reason, void *opaque)
{
	rlm_kafka_thread_t	*t = opaque;

	INFO("rlm_kafka (%s): %s: %s", t->inst ? "producer" : "?",
	     rd_kafka_err2name(err), reason ? reason : "(no reason)");
}

/** Delivery report callback, runs on the worker thread that owns the producer.
 *
 * If the request is still alive, stash the result and mark it runnable so the
 * resume function can translate the result to an rcode.  If cancelled
 * (ctx->request == @c NULL) just free the ctx silently.  A @c NULL opaque
 * means the message was produced without a context (e.g. a future
 * fire-and-forget path) - nothing to resume, just return.
 *
 * @param[in] rk        UNUSED.
 * @param[in] rkmessage librdkafka delivery report.
 * @param[in] opaque    UNUSED (we stash thread context on the message opaque).
 */
static void kafka_dr_msg_cb(UNUSED rd_kafka_t *rk, rd_kafka_message_t const *rkmessage, UNUSED void *opaque)
{
	kafka_produce_ctx_t	*ctx = rkmessage->_private;

	if (!ctx) return;

	fr_dlist_remove(&ctx->t->inflight, ctx);

	if (!ctx->request) {
		talloc_free(ctx);
		return;
	}

	ctx->err = rkmessage->err;
	ctx->partition = rkmessage->partition;
	ctx->offset = rkmessage->offset;

	unlang_interpret_mark_runnable(ctx->request);
}

/* ---- self-pipe readiness ---- */

/** Drain every byte currently pending on the self-pipe read end.
 *
 * librdkafka suppresses subsequent writes until the queue has been served,
 * so there's rarely more than one byte to drain, but the non-blocking read
 * loop handles any accumulated signals.
 */
static void kafka_drain_pipe(int fd)
{
	uint8_t	buf[256];

	while (read(fd, buf, sizeof(buf)) > 0) {
		/* drain */
	}
}

/** Pipe became readable because librdkafka wrote a wake byte.
 *
 * Drain the pipe, then drain the producer's main queue by polling with a zero
 * timeout.  @c rd_kafka_poll() dispatches delivery reports (kafka_dr_msg_cb)
 * and broker errors (kafka_error_cb) inline on this thread.
 *
 * @param[in] el    UNUSED.
 * @param[in] fd    self-pipe read end.
 * @param[in] flags UNUSED.
 * @param[in] uctx  @c rlm_kafka_thread_t pointer.
 */
static void kafka_fd_readable(UNUSED fr_event_list_t *el, int fd, UNUSED int flags, void *uctx)
{
	rlm_kafka_thread_t	*t = uctx;

	kafka_drain_pipe(fd);
	while (rd_kafka_poll(t->rk, 0) > 0) {
		/* drain all queued events */
	}
}

/** Self-pipe error - fatal, since we have no fallback polling path.
 *
 * Cancel every in-flight produce so yielded requests don't deadlock.
 * Subsequent produces will fail synchronously when the producer can't be
 * polled; nothing graceful we can do at this level.
 *
 * @param[in] el       event list.
 * @param[in] fd       pipe read end.
 * @param[in] flags    UNUSED.
 * @param[in] fd_errno errno as reported by the event loop.
 * @param[in] uctx     @c rlm_kafka_thread_t pointer.
 */
static void kafka_fd_error(fr_event_list_t *el, int fd, UNUSED int flags, int fd_errno, void *uctx)
{
	rlm_kafka_thread_t	*t = uctx;
	kafka_produce_ctx_t	*ctx;

	ERROR("rlm_kafka: self-pipe error (fd=%d): %s - cancelling in-flight requests",
	      fd, fr_syserror(fd_errno));

	(void) fr_event_fd_delete(el, fd, FR_EVENT_FILTER_IO);
	t->ev_fd = NULL;

	for (ctx = fr_dlist_head(&t->inflight); ctx; ctx = fr_dlist_next(&t->inflight, ctx)) {
		if (!ctx->request) continue;
		ctx->err = RD_KAFKA_RESP_ERR__FATAL;
		unlang_interpret_mark_runnable(ctx->request);
	}
}

/* ---- produce + resume ---- */

/** Translate a librdkafka delivery-report error into a module rcode.
 *
 * @param[in] request  associated request (for logging).
 * @param[in] ctx      resume context with the stashed error.
 * @return an @c rlm_rcode_t summarising the outcome.
 */
static rlm_rcode_t kafka_err_to_rcode(request_t *request, kafka_produce_ctx_t const *ctx)
{
	switch (ctx->err) {
	case RD_KAFKA_RESP_ERR_NO_ERROR:
		RDEBUG2("Delivered to partition %" PRId32 " offset %" PRId64, ctx->partition, ctx->offset);
		return RLM_MODULE_OK;

	case RD_KAFKA_RESP_ERR__MSG_TIMED_OUT:
	case RD_KAFKA_RESP_ERR__TIMED_OUT:
	case RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE:
		REDEBUG("Kafka delivery timed out: %s", rd_kafka_err2str(ctx->err));
		return RLM_MODULE_TIMEOUT;

	case RD_KAFKA_RESP_ERR_MSG_SIZE_TOO_LARGE:
	case RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART:
	case RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED:
	case RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC:
		REDEBUG("Kafka rejected message: %s", rd_kafka_err2str(ctx->err));
		return RLM_MODULE_REJECT;

	default:
		REDEBUG("Kafka delivery failed: %s (%s)",
			rd_kafka_err2name(ctx->err), rd_kafka_err2str(ctx->err));
		return RLM_MODULE_FAIL;
	}
}

/** Resume a yielded module method after its delivery report has arrived. */
static unlang_action_t kafka_produce_resume(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	kafka_produce_ctx_t	*ctx = talloc_get_type_abort(mctx->rctx, kafka_produce_ctx_t);
	rlm_rcode_t		rcode = kafka_err_to_rcode(request, ctx);

	talloc_free(ctx);
	RETURN_UNLANG_RCODE(rcode);
}

/** Module-method cancellation.
 *
 * Do NOT free the ctx - librdkafka still owns the in-flight message and will
 * fire dr_msg_cb later with our opaque pointer.  Instead detach the request
 * so dr_msg_cb discards silently on arrival; dr_msg_cb frees the ctx.
 *
 * Safe against dr_msg_cb racing because both run on the same worker thread
 * (per-worker producer).
 *
 * @param[in] mctx    module ctx with ctx as rctx.
 * @param[in] request associated request.
 * @param[in] action  UNUSED (we mask off everything except CANCEL).
 */
static void kafka_produce_signal(module_ctx_t const *mctx, request_t *request, UNUSED fr_signal_t action)
{
	kafka_produce_ctx_t	*ctx = talloc_get_type_abort(mctx->rctx, kafka_produce_ctx_t);

	RDEBUG2("Cancellation signal received - detaching delivery report");
	ctx->request = NULL;
}

/** Common produce-and-yield helper.
 *
 * Submits a message to librdkafka and returns a yielded module action if the
 * enqueue succeeded.  The caller's resume/signal callbacks receive the ctx as
 * their rctx; on synchronous failure the ctx is freed and @c NULL is returned.
 *
 * @param[in] t         thread instance.
 * @param[in] request   request to yield on.
 * @param[in] rkt       preconfigured per-thread topic handle.
 * @param[in] key       optional message key, may be @c NULL.
 * @param[in] key_len   length of @p key, 0 if @p key is @c NULL.
 * @param[in] value     message payload.
 * @param[in] value_len length of @p value.
 * @return the kafka_produce_ctx_t tracking the request, or @c NULL on failure.
 */
static kafka_produce_ctx_t *kafka_produce_enqueue(rlm_kafka_thread_t *t, request_t *request,
						  rd_kafka_topic_t *rkt,
						  uint8_t const *key, size_t key_len,
						  uint8_t const *value, size_t value_len)
{
	kafka_produce_ctx_t	*ctx;

	MEM(ctx = talloc_zero(t, kafka_produce_ctx_t));
	ctx->t = t;
	ctx->request = request;
	ctx->err = RD_KAFKA_RESP_ERR_NO_ERROR;
	ctx->partition = RD_KAFKA_PARTITION_UA;
	ctx->offset = -1;

	fr_dlist_insert_tail(&t->inflight, ctx);

	if (rd_kafka_produce(rkt, RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY,
			     /* librdkafka copies under MSG_F_COPY */
			     (void *)(uintptr_t) value, value_len,
			     key, key_len,
			     ctx) != 0) {
		rd_kafka_resp_err_t	err = rd_kafka_last_error();

		fr_dlist_remove(&t->inflight, ctx);
		talloc_free(ctx);

		REDEBUG("rd_kafka_produce failed: %s", rd_kafka_err2str(err));
		return NULL;
	}

	return ctx;
}

/** Module method: `kafka.produce <topic> { value = ... key = ... }`. */
static unlang_action_t CC_HINT(nonnull) mod_produce(UNUSED unlang_result_t *p_result,
						    module_ctx_t const *mctx, request_t *request)
{
	rlm_kafka_thread_t		*t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	rlm_kafka_produce_env_t		*env = talloc_get_type_abort(mctx->env_data, rlm_kafka_produce_env_t);
	rd_kafka_topic_t		*rkt;
	kafka_produce_ctx_t		*ctx;

	uint8_t const			*key = NULL;
	size_t				key_len = 0;

	rkt = kafka_topic_handle(t, env->topic->name);
	if (!rkt) {
		/*
		 *	Can't happen if parsing succeeded, but defensive.
		 */
		REDEBUG("Kafka topic '%s' has no handle on this worker", env->topic->name);
		RETURN_UNLANG_FAIL;
	}

	if (env->key) {
		key = (uint8_t const *) env->key->vb_strvalue;
		key_len = env->key->vb_length;
	}

	ctx = kafka_produce_enqueue(t, request, rkt,
				    key, key_len,
				    (uint8_t const *) env->value->vb_strvalue, env->value->vb_length);
	if (!ctx) RETURN_UNLANG_FAIL;

	return unlang_module_yield(request, kafka_produce_resume, kafka_produce_signal,
				   ~FR_SIGNAL_CANCEL, ctx);
}

/* ---- xlat: %kafka.produce(topic, value) ---- */

static xlat_arg_parser_t const kafka_xlat_produce_args[] = {
	{ .required = true, .concat = true, .type = FR_TYPE_STRING },	/* topic */
	{ .required = true, .concat = true, .type = FR_TYPE_STRING },	/* value */
	XLAT_ARG_PARSER_TERMINATOR
};

/** Xlat resume: translate delivery report into a "partition:offset" string.
 *
 * @param[in] xctx_ctx talloc context for the returned value box.
 * @param[in] out      cursor to append the result to.
 * @param[in] xctx     xlat ctx, rctx points at the kafka_produce_ctx_t.
 * @param[in] request  associated request (for logging).
 * @param[in] in       UNUSED (original args).
 */
static xlat_action_t kafka_xlat_produce_resume(TALLOC_CTX *xctx_ctx, fr_dcursor_t *out,
					       xlat_ctx_t const *xctx,
					       request_t *request, UNUSED fr_value_box_list_t *in)
{
	kafka_produce_ctx_t	*ctx = talloc_get_type_abort(xctx->rctx, kafka_produce_ctx_t);
	xlat_action_t		xa = XLAT_ACTION_DONE;
	fr_value_box_t		*vb;

	if (ctx->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
		REDEBUG("Kafka produce failed: %s", rd_kafka_err2str(ctx->err));
		xa = XLAT_ACTION_FAIL;
		goto finish;
	}

	MEM(vb = fr_value_box_alloc(xctx_ctx, FR_TYPE_STRING, NULL));
	fr_value_box_asprintf(xctx_ctx, vb, NULL, false, "%" PRId32 ":%" PRId64, ctx->partition, ctx->offset);
	fr_dcursor_append(out, vb);

finish:
	talloc_free(ctx);
	return xa;
}

/** Xlat cancellation.  Same semantics as the module method signal: detach
 *  the request so dr_msg_cb discards silently. */
static void kafka_xlat_produce_signal(xlat_ctx_t const *xctx, UNUSED request_t *request, UNUSED fr_signal_t action)
{
	kafka_produce_ctx_t	*ctx = talloc_get_type_abort(xctx->rctx, kafka_produce_ctx_t);
	ctx->request = NULL;
}

/** %kafka.produce(topic, value) - runtime-named produce.
 *
 * The topic must have been declared in the module config; unknown topics
 * fail the xlat.  This matches the method form's strictness: librdkafka
 * per-topic settings only apply to declared topics.
 */
static xlat_action_t kafka_xlat_produce(UNUSED TALLOC_CTX *xctx_ctx, UNUSED fr_dcursor_t *out,
					xlat_ctx_t const *xctx,
					request_t *request, fr_value_box_list_t *in)
{
	rlm_kafka_thread_t	*t = talloc_get_type_abort(xctx->mctx->thread, rlm_kafka_thread_t);
	fr_value_box_t		*topic_vb = fr_value_box_list_head(in);
	fr_value_box_t		*value_vb = fr_value_box_list_next(in, topic_vb);
	rd_kafka_topic_t	*rkt;
	kafka_produce_ctx_t	*ctx;

	if (!topic_vb || !value_vb) {
		REDEBUG("kafka.produce xlat requires (topic, value)");
		return XLAT_ACTION_FAIL;
	}

	rkt = kafka_topic_handle(t, topic_vb->vb_strvalue);
	if (!rkt) {
		REDEBUG("Kafka topic '%s' is not declared in the module config", topic_vb->vb_strvalue);
		return XLAT_ACTION_FAIL;
	}

	ctx = kafka_produce_enqueue(t, request, rkt,
				    NULL, 0,
				    (uint8_t const *) value_vb->vb_strvalue, value_vb->vb_length);
	if (!ctx) return XLAT_ACTION_FAIL;

	return unlang_xlat_yield(request, kafka_xlat_produce_resume, kafka_xlat_produce_signal,
				 ~FR_SIGNAL_CANCEL, ctx);
}

/* ---- call_env parser: resolve topic by section name2 ---- */

/** Custom call_env pair parser for the `topic = <name>` entry.
 *
 * Runs at module-method instantiate time.  Takes a literal topic name from
 * the method body and looks it up in the module's declared topic tree,
 * stashing a direct pointer to the @c rlm_kafka_topic_decl_t entry on the
 * env.  Unknown topics fail parsing rather than waiting for a runtime kafka
 * lookup.
 *
 * @param[in] ctx      UNUSED.
 * @param[out] out     pointer to `rlm_kafka_produce_env_t::topic`.
 * @param[in] t_rules  UNUSED.
 * @param[in] ci       the `topic = ...` config pair.
 * @param[in] cec      call_env ctx with module instance info.
 * @param[in] rule     UNUSED.
 * @return 0 on success, -1 if the topic is not declared.
 */
static int kafka_topic_env_parse(UNUSED TALLOC_CTX *ctx, void *out, UNUSED tmpl_rules_t const *t_rules,
				 CONF_ITEM *ci, call_env_ctx_t const *cec,
				 UNUSED call_env_parser_t const *rule)
{
	rlm_kafka_boot_t const		*boot = talloc_get_type_abort_const(cec->mi->boot, rlm_kafka_boot_t);
	rlm_kafka_topic_decl_t		*decl;
	rlm_kafka_topic_decl_t		key = { 0 };
	CONF_PAIR			*cp = cf_item_to_pair(ci);
	char const			*topic_name = cf_pair_value(cp);

	if (!topic_name || !*topic_name) {
		cf_log_err(ci, "kafka topic name must be non-empty");
		return -1;
	}

	key.name = topic_name;
	decl = fr_rb_find(boot->topics, &key);
	if (!decl) {
		cf_log_err(ci, "Kafka topic '%s' is not declared in the '%s' module config",
			   topic_name, cec->mi->name);
		return -1;
	}

	*((rlm_kafka_topic_decl_t const **) out) = decl;
	return 0;
}

static const call_env_method_t rlm_kafka_produce_env = {
	FR_CALL_ENV_METHOD_OUT(rlm_kafka_produce_env_t),
	.env = (call_env_parser_t[]) {
		{ FR_CALL_ENV_PARSE_ONLY_OFFSET("topic", FR_TYPE_VOID, CALL_ENV_FLAG_REQUIRED,
						rlm_kafka_produce_env_t, topic),
		  .pair.func = kafka_topic_env_parse },
		{ FR_CALL_ENV_OFFSET("key",   FR_TYPE_STRING, CALL_ENV_FLAG_CONCAT | CALL_ENV_FLAG_NULLABLE,
				     rlm_kafka_produce_env_t, key) },
		{ FR_CALL_ENV_OFFSET("value", FR_TYPE_STRING, CALL_ENV_FLAG_REQUIRED | CALL_ENV_FLAG_CONCAT,
				     rlm_kafka_produce_env_t, value) },
		CALL_ENV_TERMINATOR
	}
};

static conf_parser_t const module_config[] = {
	{ FR_CONF_POINTER("producer", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = (void const *) kafka_base_producer_config },

	{ FR_CONF_OFFSET("flush_timeout", rlm_kafka_t, flush_timeout), .dflt = "5s" },

	CONF_PARSER_TERMINATOR
};

/* ---- module lifecycle ---- */

/** Walk `producer.topic.<name> { ... }` subsections and index them by name.
 *
 * The kafka base library parses each topic's per-topic conf into a
 * @c fr_kafka_topic_conf_t cached on the section; we just capture pointers
 * to them.
 *
 * @param[in] boot      module bootstrap data (mutable during bootstrap).
 * @param[in] producer  the already-parsed `producer {}` CONF_SECTION.
 * @return 0 on success, -1 on allocation failure.
 */
static int kafka_topic_decls_build(rlm_kafka_boot_t *boot, CONF_SECTION *producer)
{
	CONF_SECTION	*topic_list;
	CONF_SECTION	*topic_cs = NULL;

	boot->topics = fr_rb_inline_talloc_alloc(boot, rlm_kafka_topic_decl_t, node, topic_name_cmp, NULL);
	if (!boot->topics) {
		cf_log_err(producer, "Failed to allocate kafka topic tree");
		return -1;
	}

	topic_list = cf_section_find(producer, "topic", NULL);
	if (!topic_list) return 0;		/* no declared topics - produces will fail validation */

	while ((topic_cs = cf_section_find_next(topic_list, topic_cs, CF_IDENT_ANY, NULL))) {
		rlm_kafka_topic_decl_t	*decl;
		char const		*name = cf_section_name1(topic_cs);

		MEM(decl = talloc_zero(boot->topics, rlm_kafka_topic_decl_t));
		decl->name = talloc_strdup(decl, name);
		decl->topic_conf = kafka_topic_conf_from_cs(topic_cs);
		talloc_set_destructor(decl, _topic_decl_free);

		if (!fr_rb_insert(boot->topics, decl)) {
			cf_log_err(topic_cs, "Duplicate kafka topic '%s'", name);
			return -1;
		}
	}

	return 0;
}

static int mod_bootstrap(module_inst_ctx_t const *mctx)
{
	rlm_kafka_boot_t	*boot = talloc_get_type_abort(mctx->mi->boot, rlm_kafka_boot_t);
	CONF_SECTION		*producer_cs;
	xlat_t			*xlat;

	/*
	 *	Declared topics are read at virtual-server compile time
	 *	(which runs before mod_instantiate), so build the topic tree
	 *	here in bootstrap.  mi->boot is writable during this phase
	 *	and then frozen, so the call_env parser sees a stable tree.
	 */
	producer_cs = cf_section_find(mctx->mi->conf, "producer", NULL);
	if (!producer_cs) {
		cf_log_err(mctx->mi->conf, "Missing 'producer' subsection");
		return -1;
	}

	if (kafka_topic_decls_build(boot, producer_cs) < 0) return -1;

	xlat = module_rlm_xlat_register(mctx->mi->boot, mctx, "produce", kafka_xlat_produce, FR_TYPE_STRING);
	if (!xlat) return -1;
	xlat_func_args_set(xlat, kafka_xlat_produce_args);

	return 0;
}

static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_kafka_t		*inst = talloc_get_type_abort(mctx->mi->data, rlm_kafka_t);
	rlm_kafka_boot_t const	*boot = talloc_get_type_abort_const(mctx->mi->boot, rlm_kafka_boot_t);

	inst->boot = boot;

	inst->producer_cs = cf_section_find(mctx->mi->conf, "producer", NULL);
	if (!inst->producer_cs) {
		cf_log_err(mctx->mi->conf, "Missing 'producer' subsection");
		return -1;
	}
	inst->kconf = kafka_conf_from_cs(inst->producer_cs);
	if (!inst->kconf) {
		cf_log_err(mctx->mi->conf, "Failed to build kafka producer configuration");
		return -1;
	}

	return 0;
}

/** Create a per-thread rd_kafka_topic_t for every declared topic.
 *
 * Called at thread_instantiate.  Each entry uses a dup of the instance-wide
 * fr_kafka_topic_conf_t so librdkafka can own its copy.
 */
static int kafka_topic_handles_build(rlm_kafka_thread_t *t)
{
	rlm_kafka_topic_decl_t	*decl;
	fr_rb_iter_inorder_t	iter;

	t->topics = fr_rb_inline_talloc_alloc(t, rlm_kafka_topic_handle_t, node, topic_name_cmp, NULL);
	if (!t->topics) {
		ERROR("rlm_kafka: failed to allocate topic handle tree");
		return -1;
	}

	for (decl = fr_rb_iter_init_inorder(t->inst->boot->topics, &iter);
	     decl != NULL;
	     decl = fr_rb_iter_next_inorder(t->inst->boot->topics, &iter)) {
		rlm_kafka_topic_handle_t	*h;
		rd_kafka_topic_conf_t		*tc;

		tc = rd_kafka_topic_conf_dup(decl->topic_conf->conf);
		MEM(h = talloc_zero(t->topics, rlm_kafka_topic_handle_t));
		h->name = talloc_strdup(h, decl->name);
		h->rkt = rd_kafka_topic_new(t->rk, h->name, tc);
		if (!h->rkt) {
			/* librdkafka consumes tc only on success */
			rd_kafka_topic_conf_destroy(tc);
			ERROR("rlm_kafka: rd_kafka_topic_new('%s') failed: %s",
			      h->name, rd_kafka_err2str(rd_kafka_last_error()));
			talloc_free(h);
			return -1;
		}
		talloc_set_destructor(h, _topic_handle_free);

		if (!fr_rb_insert(t->topics, h)) {
			talloc_free(h);
			ERROR("rlm_kafka: duplicate topic handle '%s'", h->name);
			return -1;
		}
	}

	return 0;
}

static int mod_thread_instantiate(module_thread_inst_ctx_t const *mctx)
{
	rlm_kafka_t const	*inst = talloc_get_type_abort_const(mctx->mi->data, rlm_kafka_t);
	rlm_kafka_thread_t	*t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	rd_kafka_conf_t		*conf;
	char			errstr[512];

	t->inst = inst;
	t->el = mctx->el;
	t->wake_pipe[0] = t->wake_pipe[1] = -1;

	fr_dlist_talloc_init(&t->inflight, kafka_produce_ctx_t, entry);

	/*
	 *	rd_kafka_new consumes the conf, so dup it.
	 */
	conf = rd_kafka_conf_dup(inst->kconf->conf);
	rd_kafka_conf_set_dr_msg_cb(conf, kafka_dr_msg_cb);
	rd_kafka_conf_set_error_cb(conf, kafka_error_cb);
	rd_kafka_conf_set_opaque(conf, t);

	t->rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
	if (!t->rk) {
		rd_kafka_conf_destroy(conf);			/* only consumed on success */
		ERROR("rlm_kafka: rd_kafka_new failed: %s", errstr);
		return -1;
	}

	t->main_q = rd_kafka_queue_get_main(t->rk);
	if (!t->main_q) {
		ERROR("rlm_kafka: rd_kafka_queue_get_main returned NULL");
		goto error;
	}

	if (kafka_topic_handles_build(t) < 0) goto error;

	/*
	 *	Wire up the self-pipe.  librdkafka writes a single byte to the
	 *	write end whenever the main queue transitions from empty to
	 *	non-empty, waking our event loop so kafka_fd_readable() can
	 *	poll the queue.
	 */
	if (pipe(t->wake_pipe) < 0) {
		ERROR("rlm_kafka: pipe() failed: %s", fr_syserror(errno));
		goto error;
	}
	(void) fr_nonblock(t->wake_pipe[0]);
	(void) fr_nonblock(t->wake_pipe[1]);

	rd_kafka_queue_io_event_enable(t->main_q, t->wake_pipe[1], "x", 1);

	if (fr_event_fd_insert(t, &t->ev_fd, t->el, t->wake_pipe[0],
			       kafka_fd_readable, NULL, kafka_fd_error, t) < 0) {
		PERROR("rlm_kafka: fr_event_fd_insert failed");
		rd_kafka_queue_io_event_enable(t->main_q, -1, NULL, 0);
		goto error;
	}

	return 0;

error:
	if (t->topics) TALLOC_FREE(t->topics);
	if (t->main_q) {
		rd_kafka_queue_destroy(t->main_q);
		t->main_q = NULL;
	}
	if (t->rk) {
		rd_kafka_destroy(t->rk);
		t->rk = NULL;
	}
	if (t->wake_pipe[0] >= 0) close(t->wake_pipe[0]);
	if (t->wake_pipe[1] >= 0) close(t->wake_pipe[1]);
	return -1;
}

static int mod_thread_detach(module_thread_inst_ctx_t const *mctx)
{
	rlm_kafka_thread_t	*t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	kafka_produce_ctx_t	*ctx;

	/*
	 *	Detach all in-flight requests.  After this, dr_msg_cb will
	 *	discard delivery reports rather than try to resume a dying
	 *	interpreter.
	 */
	for (ctx = fr_dlist_head(&t->inflight); ctx; ctx = fr_dlist_next(&t->inflight, ctx)) {
		ctx->request = NULL;
	}

	/*
	 *	Stop librdkafka from writing to our pipe before we close it.
	 *	Passing fd=-1 disables the io event.
	 */
	if (t->rk && t->main_q) (void) rd_kafka_queue_io_event_enable(t->main_q, -1, NULL, 0);

	if (t->ev_fd) {
		(void) fr_event_fd_delete(t->el, t->wake_pipe[0], FR_EVENT_FILTER_IO);
		t->ev_fd = NULL;
	}

	if (t->rk) {
		rd_kafka_resp_err_t	ferr;

		ferr = rd_kafka_flush(t->rk, fr_time_delta_to_msec(t->inst->flush_timeout));
		if (ferr != RD_KAFKA_RESP_ERR_NO_ERROR) {
			WARN("rlm_kafka: flush timed out - %d messages remain in queue",
			     rd_kafka_outq_len(t->rk));
		}

		while (rd_kafka_poll(t->rk, 0) > 0) {
			/* drain any dr_cbs queued by flush */
		}
	}

	if (t->topics) TALLOC_FREE(t->topics);
	if (t->main_q) {
		rd_kafka_queue_destroy(t->main_q);
		t->main_q = NULL;
	}
	if (t->rk) {
		rd_kafka_destroy(t->rk);
		t->rk = NULL;
	}

	if (t->wake_pipe[0] >= 0) {
		close(t->wake_pipe[0]);
		t->wake_pipe[0] = -1;
	}
	if (t->wake_pipe[1] >= 0) {
		close(t->wake_pipe[1]);
		t->wake_pipe[1] = -1;
	}

	return 0;
}

extern module_rlm_t rlm_kafka;
module_rlm_t rlm_kafka = {
	.common = {
		.magic			= MODULE_MAGIC_INIT,
		.name			= "kafka",
		.inst_size		= sizeof(rlm_kafka_t),
		.boot_size		= sizeof(rlm_kafka_boot_t),
		.boot_type		= "rlm_kafka_boot_t",
		.thread_inst_size	= sizeof(rlm_kafka_thread_t),
		.config			= module_config,
		.bootstrap		= mod_bootstrap,
		.instantiate		= mod_instantiate,
		.thread_instantiate	= mod_thread_instantiate,
		.thread_detach		= mod_thread_detach
	},
	.method_group = {
		.bindings = (module_method_binding_t[]){
			{
				.section = SECTION_NAME("produce", CF_IDENT_ANY),
				.method = mod_produce,
				.method_env = &rlm_kafka_produce_env
			},
			MODULE_BINDING_TERMINATOR
		}
	}
};

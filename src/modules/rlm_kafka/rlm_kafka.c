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
 * `rd_kafka_queue_io_event_enable()`; the pipe's read end sits in the
 * worker event loop and drains callbacks (delivery reports, broker errors)
 * as they arrive.
 *
 * The schema for the module config is just @ref kafka_base_producer_config
 * from the kafka base library (librdkafka passthrough plus
 * `flush_timeout`), with `fr_kafka_conf_t` embedded as the first member
 * of @ref rlm_kafka_t so `FR_CONF_OFFSET` entries resolve correctly.
 * Topics are declared once in the module config and referenced by name at
 * method/xlat invocation time; unknown topics are rejected at config-parse
 * time rather than being created on the fly, so per-topic settings (acks,
 * compression, partitioner) always match the declared configuration.
 *
 * See @ref mod_produce and @ref kafka_xlat_produce for the caller-facing
 * surfaces.
 *
 * @copyright 2022,2026 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 */
RCSID("$Id$")

USES_APPLE_DEPRECATED_API

#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/dlist.h>
#include <freeradius-devel/util/misc.h>
#include <freeradius-devel/util/rb.h>
#include <freeradius-devel/util/types.h>
#include <freeradius-devel/util/value.h>

#include <freeradius-devel/kafka/base.h>

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module_rlm.h>

#include <freeradius-devel/unlang/call_env.h>
#include <freeradius-devel/unlang/module.h>
#include <freeradius-devel/unlang/xlat.h>
#include <freeradius-devel/unlang/xlat_ctx.h>
#include <freeradius-devel/unlang/xlat_func.h>
#include <freeradius-devel/unlang/xlat_priv.h>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

/** Module instance data
 *
 * `fr_kafka_conf_t` is embedded as the first member so
 * `KAFKA_BASE_CONFIG` / `KAFKA_PRODUCER_CONFIG` `FR_CONF_OFFSET`
 * entries (relative to `fr_kafka_conf_t`) address correctly when the
 * framework passes `rlm_kafka_t` as the parse base.  The librdkafka
 * handle and per-topic tree inside `kconf` are released automatically
 * by talloc when the module instance is torn down (the library attaches
 * a lifecycle sentinel during config parse).
 */
typedef struct {
	fr_kafka_conf_t			kconf;		//!< parsed producer conf - MUST be first
	fr_time_delta_t			flush_timeout;	//!< How long `thread_detach` waits for in-flight
							//!< messages to drain before tearing down the producer.
	char const			*log_prefix;	//!< pre-rendered `"rlm_kafka (<instance>)"`, used by
							//!< librdkafka's log_cb which fires from internal
							//!< threads with no mctx in scope.  Built once in
							//!< mod_instantiate so we don't reformat per line.
} rlm_kafka_t;

/** Per-thread topic handle
 *
 * One per declared topic, created at thread_instantiate.  rd_kafka_topic_t
 * is per-producer so each worker thread has its own.
 */
typedef struct {
	char const		*name;
	rd_kafka_topic_t	*kt;
	fr_rb_node_t		node;
} rlm_kafka_topic_t;

typedef struct rlm_kafka_thread_s rlm_kafka_thread_t;

/** Per produce() invocation context
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
} rlm_kafka_msg_ctx_t;

struct rlm_kafka_thread_s {
	rlm_kafka_t const	*inst;
	fr_event_list_t		*el;

	rd_kafka_t		*rk;		//!< producer handle
	rd_kafka_queue_t	*main_q;	//!< producer main queue (delivery + error events)

	int			wake_pipe[2];	//!< self-pipe, [0]=read, [1]=write
	fr_event_fd_t		*ev_fd;		//!< event for wake_pipe[0]

	fr_rb_tree_t		*topics;	//!< rlm_kafka_topic_t per declared topic

	fr_dlist_head_t		inflight;	//!< outstanding rlm_kafka_msg_ctx_t

#ifndef NDEBUG
	pthread_t		worker_tid;	//!< pthread_self() captured at thread_instantiate.
						//!< Debug-build sanity check: the delivery-report and
						//!< error callbacks must run on this thread, never
						//!< cross-thread - a cross-thread hit would mean
						//!< librdkafka dispatched through something other than
						//!< our polled main queue and invalidate the no-lock
						//!< assumption around the inflight list.
#endif
};

/** Call env for `kafka.produce.<topic>`
 *
 * Topic comes from the method's second identifier and is validated
 * against the declared-topic list at call_env parse time, then stashed
 * here as a plain name string.
 */
typedef struct {
	char const		*topic;	//!< resolved topic name (validated at parse time)
	fr_value_box_t		*key;	//!< optional message key
	fr_value_box_t		*value;	//!< message payload
} rlm_kafka_env_t;

/** @param[in] a  rlm_kafka_topic_t
 *  @param[in] b  same.
 *  @return `strcmp` ordering of `a->name` and `b->name`. */
static int8_t topic_name_cmp(void const *a, void const *b)
{
	rlm_kafka_topic_t const *ta = a;
	rlm_kafka_topic_t const *tb = b;
	return CMP(strcmp(ta->name, tb->name), 0);
}

/** Destructor for per-thread topic handles.  Releases the rd_kafka_topic_t. */
static int _topic_free(rlm_kafka_topic_t *h)
{
	if (h->kt) rd_kafka_topic_destroy(h->kt);
	return 0;
}

/** Look up a per-thread topic handle by name
 *
 * @param[in] t    thread instance.
 * @param[in] name topic name (must have been declared at config time).
 * @return `rd_kafka_topic_t` if found, `NULL` otherwise.
 */
static rd_kafka_topic_t *kafka_thread_topic(rlm_kafka_thread_t *t, char const *name)
{
	rlm_kafka_topic_t	key = { .name = name };
	rlm_kafka_topic_t	*h;

	h = fr_rb_find(t->topics, &key);
	return h ? h->kt : NULL;
}

/** Broker-level error callback (connection failures etc)
 *
 * Not delivery reports - those come via dr_msg_cb.
 *
 * @param[in] rk     UNUSED.
 * @param[in] err    librdkafka error code.
 * @param[in] reason human-readable description.
 * @param[in] uctx   thread instance pointer we passed to rd_kafka_conf_set_opaque().
 */
static void _kafka_error_cb(UNUSED rd_kafka_t *rk, int err, char const *reason, void *uctx)
{
	rlm_kafka_thread_t	*t = talloc_get_type_abort(uctx, rlm_kafka_thread_t);

	/*
	 *	librdkafka dispatches error events via our polled main
	 *	queue, so this must fire on the worker thread that
	 *	called rd_kafka_poll - otherwise the no-lock assumption
	 *	around our per-thread state would be unsafe.
	 */
	fr_assert(pthread_equal(pthread_self(), t->worker_tid) != 0);

	ERROR("%s", rd_kafka_err2name(err), reason ? reason : "<UNKNOWN ERROR>");
}

/** librdkafka log callback - bridge internal library messages into the server log
 *
 * Called from librdkafka's internal threads (no request context, no mctx in
 * scope), so we pull the pre-rendered log prefix off the producer's opaque
 * pointer (the `rlm_kafka_thread_t` we attached at thread_instantiate).
 * Which librdkafka categories are actually emitted is controlled by the
 * top-level `debug` config knob.
 *
 * @param[in] rk    producer handle.  `rd_kafka_opaque(rk)` is the
 *                  `rlm_kafka_thread_t` set during thread_instantiate.
 * @param[in] level syslog-style severity (0 emerg .. 7 debug).
 * @param[in] fac   librdkafka facility / category, e.g. `BROKER`, `MSG`.
 * @param[in] buf   pre-formatted message body.
 */
static void _kafka_log_cb(rd_kafka_t const *rk, int level, char const *fac, char const *buf)
{
	rlm_kafka_thread_t	*t = talloc_get_type_abort(rd_kafka_opaque(rk), rlm_kafka_thread_t);

	switch (level) {
	case 0:		/* LOG_EMERG   */
	case 1:		/* LOG_ALERT   */
	case 2:		/* LOG_CRIT    */
	case 3:		/* LOG_ERR     */
		ERROR("%s - %s: %s", t->inst->log_prefix, fac, buf);
		break;

	case 4:		/* LOG_WARNING */
		WARN("%s - %s: %s", t->inst->log_prefix, fac, buf);
		break;

	case 5:		/* LOG_NOTICE  */
	case 6:		/* LOG_INFO    */
		INFO("%s - %s: %s", t->inst->log_prefix, fac, buf);
		break;

	default:	/* LOG_DEBUG and anything else */
		DEBUG("%s - %s: %s", t->inst->log_prefix, fac, buf);
		break;
	}
}

/** Drain every byte currently pending on the self-pipe read end
 *
 * librdkafka suppresses subsequent writes until the queue has been served,
 * so there's rarely more than one byte to drain, but the non-blocking read
 * loop handles any accumulated signals.
 */
static void kafka_drain_pipe(int fd)
{
	uint8_t	buf[256];
	while (read(fd, buf, sizeof(buf)) > 0);
}

/** Pipe became readable because librdkafka wrote a wake byte
 *
 * Drain the pipe, then drain the producer's main queue by polling with a zero
 * timeout.  `rd_kafka_poll()` dispatches delivery reports
 * (_kafka_delivery_report_cb) and broker errors (_kafka_error_cb) inline
 * on this thread.
 *
 * @param[in] el    UNUSED.
 * @param[in] fd    self-pipe read end.
 * @param[in] flags UNUSED.
 * @param[in] uctx  `rlm_kafka_thread_t` pointer.
 */
static void _kafka_fd_readable(UNUSED fr_event_list_t *el, int fd, UNUSED int flags, void *uctx)
{
	rlm_kafka_thread_t	*t = talloc_get_type_abort(uctx, rlm_kafka_thread_t);
	kafka_drain_pipe(fd);
	while (rd_kafka_poll(t->rk, 0) > 0);
}

/** Self-pipe error
 *
 * The pipe is ours - we create it at thread start, keep both ends
 * non-blocking, and librdkafka only writes one wake byte at a time.
 * There's no realistic way for this callback to fire in a healthy
 * process, and if it does, any recovery path would race with librdkafka's
 * in-flight opaque pointers (pctx entries we can't free without
 * coordinating with the library).  Fail hard instead.
 *
 * @param[in] el       UNUSED.
 * @param[in] fd       pipe read end.
 * @param[in] flags    UNUSED.
 * @param[in] fd_errno errno as reported by the event loop.
 * @param[in] uctx     UNUSED.
 */
static void _kafka_fd_error(UNUSED fr_event_list_t *el, int fd, UNUSED int flags, int fd_errno, void *uctx)
{
	rlm_kafka_thread_t	*t = talloc_get_type_abort(uctx, rlm_kafka_thread_t);

	FATAL("%s - self-pipe error (fd=%d): %s", t->inst->log_prefix, fd, fr_syserror(fd_errno));
}

/** Translate a librdkafka delivery-report error into a module rcode
 *
 * @param[in] request  associated request (for logging).
 * @param[in] pctx     produce context with the stashed error.
 * @return an `rlm_rcode_t` summarising the outcome.
 */
static rlm_rcode_t kafka_err_to_rcode(request_t *request, rlm_kafka_msg_ctx_t const *pctx)
{
	switch (pctx->err) {
	case RD_KAFKA_RESP_ERR_NO_ERROR:
		RDEBUG2("Delivered to partition %" PRId32 " offset %" PRId64, pctx->partition, pctx->offset);
		return RLM_MODULE_OK;

	case RD_KAFKA_RESP_ERR__MSG_TIMED_OUT:
	case RD_KAFKA_RESP_ERR__TIMED_OUT:
	case RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE:
		REDEBUG("Kafka delivery timed out: %s", rd_kafka_err2str(pctx->err));
		return RLM_MODULE_TIMEOUT;

	case RD_KAFKA_RESP_ERR_MSG_SIZE_TOO_LARGE:
	case RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART:
	case RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED:
	case RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC:
		REDEBUG("Kafka rejected message: %s", rd_kafka_err2str(pctx->err));
		return RLM_MODULE_REJECT;

	default:
		REDEBUG("Kafka delivery failed: %s (%s)",
			rd_kafka_err2name(pctx->err), rd_kafka_err2str(pctx->err));
		return RLM_MODULE_FAIL;
	}
}

/** Common produce-and-yield helper
 *
 * Submits a message to librdkafka and returns the produce context on
 * success.  The caller is responsible for yielding the request with the
 * returned pctx as rctx.  On synchronous failure the pctx is freed and
 * `NULL` is returned.
 *
 * @param[in] t         thread instance.
 * @param[in] request   request to yield on.
 * @param[in] topic     preconfigured per-thread topic handle.
 * @param[in] key       optional message key, may be `NULL`.
 * @param[in] key_len   length of `key`, 0 if `key` is `NULL`.
 * @param[in] value     message payload.
 * @param[in] value_len length of `value`.
 * @return the rlm_kafka_msg_ctx_t tracking the request, or `NULL` on failure.
 */
static inline CC_HINT(always_inline)
rlm_kafka_msg_ctx_t *kafka_produce_enqueue(rlm_kafka_thread_t *t, request_t *request,
					   rd_kafka_topic_t *topic,
					   uint8_t const *key, size_t key_len,
					   uint8_t const *value, size_t value_len)
{
	rlm_kafka_msg_ctx_t	*pctx;

	MEM(pctx = talloc(t, rlm_kafka_msg_ctx_t));
	*pctx = (rlm_kafka_msg_ctx_t) {
		.t = t,
		.request = request,
		.err = RD_KAFKA_RESP_ERR_NO_ERROR,
		.partition = RD_KAFKA_PARTITION_UA,
		.offset = -1
	};
	if (unlikely(rd_kafka_produce(topic, RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY,
				      /* librdkafka copies under MSG_F_COPY */
				      (void *)(uintptr_t) value, value_len,
				      key, key_len,
				      pctx) != 0)) {
		rd_kafka_resp_err_t	err = rd_kafka_last_error();

		talloc_free(pctx);

		REDEBUG("Failed enqueuing message - %s", rd_kafka_err2str(err));
		return NULL;
	}
	fr_dlist_insert_tail(&t->inflight, pctx);

	return pctx;
}

/** Per-topic call_env rules, applied against the `topic <name>` subsection
 *
 * Invoked recursively from @ref _kafka_topic_env_parse via `call_env_parse()`
 * so the framework handles pair lookup / tmpl compilation / offset writes
 * for us.
 *
 * Both `value` and `key` are typed `FR_TYPE_OCTETS`.  Kafka payloads and
 * keys are opaque byte strings on the wire, and casting to octets keeps
 * binary content (embedded NULs, high-bit bytes) intact without any
 * UTF-8/string-termination assumptions creeping in from intermediate
 * tmpl expansion.  It also means an integer-typed `key` attribute is
 * serialised in network byte order, which matches the keying convention
 * other Kafka clients use so the same numeric key hashes to the same
 * partition across producers.
 */
static call_env_parser_t const topic_env[] = {
	{ FR_CALL_ENV_OFFSET("value", FR_TYPE_OCTETS, CALL_ENV_FLAG_REQUIRED | CALL_ENV_FLAG_CONCAT,
			     rlm_kafka_env_t, value) },
	{ FR_CALL_ENV_OFFSET("key", FR_TYPE_OCTETS, CALL_ENV_FLAG_CONCAT | CALL_ENV_FLAG_NULLABLE,
			     rlm_kafka_env_t, key) },
	CALL_ENV_TERMINATOR
};

/** Resolve the topic named in the method's second identifier, then hand its
 *  subsection back to the call_env framework for per-topic `value` / `key`
 *  tmpl parsing.
 *
 * Invocations look like `kafka.produce.<topic_name>`.  We:
 *
 *  1. Validate the topic against the declared-topic tree.  Unknown topics
 *     fail here so typos surface at startup instead of at first produce.
 *  2. Emit a synthetic call_env entry carrying the topic name.
 *  3. Recurse into `call_env_parse()` with @ref topic_env pointed at the
 *     topic's CONF_SECTION - the framework walks `value` and `key` for us.
 *
 * Per-topic `value` and `key` means each declared topic carries its own
 * payload template; operators can publish different shapes to different
 * topics from one module instance.
 */
static int _kafka_topic_env_parse(TALLOC_CTX *ctx, call_env_parsed_head_t *out,
				  tmpl_rules_t const *t_rules, CONF_ITEM *ci,
				  call_env_ctx_t const *cec, UNUSED call_env_parser_t const *rule)
{
	rlm_kafka_t const	*inst = talloc_get_type_abort_const(cec->mi->data, rlm_kafka_t);
	fr_kafka_topic_t	*topic;
	call_env_parsed_t	*parsed;
	char const		*topic_name = cec->asked->name2;

	if (!topic_name) {
		cf_log_err(ci, "kafka.produce requires a topic name, e.g. kafka.produce.<topic>");
		return -1;
	}

	topic = kafka_topic_find(&inst->kconf, topic_name);
	if (!topic) {
		cf_log_err(ci, "Kafka topic '%s' is not declared in the '%s' module config",
			   topic_name, cec->mi->name);
		return -1;
	}

	/*
	 *	Topic name (plain string).
	 */
	MEM(parsed = call_env_parsed_add(ctx, out,
					 &(call_env_parser_t){
						.name = "topic",
						.flags = CALL_ENV_FLAG_PARSE_ONLY,
						.pair = {
							.parsed = {
								.offset = offsetof(rlm_kafka_env_t, topic),
								.type = CALL_ENV_PARSE_TYPE_VOID
							}
						}
					 }));
	call_env_parsed_set_data(parsed, talloc_strdup(ctx, topic_name));

	/*
	 *	Framework walks `value` / `key` inside the topic subsection
	 *	according to topic_env.
	 */
	return call_env_parse(ctx, out, "kafka", t_rules, topic->cs, cec, topic_env);
}

static const call_env_method_t rlm_kafka_produce_env = {
	FR_CALL_ENV_METHOD_OUT(rlm_kafka_env_t),
	.env = (call_env_parser_t[]) {
		{ FR_CALL_ENV_SUBSECTION_FUNC(CF_IDENT_ANY, CF_IDENT_ANY,
					      CALL_ENV_FLAG_PARSE_MISSING, _kafka_topic_env_parse) },
		CALL_ENV_TERMINATOR
	}
};

/** Resume a yielded module method after its delivery report has arrived
 *
 * Runs on the same worker as the originating produce (per-thread
 * producer), with `mctx->rctx` being the @ref rlm_kafka_msg_ctx_t the
 * method stashed on yield.  Translates the dr_msg_cb-populated error
 * into an rcode, frees the pctx, and hands control back to unlang.
 *
 * @param[out] p_result where to write the resulting rcode.
 * @param[in] mctx      module ctx carrying the pctx as rctx.
 * @param[in] request   the request being resumed.
 * @return `UNLANG_ACTION_CALCULATE_RESULT` always.
 */
static unlang_action_t mod_resume(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_kafka_msg_ctx_t	*pctx = talloc_get_type_abort(mctx->rctx, rlm_kafka_msg_ctx_t);
	rlm_rcode_t		rcode = kafka_err_to_rcode(request, pctx);

	talloc_free(pctx);
	RETURN_UNLANG_RCODE(rcode);
}

/** Module-method cancellation
 *
 * Do NOT free the pctx - librdkafka still owns the in-flight message and
 * will fire dr_msg_cb later with our opaque pointer.  Instead detach the
 * request so dr_msg_cb discards silently on arrival; dr_msg_cb frees the
 * pctx.
 *
 * Safe against dr_msg_cb racing because both run on the same worker thread
 * (per-worker producer).
 *
 * @param[in] mctx    module ctx with pctx as rctx.
 * @param[in] request associated request.
 * @param[in] action  UNUSED (we mask off everything except CANCEL).
 */
static void mod_signal(module_ctx_t const *mctx, request_t *request, UNUSED fr_signal_t action)
{
	rlm_kafka_msg_ctx_t	*pctx = talloc_get_type_abort(mctx->rctx, rlm_kafka_msg_ctx_t);

	RDEBUG2("Cancellation signal received - detaching delivery report");
	pctx->request = NULL;
}

/** Module method entry point for `kafka.produce.<topic>`
 *
 * The topic is the method's second identifier; `key` and `value` are
 * pulled from the module config via @ref rlm_kafka_produce_env:
 *
 * @code
 *     kafka {
 *         server = "broker1:9092"
 *         topic {
 *             Accounting-Request {
 *	           request_required_acks = -1
 *                 value = %json.encode(&request.[*])
 *                 key   = &User-Name
 *             }
 *         }
 *     }
 *
 *     recv Accounting-Request {
 *         kafka
 *     }
 * @endcode
 *
 * The topic name in `name2` is resolved against the declared-topic
 * tree at config-parse time (see @ref _kafka_topic_env_parse), so
 * typos fail fast.  Different topics reuse the same module instance -
 * e.g. `kafka.produce.auth` and `kafka.produce.accounting` both
 * dispatch through the same per-worker producer handle.
 *
 * Runtime behaviour: looks up the per-thread `rd_kafka_topic_t` by the
 * parse-time-resolved topic declaration, extracts the expanded
 * `key`/value tmpls from the call_env, hands everything to
 * @ref kafka_produce_enqueue, and yields until the delivery report
 * arrives (see @ref kafka_produce_resume for rcode mapping).
 *
 * @param[out] p_result UNUSED (resume writes the real rcode).
 * @param[in] mctx      module ctx (mctx->thread is the rlm_kafka_thread_t,
 *                      mctx->env_data is the rlm_kafka_env_t).
 * @param[in] request   the request being handled.
 * @return yielded on success, UNLANG_ACTION_FAIL if the produce couldn't
 *         even be enqueued.
 */
static unlang_action_t CC_HINT(nonnull) mod_produce(UNUSED unlang_result_t *p_result,
						    module_ctx_t const *mctx, request_t *request)
{
	rlm_kafka_thread_t	*t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	rlm_kafka_env_t		*env = talloc_get_type_abort(mctx->env_data, rlm_kafka_env_t);
	rd_kafka_topic_t	*topic;
	rlm_kafka_msg_ctx_t	*pctx;

	uint8_t const		*key = NULL;
	size_t			key_len = 0;

	topic = kafka_thread_topic(t, env->topic);
	if (unlikely(!topic)) {
		/*
		 *	Can't happen if parsing succeeded, but defensive.
		 */
		REDEBUG("Kafka topic '%s' has no handle on this worker", env->topic);
		RETURN_UNLANG_FAIL;
	}

	if (env->key) {
		key = env->key->vb_octets;
		key_len = env->key->vb_length;
	}

	pctx = kafka_produce_enqueue(t, request, topic,
				     key, key_len,
				     env->value->vb_octets, env->value->vb_length);
	if (unlikely(!pctx)) RETURN_UNLANG_FAIL;

	return unlang_module_yield(request, mod_resume, mod_signal,
				   ~FR_SIGNAL_CANCEL, pctx);
}

/** Xlat instance data - a cached topic name if the first arg is a literal. */
typedef struct {
	char const	*topic_name;
} rlm_kafka_xlat_inst_t;

/** Xlat thread instance data - the pre-resolved per-thread topic handle. */
typedef struct {
	rd_kafka_topic_t	*topic;
} rlm_kafka_xlat_thread_inst_t;

/** Xlat instance init: if the topic arg is a compile-time literal, capture
 *  and validate it against the declared-topic list.
 *
 * The first argument to `%kafka.produce(topic, value)` is the topic
 * name.  When it's a bare string with no xlat expansion, we:
 *
 *  1. Stash the name so the thread-instance init can turn it into a
 *     pre-resolved `rd_kafka_topic_t` handle once per worker (skipping
 *     the rbtree lookup on every call).
 *  2. Validate it against the module's declared topic list so typos
 *     fail at config-compile time rather than the first runtime call.
 *
 * When the arg is dynamic (an attribute reference or nested xlat),
 * we leave `topic_name` NULL and fall back to the per-call lookup in
 * the xlat runtime; validation happens there.
 */
static int kafka_xlat_instantiate(xlat_inst_ctx_t const *xctx)
{
	rlm_kafka_xlat_inst_t	*inst = talloc_get_type_abort(xctx->inst, rlm_kafka_xlat_inst_t);
	rlm_kafka_t const	*mod_inst = talloc_get_type_abort_const(xctx->mctx->mi->data, rlm_kafka_t);
	xlat_exp_t		*topic_arg;
	xlat_exp_t const	*topic_node;
	char const		*topic_name;
	fr_value_box_t		topic_vb = FR_VALUE_BOX_INITIALISER_NULL(topic_vb);

	/*
	 *	ex is the XLAT_FUNC node; its args are wrapped as
	 *	XLAT_GROUP children, one per positional argument.
	 */
	topic_arg = xlat_exp_head(xctx->ex->call.args);
	if (!topic_arg || topic_arg->type != XLAT_GROUP) return 0;

	if (!xlat_is_literal(topic_arg->group)) return 0;

	topic_node = xlat_exp_head(topic_arg->group);
	if (!topic_node) return 0;

	/*
	 *	Attempt to cast to a string
	 */
	if (topic_node->data.type != FR_TYPE_STRING) {
		if (unlikely(fr_value_box_cast(inst, &topic_vb, FR_TYPE_STRING, NULL, &topic_node->data) < 0)) {
			PERROR("First argument of %%<module>.produce() must be stringlike");
			return -1;
		}
		topic_name = topic_vb.vb_strvalue;
	} else {
		topic_name = topic_node->data.vb_strvalue;
	}

	/*
	 *	Validate now rather than waiting for a worker thread to
	 *	trip the runtime check.
	 */
	if (!kafka_topic_find(&mod_inst->kconf, topic_name)) {
		cf_log_err(xctx->mctx->mi->conf,
			   "Kafka topic '%s' is not declared in the '%s' module config",
			   topic_name, xctx->mctx->mi->name);
		fr_value_box_clear_value(&topic_vb);
		return -1;
	}
	inst->topic_name = talloc_strdup(inst, topic_name);
	fr_value_box_clear_value(&topic_vb);

	return 0;
}

/** Xlat thread-instance init: resolve the cached topic name to a handle
 *
 * Runs once per worker thread for each xlat node that has a literal
 * topic arg.  The module's `mod_thread_instantiate` runs first and
 * populates the per-thread topic handle tree, so by the time we reach
 * here the lookup is just a rbtree walk that we do once and cache.
 */
static int kafka_xlat_thread_instantiate(xlat_thread_inst_ctx_t const *xctx)
{
	rlm_kafka_xlat_inst_t const	*inst = talloc_get_type_abort_const(xctx->inst, rlm_kafka_xlat_inst_t);
	rlm_kafka_xlat_thread_inst_t	*t_inst = talloc_get_type_abort(xctx->thread, rlm_kafka_xlat_thread_inst_t);
	rlm_kafka_thread_t		*t;

	/*
	 *	We didn't cache the topic name the first time through
	 */
	if (!inst->topic_name) return 0;

	t = talloc_get_type_abort(xctx->mctx->thread, rlm_kafka_thread_t);
	if (!fr_cond_assert_msg((t_inst->topic = kafka_thread_topic(t, inst->topic_name)),
				"pre-resolved topic '%s' not found in thread handle tree", inst->topic_name)) {
		return -1;
	}

	return 0;
}

/** Xlat resume: translate delivery report into a "partition:offset" string
 *
 * @param[in] xctx_ctx talloc context for the returned value box.
 * @param[in] out      cursor to append the result to.
 * @param[in] xctx     xlat ctx, rctx points at the rlm_kafka_msg_ctx_t.
 * @param[in] request  associated request (for logging).
 * @param[in] in       UNUSED (original args).
 */
static xlat_action_t kafka_xlat_produce_resume(TALLOC_CTX *xctx_ctx, fr_dcursor_t *out,
					       xlat_ctx_t const *xctx,
					       request_t *request, UNUSED fr_value_box_list_t *in)
{
	rlm_kafka_msg_ctx_t	*pctx = talloc_get_type_abort(xctx->rctx, rlm_kafka_msg_ctx_t);
	fr_value_box_t		*vb;
	bool			delivered = (pctx->err == RD_KAFKA_RESP_ERR_NO_ERROR);

	if (unlikely(!delivered)) REDEBUG("Kafka produce failed: %s", rd_kafka_err2str(pctx->err));

	MEM(vb = fr_value_box_alloc(xctx_ctx, FR_TYPE_BOOL, NULL));
	vb->vb_bool = delivered;
	fr_dcursor_append(out, vb);

	talloc_free(pctx);
	return XLAT_ACTION_DONE;
}

/** Xlat cancellation
 *
 * Same semantics as @ref kafka_produce_signal: detach the request from
 * the in-flight `pctx` so the eventual dr_msg_cb discards silently
 * rather than trying to resume a cancelled request.  dr_msg_cb owns
 * the free.
 *
 * @param[in] xctx    xlat ctx (xctx->rctx is the rlm_kafka_msg_ctx_t).
 * @param[in] request UNUSED.
 * @param[in] action  UNUSED (we mask off everything except CANCEL).
 */
static void kafka_xlat_produce_signal(xlat_ctx_t const *xctx, UNUSED request_t *request, UNUSED fr_signal_t action)
{
	rlm_kafka_msg_ctx_t	*pctx = talloc_get_type_abort(xctx->rctx, rlm_kafka_msg_ctx_t);
	pctx->request = NULL;
}

static xlat_arg_parser_t const kafka_xlat_produce_args[] = {
	{ .required = true, .concat = true, .type = FR_TYPE_STRING },	/* topic */
	{ .required = true, .concat = true, .type = FR_TYPE_OCTETS },	/* key (zero-length octets = no key on the wire) */
	{ .required = true, .concat = true, .type = FR_TYPE_OCTETS },	/* value */
	XLAT_ARG_PARSER_TERMINATOR
};

/** `%kafka.produce(topic, key, value)` - runtime-named produce
 *
 * Unlike the @ref mod_produce method form (which resolves topics at
 * config-parse time), the xlat takes the topic name as a runtime
 * argument.  Use this when the topic or payload is chosen per-request:
 *
 * @code
 *     send Accounting-Response {
 *         if (!%kafka.produce('accounting', %{Acct-Session-Id}, %json.encode(&request.[*]))) {
 *             reject
 *         }
 *     }
 * @endcode
 *
 * `key` is optional: pass an empty string (or an unset attribute) to
 * produce without a key - librdkafka then uses the configured
 * partitioner to spread records across partitions.  When a non-empty
 * key is supplied, librdkafka hashes it to pick a partition, so
 * records with the same key end up on the same partition and preserve
 * per-key produce order on the consumer side.
 *
 * Returns a bool: `true` on successful delivery, `false` on failure.
 * The topic must have been declared in the module config (unknown
 * topics fail the xlat) so librdkafka per-topic settings continue to
 * apply to whichever topic is selected.
 *
 * Runtime behaviour mirrors the method: submit via
 * @ref kafka_produce_enqueue, yield until the delivery report arrives,
 * then resume in @ref kafka_xlat_produce_resume.
 */
static xlat_action_t kafka_xlat_produce(UNUSED TALLOC_CTX *xctx_ctx, UNUSED fr_dcursor_t *out,
					xlat_ctx_t const *xctx,
					request_t *request, fr_value_box_list_t *in)
{
	rlm_kafka_thread_t			*t = talloc_get_type_abort(xctx->mctx->thread, rlm_kafka_thread_t);
	rlm_kafka_xlat_thread_inst_t const	*t_inst = xctx->thread;
	fr_value_box_t				*topic_vb = fr_value_box_list_head(in);
	fr_value_box_t				*key_vb   = fr_value_box_list_next(in, topic_vb);
	fr_value_box_t				*value_vb = fr_value_box_list_next(in, key_vb);
	rd_kafka_topic_t			*topic;
	rlm_kafka_msg_ctx_t			*pctx;
	uint8_t const				*key = NULL;
	size_t					key_len = 0;

	/*
	 *	Fast path: a literal topic argument was pre-resolved to
	 *	an rd_kafka_topic_t at thread_instantiate.
	 */
	topic = t_inst ? t_inst->topic : NULL;
	if (!topic) topic = kafka_thread_topic(t, topic_vb->vb_strvalue);
	if (unlikely(!topic)) {
		REDEBUG("Kafka topic '%s' is not declared in the module config", topic_vb->vb_strvalue);
		return XLAT_ACTION_FAIL;
	}

	/*
	 *	Zero-length octets (e.g. `''` or an attribute expanding
	 *	to nothing) map to "no key" on the wire - librdkafka then
	 *	uses the configured partitioner instead of key-hash
	 *	partitioning.
	 */
	if (key_vb->vb_length > 0) {
		key = key_vb->vb_octets;
		key_len = key_vb->vb_length;
	}

	pctx = kafka_produce_enqueue(t, request, topic,
				     key, key_len,
				     value_vb->vb_octets, value_vb->vb_length);
	if (unlikely(!pctx)) return XLAT_ACTION_FAIL;

	return unlang_xlat_yield(request, kafka_xlat_produce_resume, kafka_xlat_produce_signal,
				 ~FR_SIGNAL_CANCEL, pctx);
}

/** Delivery report callback, runs on the worker thread that owns the producer
 *
 * If the request is still alive, stash the result and mark it runnable so
 * the resume function can translate the result to an rcode.  If cancelled
 * (pctx->request == `NULL`) just free the pctx silently.  A `NULL` opaque
 * means the message was produced without a context (e.g. a future
 * fire-and-forget path) - nothing to resume, just return.
 *
 * @param[in] rk        UNUSED.
 * @param[in] msg	librdkafka delivery report.
 * @param[in] opaque    UNUSED (we stash thread context on the message opaque).
 */
static void _kafka_delivery_report_cb(UNUSED rd_kafka_t *rk, rd_kafka_message_t const *msg, UNUSED void *opaque)
{
	rlm_kafka_msg_ctx_t	*pctx;

	/*
	 *	A NULL opaque is the documented signal for
	 *	fire-and-forget produces - nothing to resume or free,
	 *	just return before we try to unbox the pointer.
	 */
	if (!msg->_private) return;
	pctx = talloc_get_type_abort(msg->_private, rlm_kafka_msg_ctx_t);

	/*
	 *	DR dispatch must happen on the thread that owns the
	 *	producer - librdkafka is only allowed to wake us via
	 *	the polled main queue, so a cross-thread hit here would
	 *	invalidate the no-lock handling of the inflight list.
	 */
	fr_assert(pthread_equal(pthread_self(), pctx->t->worker_tid) != 0);

	fr_dlist_remove(&pctx->t->inflight, pctx);

	if (unlikely(!pctx->request)) {
		talloc_free(pctx);
		return;
	}

	pctx->err = msg->err;
	pctx->partition = msg->partition;
	pctx->offset = msg->offset;

	unlang_interpret_mark_runnable(pctx->request);
}

/** Create a per-thread rd_kafka_topic_t for every declared topic
 *
 * Called at thread_instantiate.  Walks the `topic { <name> { ... } }`
 * subsections directly off the module's CONF_SECTION - the kafka base
 * library has already parsed each per-topic conf into an
 * `fr_kafka_topic_conf_t` stashed via cf_data on the topic's section,
 * so we just fetch and dup it.
 */
static int kafka_topic_thread_handles(rlm_kafka_thread_t *t)
{
	MEM(t->topics = fr_rb_inline_talloc_alloc(t, rlm_kafka_topic_t, node, topic_name_cmp, NULL));

	if (!t->inst->kconf.topics) return 0;

	fr_rb_inorder_foreach(t->inst->kconf.topics, fr_kafka_topic_t, topic) {
		rlm_kafka_topic_t	*topic_t;
		rd_kafka_topic_conf_t	*ktc;

		MEM(ktc = rd_kafka_topic_conf_dup(topic->conf->rdtc));
		MEM(topic_t = talloc_zero(t->topics, rlm_kafka_topic_t));
		MEM(topic_t->name = talloc_strdup(topic_t, topic->name));
		topic_t->kt = rd_kafka_topic_new(t->rk, topic_t->name, ktc);
		if (!topic_t->kt) {
			/* librdkafka consumes tc only on success */
			rd_kafka_topic_conf_destroy(ktc);
			ERROR("Failed creating topic - %s",
			      topic_t->name, rd_kafka_err2str(rd_kafka_last_error()));
			talloc_free(topic_t);
			return -1;
		}
		talloc_set_destructor(topic_t, _topic_free);

		if (!fr_cond_assert_msg(fr_rb_insert(t->topics, topic_t), "duplicate topic handle")) {
			talloc_free(topic_t);
			return -1;
		}
	}
	endforeach

	return 0;
}

/** Tear down a worker's kafka producer state
 *
 * Orderly shutdown: detach in-flight requests so any late dr_msg_cbs
 * silently free their pctx rather than resuming a dying interpreter,
 * stop librdkafka from touching our self-pipe, flush outstanding
 * produces within `flush_timeout`, drain the final delivery reports,
 * then destroy the producer handle.
 *
 * @param[in] mctx thread-instance ctx (`mctx->thread` is our
 *                 rlm_kafka_thread_t).
 * @return 0 (never fails fatally - worst case we warn about undrained
 *         queue depth).
 */
static int mod_thread_detach(module_thread_inst_ctx_t const *mctx)
{
	rlm_kafka_thread_t	*t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	rlm_kafka_msg_ctx_t	*pctx;

	/*
	 *	Detach all in-flight requests.  After this, dr_msg_cb will
	 *	discard delivery reports rather than try to resume a dying
	 *	interpreter.
	 */
	for (pctx = fr_dlist_head(&t->inflight); pctx; pctx = fr_dlist_next(&t->inflight, pctx)) {
		pctx->request = NULL;
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
			WARN("kafka - Flush timed out - %d messages remain in queue",
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

/** Stand up this worker's kafka producer
 *
 * Creates the per-worker rd_kafka_t (duping the shared fr_kafka_conf_t
 * so librdkafka owns its own copy), builds per-thread rd_kafka_topic_t
 * handles for every declared topic, wires the producer's main queue to
 * a non-blocking self-pipe, and registers the pipe's read end with the
 * worker's event loop.  From this point onward delivery reports and
 * broker errors flow inline on this thread via @ref _kafka_fd_readable.
 *
 * @param[in] mctx thread-instance ctx (`mctx->thread` is our
 *                 rlm_kafka_thread_t, `mctx->el` is the worker's
 *                 event list).
 * @return 0 on success, -1 on any setup failure.
 */
static int mod_thread_instantiate(module_thread_inst_ctx_t const *mctx)
{
	rlm_kafka_t const	*inst = talloc_get_type_abort_const(mctx->mi->data, rlm_kafka_t);
	rlm_kafka_thread_t	*t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	rd_kafka_conf_t		*conf;
	char			errstr[512];

	t->inst = inst;
	t->el = mctx->el;
	t->wake_pipe[0] = t->wake_pipe[1] = -1;

#ifndef NDEBUG
	t->worker_tid = pthread_self();
#endif

	fr_dlist_talloc_init(&t->inflight, rlm_kafka_msg_ctx_t, entry);

	/*
	 *	rd_kafka_new consumes the conf, so dup it.
	 */
	conf = rd_kafka_conf_dup(inst->kconf.conf);
	rd_kafka_conf_set_dr_msg_cb(conf, _kafka_delivery_report_cb);
	rd_kafka_conf_set_error_cb(conf, _kafka_error_cb);
	rd_kafka_conf_set_opaque(conf, t);

	t->rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
	if (!t->rk) {
		rd_kafka_conf_destroy(conf);			/* only consumed on success */
		ERROR("rd_kafka_new failed: %s", errstr);
		return -1;
	}

	t->main_q = rd_kafka_queue_get_main(t->rk);
	if (!t->main_q) {
		ERROR("rd_kafka_queue_get_main returned NULL");
		goto error;
	}

	if (kafka_topic_thread_handles(t) < 0) goto error;

	/*
	 *	Wire up the self-pipe.  librdkafka writes a single byte to the
	 *	write end whenever the main queue transitions from empty to
	 *	non-empty, waking our event loop so _kafka_fd_readable() can
	 *	poll the queue.
	 */
	if (pipe(t->wake_pipe) < 0) {
		ERROR("pipe() failed - %s", fr_syserror(errno));
		goto error;
	}
	(void) fr_nonblock(t->wake_pipe[0]);
	(void) fr_nonblock(t->wake_pipe[1]);

	rd_kafka_queue_io_event_enable(t->main_q, t->wake_pipe[1], "x", 1);

	if (fr_event_fd_insert(t, &t->ev_fd, t->el, t->wake_pipe[0],
			       _kafka_fd_readable, NULL, _kafka_fd_error, t) < 0) {
		PERROR("fr_event_fd_insert failed");
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


/** Module config: just the kafka base producer config for now
 *
 * Kept as a local array rather than pointing `common.config` directly
 * at `KAFKA_BASE_PRODUCER_CONFIG` so we can drop in rlm_kafka-specific
 * entries (or additional librdkafka properties) alongside it later
 * without touching the library.
 */
static conf_parser_t const module_config[] = {
	KAFKA_BASE_CONFIG,
	KAFKA_PRODUCER_CONFIG,

	/*
	 *	How long to wait for in-flight messages to drain when a
	 *	worker tears down its producer handle.  Module-level (not
	 *	a librdkafka property) so we own the CONF_PARSER entry
	 *	rather than the kafka base library.
	 */
	{ FR_CONF_OFFSET("flush_timeout", rlm_kafka_t, flush_timeout), .dflt = "5s" },

	CONF_PARSER_TERMINATOR
};

/** Module-instance setup
 *
 * Stash the instance name for later use as a log prefix (librdkafka's
 * log_cb fires from internal threads, so no mctx is in scope at that
 * point), then register `_kafka_log_cb` on the shared producer conf.
 * Every per-thread `rd_kafka_conf_dup()` in `mod_thread_instantiate`
 * inherits this registration, so all producers feed through the same
 * bridge into the server log.
 *
 * @param[in] mctx module-instance ctx.
 * @return 0 on success, -1 on error.
 */
static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_kafka_t	*inst = talloc_get_type_abort(mctx->mi->data, rlm_kafka_t);

	MEM(inst->log_prefix = talloc_typed_asprintf(inst, "rlm_kafka (%s)", mctx->mi->name));
	rd_kafka_conf_set_log_cb(inst->kconf.conf, _kafka_log_cb);

	return 0;
}

/** Bootstrap-phase setup
 *
 * Just registers the `%kafka.produce()` xlat.  Topic declarations are
 * looked up directly via `cf_section_find` at call_env parse time
 * (see `_kafka_topic_env_parse`), and at worker thread_instantiate
 * time via `cf_section_find_next`, so there's nothing to build here.
 *
 * @param[in] mctx module-instance ctx.
 * @return 0 on success, -1 on error.
 */
static int mod_bootstrap(module_inst_ctx_t const *mctx)
{
	xlat_t	*xlat;

	xlat = module_rlm_xlat_register(mctx->mi->boot, mctx, "produce", kafka_xlat_produce, FR_TYPE_BOOL);
	if (!xlat) return -1;
	xlat_func_args_set(xlat, kafka_xlat_produce_args);
	xlat_func_instantiate_set(xlat, kafka_xlat_instantiate,
				  rlm_kafka_xlat_inst_t, NULL, NULL);
	xlat_func_thread_instantiate_set(xlat, kafka_xlat_thread_instantiate,
					 rlm_kafka_xlat_thread_inst_t, NULL, NULL);

	return 0;
}

module_rlm_t rlm_kafka = {
	.common = {
		.magic			= MODULE_MAGIC_INIT,
		.name			= "kafka",
		.inst_size		= sizeof(rlm_kafka_t),
		.thread_inst_size	= sizeof(rlm_kafka_thread_t),
		.config			= module_config,
		.bootstrap		= mod_bootstrap,
		.instantiate		= mod_instantiate,
		.thread_instantiate	= mod_thread_instantiate,
		.thread_detach		= mod_thread_detach
	},
	/*
	 *	`send` and `recv` alias `produce` so the call reads naturally
	 *	in its surrounding section - e.g. `recv Access-Request {
	 *	kafka.recv.auth }` or `send Access-Accept { kafka.send.audit }`.
	 *	All three dispatch to the same producer path.
	 */
	.method_group = {
		.bindings = (module_method_binding_t[]){
			{
				.section = SECTION_NAME("produce", CF_IDENT_ANY),
				.method = mod_produce,
				.method_env = &rlm_kafka_produce_env
			},
			{
				.section = SECTION_NAME("send", CF_IDENT_ANY),
				.method = mod_produce,
				.method_env = &rlm_kafka_produce_env
			},
			{
				.section = SECTION_NAME("recv", CF_IDENT_ANY),
				.method = mod_produce,
				.method_env = &rlm_kafka_produce_env
			},
			MODULE_BINDING_TERMINATOR
		}
	}
};

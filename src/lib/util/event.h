#pragma once
/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/** Wrapper around libkqueue to make managing events easier
 *
 * @file src/lib/util/event.h
 *
 * @copyright 2007 The FreeRADIUS server project
 * @copyright 2007 Alan DeKok (aland@deployingradius.com)
 */
RCSIDH(event_h, "$Id$")

#ifdef __cplusplus
extern "C" {
#endif

#include <freeradius-devel/util/timer.h>

/*
 *	Allow public and private versions of the same structures
 */
#ifndef _EVENT_LIST_PRIVATE
typedef struct fr_event_list_pub_s fr_event_list_t;
#endif

/** Public event list structure
 *
 * Make the event timer list available, but nothing else.
 *
 * This allows us to access these values without the cost of a function call.
 */
struct fr_event_list_pub_s {
	fr_timer_list_t *tl;			//!< The timer list associated with this event loop.
};

#include <freeradius-devel/build.h>
#include <freeradius-devel/missing.h>
#include <freeradius-devel/util/time.h>

#include <freeradius-devel/util/talloc.h>

#include <stdbool.h>
#include <sys/event.h>


#ifdef WITH_EVENT_DEBUG
#  define EVENT_DEBUG(fmt, ...) printf("EVENT:");printf(fmt, ## __VA_ARGS__);printf("\n");
#  ifndef EVENT_REPORT_FREQ
#    define EVENT_REPORT_FREQ	5
#  endif
#else
#  define EVENT_DEBUG(...)
#endif

/** An opaque file descriptor handle
 */
typedef struct fr_event_fd fr_event_fd_t;

/** An opaque PID status handle
 */
typedef struct fr_event_pid fr_event_pid_t;

/** An opaque user event handle
 */
typedef struct fr_event_user_s fr_event_user_t;

/** The type of filter to install for an FD
 */
typedef enum {
	FR_EVENT_FILTER_IO = 1,			//!< Combined filter for read/write functions/
	FR_EVENT_FILTER_VNODE			//!< Filter for vnode subfilters
} fr_event_filter_t;

/** Operations to perform on filter
 */
typedef enum {
	FR_EVENT_OP_SUSPEND = 1,		//!< Temporarily remove the relevant filter from kevent.
	FR_EVENT_OP_RESUME			//!< Reinsert the filter into kevent.
} fr_event_op_t;

/** Structure describing a modification to a filter's state
 */
typedef struct {
	size_t		offset;			//!< Offset of function in func struct.
	fr_event_op_t	op;			//!< Operation to perform on function/filter.
} fr_event_update_t;

/** Temporarily remove the filter for a func from kevent
 *
 * Use to populate elements in an array of #fr_event_update_t.
 *
 @code {.c}
   static fr_event_update_t pause_read[] = {
   	FR_EVENT_SUSPEND(fr_event_io_func_t, read),
   	{ 0 }
   }
 @endcode
 *
 * @param[in] _s 	the structure containing the func to suspend.
 * @param[in] _f	the func to suspend.
 */
#define FR_EVENT_SUSPEND(_s, _f)	{ .offset = offsetof(_s, _f), .op = FR_EVENT_OP_SUSPEND }

/** Re-add the filter for a func from kevent
 *
 * Use to populate elements in an array of #fr_event_update_t.
 *
 @code {.c}
   static fr_event_update_t resume_read[] = {
   	FR_EVENT_RESUME(fr_event_io_func_t, read),
   	{ 0 }
   }
 @endcode
 *
 * @param[in] _s 	the structure containing the func to suspend.
 * @param[in] _f	the func to resume.
 */
#define FR_EVENT_RESUME(_s, _f)		{ .offset = offsetof(_s, _f), .op = FR_EVENT_OP_RESUME }

/** Called after each event loop cycle
 *
 * Called before calling kqueue to put the thread in a sleeping state.
 *
 * @param[in] now	The current time.
 * @param[in] wake	When we'll next need to wake up to service an event.
 * @param[in] uctx	User ctx passed to #fr_event_list_alloc.
 */
typedef	int (*fr_event_status_cb_t)(fr_time_t now, fr_time_delta_t wake, void *uctx);

/** Called when an IO event occurs on a file descriptor
 *
 * @param[in] el	Event list the file descriptor was inserted into.
 * @param[in] fd	That experienced the IO event.
 * @param[in] flags	field as returned by kevent.
 * @param[in] uctx	User ctx passed to #fr_event_fd_insert.
 */
typedef void (*fr_event_fd_cb_t)(fr_event_list_t *el, int fd, int flags, void *uctx);

/** Called when an IO error event occurs on a file descriptor
 *
 * @param[in] el	Event list the file descriptor was inserted into.
 * @param[in] fd	That experienced the IO event.
 * @param[in] flags	field as returned by kevent.
 * @param[in] fd_errno	File descriptor error.
 * @param[in] uctx	User ctx passed to #fr_event_fd_insert.
 */
typedef void (*fr_event_error_cb_t)(fr_event_list_t *el, int fd, int flags, int fd_errno, void *uctx);

/** Called when a child process has exited
 *
 * @param[in] el	Event list
 * @param[in] pid	That exited
 * @param[in] status	exit status
 * @param[in] uctx	User ctx passed to #fr_event_fd_insert.
 */
typedef void (*fr_event_pid_cb_t)(fr_event_list_t *el, pid_t pid, int status, void *uctx);

/** Called when a user kevent occurs
 *
 * @param[in] el	Event list
 * @param[in] uctx	User ctx passed to #fr_event_user_insert.
 */
typedef void (*fr_event_user_cb_t)(fr_event_list_t *el, void *uctx);

/** Called when a post event fires
 *
 * @param[in] el	Event list the post event was inserted into.
 * @param[in] now	The current time.
 * @param[in] uctx	User ctx passed to #fr_timer_in or #fr_timer_at.
 */
typedef	void (*fr_event_post_cb_t)(fr_event_list_t *el, fr_time_t now, void *uctx);

/** Callbacks for the #FR_EVENT_FILTER_IO filter
 */
typedef struct {
	fr_event_fd_cb_t	read;			//!< Callback for when data is available.
	fr_event_fd_cb_t	write;			//!< Callback for when we can write data.
} fr_event_io_func_t;

/** Callbacks for the #FR_EVENT_FILTER_VNODE filter
 */
typedef struct {
	fr_event_fd_cb_t	delete;			//!< The file was deleted.
	fr_event_fd_cb_t	write;			//!< The file was written to.
	fr_event_fd_cb_t	extend;			//!< Additional files were added to a directory.
	fr_event_fd_cb_t	attrib;			//!< File attributes changed.
	fr_event_fd_cb_t	link;			//!< The link count on the file changed.
	fr_event_fd_cb_t	rename;			//!< The file was renamed.
#ifdef NOTE_REVOKE
	fr_event_fd_cb_t	revoke;			//!< Volume containing the file was unmounted or
							///< access was revoked with revoke().
#endif
#ifdef NOTE_FUNLOCK
	fr_event_fd_cb_t	funlock;		//!< The file was unlocked.
#endif
} fr_event_vnode_func_t;

/** Union of all filter functions
 */
typedef union {
	fr_event_io_func_t	io;			//!< Read/write functions.
	fr_event_vnode_func_t	vnode;			//!< vnode callback functions.
} fr_event_funcs_t;

uint64_t	fr_event_list_num_fds(fr_event_list_t *el);
uint64_t	fr_event_list_num_timers(fr_event_list_t *el);
int		fr_event_list_kq(fr_event_list_t *el);
fr_time_t	fr_event_list_time(fr_event_list_t *el) CC_HINT(nonnull);

int		_fr_event_fd_move(NDEBUG_LOCATION_ARGS
				 fr_event_list_t *dst, fr_event_list_t *src, int fd, fr_event_filter_t filter);
#define		fr_event_fd_mode(...) _fr_event_fd_move(NDEBUG_LOCATION_EXP __VA_ARGS__)

int		_fr_event_filter_insert(NDEBUG_LOCATION_ARGS
				        TALLOC_CTX *ctx, fr_event_fd_t **ef_out,
				        fr_event_list_t *el, int fd,
				        fr_event_filter_t filter,
				        void *funcs,
				        fr_event_error_cb_t error,
				        void *uctx);
#define		fr_event_filter_insert(...) _fr_event_filter_insert(NDEBUG_LOCATION_EXP __VA_ARGS__)

int		_fr_event_filter_update(NDEBUG_LOCATION_ARGS
					fr_event_list_t *el, int fd, fr_event_filter_t filter,
					fr_event_update_t const updates[]);
#define		fr_event_filter_update(...) _fr_event_filter_update(NDEBUG_LOCATION_EXP __VA_ARGS__)

int		_fr_event_fd_insert(NDEBUG_LOCATION_ARGS
				    TALLOC_CTX *ctx, fr_event_fd_t **ef_out, fr_event_list_t *el, int fd,
				    fr_event_fd_cb_t read_fn,
				    fr_event_fd_cb_t write_fn,
				    fr_event_error_cb_t error,
				    void *uctx);
#define		fr_event_fd_insert(...) _fr_event_fd_insert(NDEBUG_LOCATION_EXP __VA_ARGS__)

int		fr_event_fd_delete(fr_event_list_t *el, int fd, fr_event_filter_t filter);

fr_event_fd_t	*fr_event_fd_handle(fr_event_list_t *el, int fd, fr_event_filter_t filter);

fr_event_fd_cb_t fr_event_fd_cb(fr_event_fd_t *ef, int filter, int fflags);

void		*fr_event_fd_uctx(fr_event_fd_t *ef);

#ifndef NDEBUG
int		fr_event_fd_armour(fr_event_list_t *el, int fd, fr_event_filter_t, uintptr_t armour);
int		fr_event_fd_unarmour(fr_event_list_t *el, int fd, fr_event_filter_t filter, uintptr_t armour);
#endif

int		_fr_event_pid_wait(NDEBUG_LOCATION_ARGS
				   TALLOC_CTX *ctx, fr_event_list_t *el, fr_event_pid_t const **ev_p,
				   pid_t pid, fr_event_pid_cb_t wait_fn, void *uctx)
				   CC_HINT(nonnull(NDEBUG_LOCATION_NONNULL(2)));
#define		fr_event_pid_wait(...) _fr_event_pid_wait(NDEBUG_LOCATION_EXP __VA_ARGS__)

int		_fr_event_pid_reap(NDEBUG_LOCATION_ARGS
				   fr_event_list_t *el, pid_t pid,
				   fr_event_pid_cb_t wait_fn, void *uctx)
				   CC_HINT(nonnull(NDEBUG_LOCATION_NONNULL(1)));
#define		fr_event_pid_reap(...) _fr_event_pid_reap(NDEBUG_LOCATION_EXP __VA_ARGS__)

unsigned int	fr_event_list_reap_signal(fr_event_list_t *el, fr_time_delta_t timeout, int signal);

int 		_fr_event_user_insert(NDEBUG_LOCATION_ARGS
				      TALLOC_CTX *ctx, fr_event_list_t *el, fr_event_user_t **ev_p,
				      bool trigger, fr_event_user_cb_t callback, void *uctx);
#define		fr_event_user_insert(_ctx, _ev_p, _el, _trigger, _callback, _uctx) \
			_fr_event_user_insert(NDEBUG_LOCATION_EXP _ctx, _ev_p, _el, _trigger, _callback, _uctx)

int		fr_event_user_trigger(fr_event_list_t *el, fr_event_user_t *ev);

int		fr_event_user_delete(fr_event_list_t *el, fr_event_user_cb_t user, void *uctx) CC_HINT(nonnull(1,2));

int		fr_event_pre_insert(fr_event_list_t *el, fr_event_status_cb_t callback, void *uctx) CC_HINT(nonnull(1,2));
int		fr_event_pre_delete(fr_event_list_t *el, fr_event_status_cb_t callback, void *uctx) CC_HINT(nonnull(1,2));

int		fr_event_post_insert(fr_event_list_t *el, fr_event_post_cb_t callback, void *uctx) CC_HINT(nonnull(1,2));
int		fr_event_post_delete(fr_event_list_t *el, fr_event_post_cb_t callback, void *uctx) CC_HINT(nonnull(1,2));

int		fr_event_corral(fr_event_list_t *el, fr_time_t now, bool wait);
void		fr_event_service(fr_event_list_t *el);

void		fr_event_loop_exit(fr_event_list_t *el, int code);
bool		fr_event_loop_exiting(fr_event_list_t *el);
int		fr_event_loop(fr_event_list_t *el);

fr_event_list_t	*fr_event_list_alloc(TALLOC_CTX *ctx, fr_event_status_cb_t status, void *status_ctx);

bool		fr_event_list_empty(fr_event_list_t *el);

#ifdef __cplusplus
}
#endif

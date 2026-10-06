#ifndef BACKEND_QNX_SCREEN_EVENT_MONITOR_H
#define BACKEND_QNX_SCREEN_EVENT_MONITOR_H

#include <pthread.h>

#include <screen/screen.h>

/*
 * screen_get_event() is a non-blocking, poll-style call: there is no fd we
 * can hand to wl_event_loop_add_fd() directly. QNX Screen instead delivers a
 * pulse (via screen_notify()/screen_register_event()) to a channel when a new
 * event becomes available.
 *
 * This monitor runs a small helper thread that blocks in MsgReceive() on
 * that channel and, on each pulse, writes a byte into a pipe. The write end
 * of the pipe is what actually gets attached to the compositor's
 * wl_event_loop, so screen_get_event()/screen_post_window() and friends are
 * only ever called from the main (event loop) thread.
 */
struct wlr_qnx_screen_event_monitor {
	screen_context_t context;
	int chid;
	int coid;
	pthread_t thread;
	int pipe_fds[2];
	struct sigevent event;
};

struct wlr_qnx_screen_event_monitor *wlr_qnx_screen_event_monitor_create(
	screen_context_t context);
void wlr_qnx_screen_event_monitor_destroy(
	struct wlr_qnx_screen_event_monitor *monitor);
/* Re-arm notification after the main thread has drained pending events. */
void wlr_qnx_screen_event_monitor_arm(
	struct wlr_qnx_screen_event_monitor *monitor);

#endif

#ifndef WLR_BACKEND_QNX_SCREEN_VSYNC_NOTIFIER_H
#define WLR_BACKEND_QNX_SCREEN_VSYNC_NOTIFIER_H

#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>

#include <screen/screen.h>

/*
 * Turns display vsync into a readable fd, so an output can emit frame events
 * at the panel's real refresh rate. See vsync_notifier.c.
 */
struct wlr_qnx_screen_vsync_notifier {
	screen_display_t display;

	pthread_t thread;
	int pipe_fds[2];

	/*
	 * Set to 1 when the reader wants a tick, cleared by the thread as it
	 * delivers one: one arm(), one event.
	 */
	atomic_int armed;
	atomic_int stop;

	/* Set if the thread gave up (see screen_wait_vsync()). */
	atomic_bool failed;
};

/*
 * Spawns the vsync thread. Nothing is delivered until
 * wlr_qnx_screen_vsync_notifier_arm() is called.
 */
struct wlr_qnx_screen_vsync_notifier *wlr_qnx_screen_vsync_notifier_create(
	screen_display_t display);

/*
 * Joins the thread, which can block for up to one refresh because
 * screen_wait_vsync() is not interruptible.
 */
void wlr_qnx_screen_vsync_notifier_destroy(
	struct wlr_qnx_screen_vsync_notifier *n);

/* Allow one tick to be delivered. */
void wlr_qnx_screen_vsync_notifier_arm(
	struct wlr_qnx_screen_vsync_notifier *n);

/* True if the thread has stopped and no further ticks will arrive. */
bool wlr_qnx_screen_vsync_notifier_failed(
	struct wlr_qnx_screen_vsync_notifier *n);

/*
 * Consumes everything pending on the fd. Call this from the fd's
 * WL_EVENT_READABLE handler, before arming for the next tick.
 */
void wlr_qnx_screen_vsync_notifier_drain(
	struct wlr_qnx_screen_vsync_notifier *n);

#endif // WLR_BACKEND_QNX_SCREEN_VSYNC_NOTIFIER_H

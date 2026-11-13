/*
 * Vsync notifier.
 *
 * screen_wait_vsync() blocks the calling thread until the next vsync on a
 * display, which makes it unusable on the compositor's single-threaded event
 * loop -- but it is the only display-locked timing source QNX Screen offers
 * here, and it is accurate: measured on virtio/virgl at 59.97 Hz with
 * +/-0.65 ms jitter and zero drift over 5s.
 *
 * So it runs on its own thread, and writes one byte to a non-blocking pipe per
 * tick, which turns each vsync into an ordinary readable fd the wayland event
 * loop can wait on -- the same shape event_monitor.c uses for input, minus the
 * message channel, since nothing needs to be correlated with the wakeup.
 *
 * Ticks are delivered one per wlr_qnx_screen_vsync_notifier_arm() call, which
 * mirrors a page flip: one arm, one completion. That both keeps a stalled
 * compositor from accumulating a backlog of frame events and keeps the fd from
 * staying permanently readable.
 *
 * screen_wait_vsync() documents only "the display is mirrored" as a failure,
 * but a failure here would silently stop frame events, so the thread records
 * it in `failed` for the output to fall back on. Note that destroying the
 * notifier can take up to one refresh: the thread only notices `stop` after
 * its current screen_wait_vsync() returns, so this must never be called from
 * anything latency sensitive.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "backend/qnx_screen_vsync_notifier.h"

static void *vsync_thread_main(void *argument) {
	struct wlr_qnx_screen_vsync_notifier *n = argument;

	while (!atomic_load(&n->stop)) {
		if (screen_wait_vsync(n->display) != 0) {
			if (!atomic_load(&n->stop)) {
				wlr_log_errno(WLR_ERROR,
					"qnx-screen: screen_wait_vsync failed, falling "
					"back to timer-based frame pacing");
			}
			atomic_store(&n->failed, true);
			break;
		}
		if (atomic_load(&n->stop)) {
			break;
		}
		/* Deliver at most one tick per arm(). */
		int expected = 1;
		if (atomic_compare_exchange_strong(&n->armed, &expected, 0)) {
			char ch = 0;
			ssize_t ignored = write(n->pipe_fds[1], &ch, sizeof(ch));
			(void)ignored;
		}
	}

	return NULL;
}

struct wlr_qnx_screen_vsync_notifier *wlr_qnx_screen_vsync_notifier_create(
		screen_display_t display) {
	struct wlr_qnx_screen_vsync_notifier *n = calloc(1, sizeof(*n));
	if (n == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	n->display = display;
	atomic_store(&n->armed, 0);
	atomic_store(&n->stop, 0);
	atomic_store(&n->failed, false);

	if (pipe2(n->pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to create vsync pipe");
		goto err_free;
	}

	if (pthread_create(&n->thread, NULL, vsync_thread_main, n) != 0) {
		wlr_log_errno(WLR_ERROR, "Failed to spawn vsync thread");
		goto err_pipe;
	}

	return n;

err_pipe:
	close(n->pipe_fds[0]);
	close(n->pipe_fds[1]);
err_free:
	free(n);
	return NULL;
}

void wlr_qnx_screen_vsync_notifier_destroy(
		struct wlr_qnx_screen_vsync_notifier *n) {
	if (n == NULL) {
		return;
	}

	atomic_store(&n->stop, 1);
	/*
	 * No wakeup is sent: screen_wait_vsync() is not interruptible here, so we
	 * join and wait for the current call to return on its own (one refresh).
	 */
	pthread_join(n->thread, NULL);

	close(n->pipe_fds[0]);
	close(n->pipe_fds[1]);
	free(n);
}

void wlr_qnx_screen_vsync_notifier_arm(
		struct wlr_qnx_screen_vsync_notifier *n) {
	if (n != NULL) {
		atomic_store(&n->armed, 1);
	}
}

bool wlr_qnx_screen_vsync_notifier_failed(
		struct wlr_qnx_screen_vsync_notifier *n) {
	return n == NULL || atomic_load(&n->failed);
}

void wlr_qnx_screen_vsync_notifier_drain(
		struct wlr_qnx_screen_vsync_notifier *n) {
	char buf[16];
	while (read(n->pipe_fds[0], buf, sizeof(buf)) > 0) {
		;
	}
}

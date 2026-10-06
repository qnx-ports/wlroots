/*
 * QNX Screen event-notification monitor thread.
 *
 * Adapted from qnx-ports/weston's qnx-screen-event-monitor.c (Copyright (C)
 * 2023 Blackberry Limited, MIT licensed) for use as a wlroots backend
 * component. The pulse/pipe plumbing is unchanged; only naming and the log
 * calls have been adjusted to wlroots conventions.
 */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "backend/qnx_screen_event_monitor.h"

static const int c_screen_code = _PULSE_CODE_MINAVAIL + 0;
static const int c_arm_code = _PULSE_CODE_MINAVAIL + 1;
static const int c_quit_code = _PULSE_CODE_MINAVAIL + 2;

static void *event_monitor_main(void *argument) {
	struct wlr_qnx_screen_event_monitor *m = argument;
	int armed = 1;

	while (1) {
		struct _pulse msg;
		memset(&msg, 0, sizeof(msg));
		int rcvid = MsgReceive(m->chid, &msg, sizeof(msg), NULL);
		if (rcvid != 0) continue;
		if (msg.code == c_quit_code) {
			break;
		} else if (msg.code == c_arm_code) {
			armed = 1;
		} else if (msg.code == c_screen_code) {
			if (armed) {
				char ch = 0;
				write(m->pipe_fds[1], &ch, sizeof(ch));
				armed = 0;
			}
		}
	}

	return NULL;
}

struct wlr_qnx_screen_event_monitor *wlr_qnx_screen_event_monitor_create(
		screen_context_t context) {
	struct wlr_qnx_screen_event_monitor *m = calloc(1, sizeof(*m));
	if (m == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	m->context = context;

	m->chid = ChannelCreate(_NTO_CHF_DISCONNECT | _NTO_CHF_UNBLOCK |
		_NTO_CHF_PRIVATE);
	if (m->chid < 0) {
		wlr_log(WLR_ERROR, "Failed to create QNX channel");
		goto err_free;
	}

	m->coid = ConnectAttach(0, 0, m->chid, _NTO_SIDE_CHANNEL, 0);
	if (m->coid < 0) {
		wlr_log(WLR_ERROR, "Failed to attach QNX connection");
		goto err_channel;
	}

	if (pipe2(m->pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to create pipe");
		goto err_connection;
	}

	SIGEV_PULSE_INIT(&m->event, m->coid, SIGEV_PULSE_PRIO_INHERIT,
		c_screen_code, 0);
	if (screen_register_event(m->context, &m->event) < 0) {
		wlr_log_errno(WLR_ERROR, "screen_register_event failed");
		goto err_pipe;
	}

	if (screen_notify(m->context, SCREEN_NOTIFY_EVENT, NULL, &m->event) < 0) {
		wlr_log_errno(WLR_ERROR, "screen_notify failed");
		goto err_register;
	}

	if (pthread_create(&m->thread, NULL, event_monitor_main, m) != 0) {
		wlr_log(WLR_ERROR, "Failed to spawn QNX Screen event monitor thread");
		goto err_notify;
	}

	return m;

err_notify:
	screen_notify(m->context, SCREEN_NOTIFY_EVENT, NULL, NULL);
err_register:
	screen_unregister_event(&m->event);
err_pipe:
	close(m->pipe_fds[0]);
	close(m->pipe_fds[1]);
err_connection:
	ConnectDetach(m->coid);
err_channel:
	ChannelDestroy(m->chid);
err_free:
	free(m);
	return NULL;
}

void wlr_qnx_screen_event_monitor_destroy(
		struct wlr_qnx_screen_event_monitor *m) {
	if (m == NULL) {
		return;
	}

	MsgSendPulse(m->coid, SIGEV_PULSE_PRIO_INHERIT, c_quit_code, 0);
	pthread_join(m->thread, NULL);

	screen_notify(m->context, SCREEN_NOTIFY_EVENT, NULL, NULL);
	screen_unregister_event(&m->event);

	close(m->pipe_fds[0]);
	close(m->pipe_fds[1]);
	ConnectDetach(m->coid);
	ChannelDestroy(m->chid);
	free(m);
}

void wlr_qnx_screen_event_monitor_arm(
		struct wlr_qnx_screen_event_monitor *m) {
	MsgSendPulse(m->coid, SIGEV_PULSE_PRIO_INHERIT, c_arm_code, 0);
}

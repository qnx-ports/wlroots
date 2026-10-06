#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_qnx_redir.h>
#include <wlr/util/log.h>
#include "qnx-redir-protocol.h"

#define QNX_REDIR_VERSION 1

static void qnx_redir_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_qnx_redir *qnx_redir = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wl_qnx_redir_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	// No requests in this interface, so no implementation/destructor needed
	// beyond the default.
	wl_resource_set_implementation(resource, NULL, NULL, NULL);

	// The whole point of this global: tell the client which display_id to
	// use, right away, since there's nothing else for this object to do.
	wl_qnx_redir_send_display_id(resource, qnx_redir->display_id);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_qnx_redir *qnx_redir =
		wl_container_of(listener, qnx_redir, display_destroy);

	wl_signal_emit_mutable(&qnx_redir->events.destroy, qnx_redir);

	wl_list_remove(&qnx_redir->display_destroy.link);
	wl_global_destroy(qnx_redir->global);
	free(qnx_redir);
}

struct wlr_qnx_redir *wlr_qnx_redir_create(struct wl_display *display,
		int display_id) {
	struct wlr_qnx_redir *qnx_redir = calloc(1, sizeof(*qnx_redir));
	if (qnx_redir == NULL) {
		return NULL;
	}

	qnx_redir->global = wl_global_create(display, &wl_qnx_redir_interface,
		QNX_REDIR_VERSION, qnx_redir, qnx_redir_bind);
	if (qnx_redir->global == NULL) {
		free(qnx_redir);
		return NULL;
	}

	qnx_redir->display_id = display_id;

	wl_signal_init(&qnx_redir->events.destroy);

	qnx_redir->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &qnx_redir->display_destroy);

	return qnx_redir;
}


#ifndef WLR_TYPES_WLR_QNX_REDIR_H
#define WLR_TYPES_WLR_QNX_REDIR_H

#include <wayland-server-core.h>

/*
 * wl_qnx_redir is a trivial global: as soon as a client binds it, the
 * compositor sends a single display_id event and the object is otherwise
 * inert. There is no request in the protocol, so no client -> server
 * traffic ever needs handling.
 */
struct wlr_qnx_redir {
	struct wl_global *global;

	// The value sent in the display_id event to every client that binds.
	// Set once at creation time by the caller (see wlr_qnx_redir_create),
	// which is expected to have gotten it from a backend that actually
	// knows the native QNX display id (e.g. wlr_qnx_backend_get_display_id).
	int display_id;

	struct wl_listener display_destroy;

	struct {
		struct wl_signal destroy;
	} events;

	void *data;
};

// Creates the wl_qnx_redir global and advertises it on the display.
// display_id is the value sent to clients in the display_id event; this
// type has no way to determine it itself, so the caller must supply it
// (typically read from a QNX-aware backend at startup).
// Returns NULL on allocation failure.
struct wlr_qnx_redir *wlr_qnx_redir_create(struct wl_display *display,
	int display_id);

#endif


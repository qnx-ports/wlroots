#ifndef BACKEND_QNX_SCREEN_H
#define BACKEND_QNX_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include <screen/screen.h>

#include <gbm.h>

#include <wayland-server-core.h>

#include <wlr/backend/qnx_screen.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/render/drm_format_set.h>

#include "backend/qnx_screen_event_monitor.h"

#define QNX_SCREEN_WINDOW_MIN_SIZE 128
#define QNX_SCREEN_WINDOW_MAX_SIZE 8192
#define QNX_SCREEN_DEFAULT_REFRESH (60 * 1000) // 60 Hz, in mHz
#define QNX_SCREEN_NUM_BUFFERS 2

struct wlr_qnx_screen_backend {
	struct wlr_backend backend;
	struct wl_event_loop *event_loop;
	bool started;

	screen_context_t context;
	screen_display_t default_display;

	int drm_fd; // -1 if unavailable; see open_drm_render_node() in backend.c
	int gbm_fd; // dup() of drm_fd, owned by gbm_device below
	struct gbm_device *gbm_device; // NULL until first successful use, or forever if unavailable
	bool gbm_device_init_attempted; // set on first qnx_screen_backend_ensure_gbm_device() call, success or not

	struct wl_event_source *screen_source;
	struct wlr_qnx_screen_event_monitor *event_monitor;

	struct wl_list outputs; // wlr_qnx_screen_output.link

	struct wlr_keyboard keyboard;
	int left_gui_state;
	int right_gui_state;
	int prev_screen_modifiers;

	struct wlr_drm_format_set shm_formats;
	// Only meaningful (and only ever populated) when drm_fd >= 0: XRGB8888
	// with an explicit LINEAR modifier, so dmabuf-backed output buffers stay
	// CPU-mmap-able for the copy into the QNX Screen window's own buffer --
	// see copy_buffer_to_window() in output.c.
	struct wlr_drm_format_set dmabuf_formats;

	struct wl_listener event_loop_destroy;
};

struct wlr_qnx_screen_output {
	struct wlr_output wlr_output;
	struct wlr_qnx_screen_backend *backend;
	struct wl_list link; // wlr_qnx_screen_backend.outputs

	screen_window_t window;
	screen_session_t session;
	screen_display_t display;

	screen_buffer_t buffers[QNX_SCREEN_NUM_BUFFERS];
	size_t n_buffers;
	size_t buffer_index;

	int32_t win_width, win_height;
	int32_t x, y;
	bool fullscreen;
	int requested_display_id;

	int32_t frame_delay_ms;
	struct wl_event_source *frame_timer;

	struct wlr_pointer pointer;
	uint32_t prev_buttons;

	struct wlr_touch touch;
	struct wl_list touchpoints; // wlr_qnx_screen_touchpoint.link
};

struct wlr_qnx_screen_touchpoint {
	int screen_id;
	int wayland_id;
	struct wl_list link; // wlr_qnx_screen_output.touchpoints
};

struct wlr_qnx_screen_backend *qnx_screen_backend_from_backend(
	struct wlr_backend *wlr_backend);
struct wlr_qnx_screen_output *qnx_screen_output_from_window(
	struct wlr_qnx_screen_backend *backend, screen_window_t window);

extern const struct wlr_keyboard_impl qnx_screen_keyboard_impl;
extern const struct wlr_pointer_impl qnx_screen_pointer_impl;
extern const struct wlr_touch_impl qnx_screen_touch_impl;

// Lazily creates (on first call) and returns backend->gbm_device, or NULL if
// unavailable/failed. Deliberately NOT created eagerly at backend creation
// time -- see the comment on backend_get_drm_fd() in backend.c for why.
// Safe to call repeatedly; only actually attempts creation once.
struct gbm_device *qnx_screen_backend_ensure_gbm_device(
	struct wlr_qnx_screen_backend *backend);

bool qnx_screen_output_create_window(struct wlr_qnx_screen_output *output);
void qnx_screen_output_destroy_window(struct wlr_qnx_screen_output *output);
bool qnx_screen_output_recreate_buffers(struct wlr_qnx_screen_output *output);

void qnx_screen_handle_pointer_event(struct wlr_qnx_screen_backend *backend,
	screen_event_t event);
void qnx_screen_handle_keyboard_event(struct wlr_qnx_screen_backend *backend,
	screen_event_t event);
void qnx_screen_handle_touch_event(struct wlr_qnx_screen_backend *backend,
	screen_event_t event, int type);
void qnx_screen_handle_property_event(struct wlr_qnx_screen_backend *backend,
	screen_event_t event);
void qnx_screen_sync_modifiers(struct wlr_qnx_screen_backend *backend,
	int screen_modifiers);

#endif

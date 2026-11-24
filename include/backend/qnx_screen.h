#ifndef BACKEND_QNX_SCREEN_H
#define BACKEND_QNX_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include <screen/screen.h>

#include <wayland-server-core.h>

#include <wlr/backend/qnx_screen.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/render/drm_format_set.h>

#include "backend/qnx_screen_event_monitor.h"

struct wlr_renderer;

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

	// -1 if unavailable; see open_drm_render_node() in backend.c. Reported to
	// the compositor so wlr_allocator_autocreate() can pick a GBM allocator for
	// client buffers; this backend never creates a gbm_device of its own.
	int drm_fd;

	struct wl_event_source *screen_source;
	struct wlr_qnx_screen_event_monitor *event_monitor;

	struct wl_list outputs; // wlr_qnx_screen_output.link

	struct wlr_keyboard keyboard;
	int left_gui_state;
	int right_gui_state;
	int prev_screen_modifiers;

	struct wlr_drm_format_set shm_formats;
	// The swapchain formats offered to the compositor, keyed off
	// output_get_primary_formats(): SHM for the pixman renderer, and GBM
	// DMA-BUF for the GLES2 one (which can only render into a DMA-BUF).
	// Populated when a DRM render node is available; see open_drm_render_node().
	struct wlr_drm_format_set dmabuf_formats;

	struct wl_listener event_loop_destroy;
};

struct wlr_qnx_screen_vsync_notifier;
struct wlr_qnx_gl_present;

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

	/*
	 * Display-locked frame pacing, when the display supports it. NULL means
	 * this output paces purely from frame_timer. See vsync_notifier.c.
	 */
	struct wlr_qnx_screen_vsync_notifier *vsync;
	struct wl_event_source *vsync_source;

	/*
	 * Guards against the vsync notifier going quiet: every commit rearms a
	 * timer with the full period, and that timer only emits a frame event if
	 * the vsync tick has not arrived in the meantime. Losing frame events
	 * stalls an output permanently, so the timer is never disabled while
	 * vsync pacing is active -- it is a watchdog as well as a fallback.
	 */
	bool frame_event_pending;

	/*
	 * Both present paths copy the finished frame into one of this window's
	 * own render buffers and hand it to screen_post_window(). Which copy is
	 * used follows the renderer: copy_buffer_to_window_gles() reads a GLES2
	 * frame back with glReadPixels, copy_buffer_to_window_cpu() memcpy's a
	 * pixman frame straight out of its buffer.
	 */

	/*
	 * Staging for the GLES2 read-back, sized on first use and freed in
	 * output_destroy(). Large: one 1080p frame is ~8MB.
	 */
	void *readback_px;
	size_t readback_cap;

	/*
	 * Set once a full frame has been written into every window buffer, so the
	 * buffer holding the previous frame is safe to copy undamaged areas from.
	 * Cleared whenever the window buffers are (re)created.
	 */
	bool baseline_valid;

	/*
	 * When the last frame event was emitted (zeroed if never). The fake vsync
	 * pacer measures the frame period from here, so it can charge the
	 * compositor's render time against the period as well as our own commit.
	 */
	struct timespec frame_event_at;

	/* GPU-side present (see gl_present.c). This is only the "it failed, never
	 * try again" latch: whether the present is wanted is derived from the
	 * renderer at window-creation time (qnx_screen_output_wants_gl_window),
	 * because the renderer is not known when the output is created and a stored
	 * default would misconfigure a non-GLES2 output's window usage. */
	struct wlr_qnx_gl_present *gl_present;
	bool gl_present_failed;
	/* Whether the window currently exists was created with GL buffer usage,
	 * i.e. whether its render buffers are writable by GL (and have no
	 * SCREEN_PROPERTY_POINTER). The present path and this flag have to agree;
	 * when they stop agreeing the window is rebuilt. */
	bool window_is_gl_backed;

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

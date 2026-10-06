/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_BACKEND_QNX_SCREEN_H
#define WLR_BACKEND_QNX_SCREEN_H

#include <stdbool.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>

struct wlr_input_device;

/**
 * Creates a new QNX Screen backend.
 *
 * This backend is created with no outputs; use wlr_qnx_screen_output_create()
 * to add one. Outputs correspond 1:1 to QNX Screen windows (screen_window_t)
 * created by this backend via the QNX Screen Graphics Subsystem
 * (libscreen / screen/screen.h).
 *
 * This backend has no notion of DRM/GBM: wlr_backend_get_drm_fd() always
 * returns -1, and the backend only advertises WLR_BUFFER_CAP_SHM. Pair it
 * with wlr_renderer_autocreate()/wlr_allocator_autocreate(), which will fall
 * back to the pixman (CPU) renderer and the generic shared-memory allocator
 * automatically.
 */
struct wlr_backend *wlr_qnx_screen_backend_create(struct wl_event_loop *loop);

/**
 * Adds a new output (QNX Screen window) to this backend.
 *
 * If called before the backend has been started, this returns NULL and the
 * output is created once wlr_backend_start() runs, delivered via the
 * backend's new_output event.
 */
struct wlr_output *wlr_qnx_screen_output_create(struct wlr_backend *backend);

/**
 * Check whether this backend is a QNX Screen backend.
 */
bool wlr_backend_is_qnx_screen(const struct wlr_backend *backend);

/**
 * Check whether this output is a QNX Screen output.
 */
bool wlr_output_is_qnx_screen(const struct wlr_output *output);

/**
 * Check whether this input device was created by a QNX Screen backend.
 */
bool wlr_input_device_is_qnx_screen(struct wlr_input_device *device);

/**
 * Sets whether the backend's outputs run full-screen on their assigned
 * QNX Screen display (as opposed to a movable, positionable window).
 *
 * Must be called before the output is enabled (i.e. before the first
 * WLR_OUTPUT_STATE_ENABLED commit).
 */
bool wlr_qnx_screen_output_set_fullscreen(struct wlr_output *output,
	bool fullscreen);

/**
 * Positions a (non-fullscreen) output's window on its QNX Screen display.
 */
bool wlr_qnx_screen_output_set_position(struct wlr_output *output,
	int32_t x, int32_t y);

/**
 * Assigns a specific QNX Screen display (by SCREEN_PROPERTY_ID) to an
 * output. Pass 0 to use the default/first display reported by the context.
 */
bool wlr_qnx_screen_output_set_display(struct wlr_output *output,
	int display_id);

#endif


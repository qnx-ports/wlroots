#include <assert.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <screen/screen.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#include "backend/qnx_screen.h"
#include "backend/qnx_screen_gl_present.h"
#include "backend/qnx_screen_vsync_notifier.h"
#include "types/wlr_buffer.h"
#include "types/wlr_output.h"



static const uint32_t SUPPORTED_OUTPUT_STATE =
	WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
	WLR_OUTPUT_STATE_BUFFER |
	WLR_OUTPUT_STATE_ENABLED |
	WLR_OUTPUT_STATE_MODE;

static size_t last_output_num = 0;

static struct wlr_qnx_screen_output *output_from_wlr_output(
		struct wlr_output *wlr_output) {
	assert(wlr_output_is_qnx_screen(wlr_output));
	struct wlr_qnx_screen_output *output =
		wl_container_of(wlr_output, output, wlr_output);
	return output;
}

/*
 * Window / native buffer lifecycle
 * ---------------------------------
 * A QNX Screen window (screen_window_t) is a native compositor resource:
 * once created, resizing it means re-requesting SCREEN_PROPERTY_SIZE /
 * SCREEN_PROPERTY_BUFFER_SIZE and re-creating its render buffers.
 *
 * The finished frame is always copied into one of the window's own render
 * buffers and handed to screen_post_window(). Which copy is used follows the
 * renderer: glReadPixels for GLES2 (its frame is a DMA-BUF that the CPU cannot
 * touch directly), a plain memcpy for pixman (its frame is already CPU memory).
 *
 * An EGL window surface on the screen_window_t -- which would avoid the copy
 * entirely -- is worth knowing about but does not work here, so it is not
 * attempted. Two things have to be true for it, and only one ever is:
 *
 *  - eglCreateWindowSurface() needs an EGLConfig with EGL_ALPHA_SIZE 8. With
 *    0 it fails EGL_BAD_NATIVE_WINDOW whatever the window's usage flags or
 *    SCREEN_PROPERTY_FORMAT are; matching the window's RGBX8888 by asking for
 *    no alpha is exactly the wrong thing to do. This is easy to misread as the
 *    driver being incapable, so it is recorded here.
 *  - the driver then has to composite what it swaps. On virtio/virgl it does
 *    not: the surface is created, glReadPixels on its back buffer returns the
 *    new frame, eglSwapBuffers() returns success (repeatedly), and the panel
 *    never changes.
 */

bool qnx_screen_output_recreate_buffers(struct wlr_qnx_screen_output *output) {
	int size[] = { output->win_width, output->win_height };

	if (screen_set_window_property_iv(output->window,
			SCREEN_PROPERTY_SIZE, size) != 0) {
		wlr_log(WLR_ERROR, "screen_set_window_property_iv(SIZE) failed");
		return false;
	}
	if (screen_set_window_property_iv(output->window,
			SCREEN_PROPERTY_BUFFER_SIZE, size) != 0) {
		wlr_log(WLR_ERROR, "screen_set_window_property_iv(BUFFER_SIZE) failed");
		return false;
	}
	if (output->session != NULL) {
		screen_set_session_property_iv(output->session,
			SCREEN_PROPERTY_SIZE, size);
	}

	if (screen_create_window_buffers(output->window,
			QNX_SCREEN_NUM_BUFFERS) != 0) {
		wlr_log(WLR_ERROR, "screen_create_window_buffers failed");
		return false;
	}

	/* Any GL imports of the old buffers describe memory that is gone; they are
	 * re-created on the next present. */
	qnx_gl_present_invalidate(output);

	memset(output->buffers, 0, sizeof(output->buffers));
	if (screen_get_window_property_pv(output->window,
			SCREEN_PROPERTY_RENDER_BUFFERS, (void **)output->buffers) != 0) {
		wlr_log(WLR_ERROR,
			"screen_get_window_property_pv(RENDER_BUFFERS) failed");
		return false;
	}
	output->n_buffers = QNX_SCREEN_NUM_BUFFERS;
	// Fresh buffers hold undefined pixels, so there is nothing to fill
	// undamaged areas from until the first full frame has been posted.
	output->baseline_valid = false;
	output->buffer_index = 0;

	return true;
}

// Create a window sized/positioned as the output wants, with the given Screen
// buffer usage flags. Returns false if the window could not be created.
static bool output_configure_window(struct wlr_qnx_screen_output *output,
		int usage) {
	struct wlr_qnx_screen_backend *b = output->backend;

	if (screen_create_window(&output->window, b->context) != 0) {
		wlr_log(WLR_ERROR, "screen_create_window failed");
		return false;
	}

	screen_display_t display =
		output->display != NULL ? output->display : b->default_display;
	if (display != NULL) {
		screen_set_window_property_pv(output->window,
			SCREEN_PROPERTY_DISPLAY, (void **)&display);
	}

	if (output->fullscreen && display != NULL) {
		int display_size[] = { output->win_width, output->win_height };
		screen_get_display_property_iv(display, SCREEN_PROPERTY_SIZE,
			display_size);

		output->win_width = display_size[0];
		output->win_height = display_size[1];
		output->x = 0;
		output->y = 0;
	}

	// Usage must be set before the window has any buffers. On the
	// virtio/virgl driver changing it afterwards leaves
	// screen_create_window_buffers() failing with ENOTSUP for the rest of the
	// window's life, so this is the only place it is ever set.
	int u[] = { usage };
	if (screen_set_window_property_iv(output->window, SCREEN_PROPERTY_USAGE,
			u) != 0) {
		wlr_log(WLR_ERROR, "screen_set_window_property_iv(USAGE) failed");
		return false;
	}

	int format[] = { SCREEN_FORMAT_RGBX8888 };
	screen_set_window_property_iv(output->window, SCREEN_PROPERTY_FORMAT, format);

	int position[] = { output->x, output->y };
	screen_set_window_property_iv(output->window, SCREEN_PROPERTY_POSITION,
		position);

	int size[] = { output->win_width, output->win_height };
	// Deliberately does NOT set SCREEN_PROPERTY_BUFFER_COUNT: on the
	// virtio/virgl driver that leaves the window unable to create any buffers
	// at all (ENOTTY), and Mesa's Screen platform sets it itself.
	screen_set_window_property_iv(output->window, SCREEN_PROPERTY_SIZE, size);
	screen_set_window_property_iv(output->window, SCREEN_PROPERTY_BUFFER_SIZE,
		size);
	return true;
}

/* Make the output's window exist and be backed the way the present path needs.
 * A GL present needs GL buffer usage (only then do GL writes to a window render
 * buffer reach the panel); the CPU read-back needs the opposite, because a
 * GL-usage window has no SCREEN_PROPERTY_POINTER to copy into. Screen cannot
 * change a window's usage once it has buffers, so a mismatch means a new
 * window. Returns false only if the window could not be created. */
static bool output_ensure_window_backing(struct wlr_qnx_screen_output *output,
		bool want_gl) {
	if (output->window != NULL && output->window_is_gl_backed == want_gl) {
		return true;
	}
	if (output->window != NULL) {
		qnx_screen_output_destroy_window(output);
	}
	return qnx_screen_output_create_window(output);
}

bool qnx_screen_output_create_window(struct wlr_qnx_screen_output *output) {
	struct wlr_qnx_screen_backend *b = output->backend;

	/* A GL write into a window render buffer only reaches the panel if the
	 * window was created with a GL usage bit; with NATIVE|READ|WRITE the buffer
	 * has a CPU view and that is what gets composited. Conversely a GL-usage
	 * window has no SCREEN_PROPERTY_POINTER, so the CPU read-back path cannot
	 * write it. The usage therefore has to match the present path chosen for
	 * this output, and it must be set before the window has any buffers (see
	 * output_configure_window). */
	bool gl_backed = qnx_screen_output_wants_gl_window(output);
	int usage = gl_backed
		? (SCREEN_USAGE_NATIVE | SCREEN_USAGE_OPENGL_ES2)
		: (SCREEN_USAGE_NATIVE | SCREEN_USAGE_READ | SCREEN_USAGE_WRITE);
	output->window_is_gl_backed = gl_backed;

	if (!output_configure_window(output, usage)) {
		if (output->window != NULL) {
			screen_destroy_window(output->window);
			output->window = NULL;
		}
		return false;
	}

	if (!qnx_screen_output_recreate_buffers(output)) {
		screen_destroy_window(output->window);
		output->window = NULL;
		return false;
	}

	if (screen_create_session_type(&output->session, b->context,
			SCREEN_EVENT_POINTER) == 0) {
		screen_set_session_property_pv(output->session, SCREEN_PROPERTY_WINDOW,
			(void **)&output->window);
		int size[] = { output->win_width, output->win_height };
		screen_set_session_property_iv(output->session, SCREEN_PROPERTY_SIZE,
			size);
		int cursor[] = { SCREEN_CURSOR_SHAPE_NONE };
		screen_set_session_property_iv(output->session, SCREEN_PROPERTY_CURSOR,
			cursor);
	} else {
		wlr_log(WLR_ERROR,
			"screen_create_session_type failed; input on this output "
			"may not work");
		output->session = NULL;
	}

	wlr_log(WLR_INFO, "qnx-screen present path: %s",
		gl_backed ? "GPU blit into the window buffer (GL-backed window)"
		: wlr_renderer_is_gles2(output->wlr_output.renderer)
			? "CPU read-back of the DMA-BUF (CPU-backed window)"
			: "CPU copy of the SHM buffer (CPU-backed window)");

	int window_id = 0;
	screen_get_window_property_iv(output->window, SCREEN_PROPERTY_ID,
		&window_id);
	wlr_log(WLR_DEBUG, "QNX Screen output %dx%d, window id %d",
		output->win_width, output->win_height, window_id);

	return true;
}

void qnx_screen_output_destroy_window(struct wlr_qnx_screen_output *output) {
	output->window_is_gl_backed = false;
	if (output->session != NULL) {
		screen_destroy_session(output->session);
		output->session = NULL;
	}
	if (output->window != NULL) {
		screen_destroy_window(output->window);
		output->window = NULL;
	}
	output->n_buffers = 0;
}

static bool output_test(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
	if (unsupported != 0) {
		wlr_log(WLR_DEBUG, "Unsupported output state fields: 0x%"PRIx32,
			unsupported);
		return false;
	}

	if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
		int pending_width, pending_height;
		output_pending_resolution(wlr_output, state,
			&pending_width, &pending_height);
		if (state->buffer->width != pending_width ||
				state->buffer->height != pending_height) {
			wlr_log(WLR_DEBUG, "Primary buffer size mismatch");
			return false;
		}

		struct wlr_fbox src_box;
		output_state_get_buffer_src_box(state, &src_box);
		if (src_box.x != 0.0 || src_box.y != 0.0 ||
				src_box.width != (double)state->buffer->width ||
				src_box.height != (double)state->buffer->height) {
			wlr_log(WLR_DEBUG, "Source crop not supported by QNX Screen output");
			return false;
		}

		if (buffer_get_drm_format(state->buffer) != DRM_FORMAT_XRGB8888) {
			wlr_log(WLR_DEBUG, "Unsupported buffer format");
			return false;
		}

		// What counts as a usable buffer depends on how the frame is read
		// out of it, which depends on the renderer.
		if (!wlr_renderer_is_gles2(wlr_output->renderer)) {
			// The frame is memcpy'd straight out of the buffer's own memory,
			// so it has to be CPU-backed.
			struct wlr_shm_attributes shm;
			if (!wlr_buffer_get_shm(state->buffer, &shm)) {
				wlr_log(WLR_DEBUG, "QNX Screen output needs a shared-memory "
					"buffer when the renderer is not GLES2");
				return false;
			}
		}
		// With GLES2 any buffer the renderer can turn into a texture works;
		// wlr_texture_from_buffer() fails for the rest, and a tiling or
		// multi-plane layout is no longer a problem because the GL driver
		// does the read-back instead of a CPU walking the bytes.
	}

	if (state->committed & WLR_OUTPUT_STATE_MODE && state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
		if (state->custom_mode.width < QNX_SCREEN_WINDOW_MIN_SIZE ||
				state->custom_mode.width > QNX_SCREEN_WINDOW_MAX_SIZE ||
				state->custom_mode.height < QNX_SCREEN_WINDOW_MIN_SIZE ||
				state->custom_mode.height > QNX_SCREEN_WINDOW_MAX_SIZE) {
			wlr_log(WLR_DEBUG, "Unsupported mode size");
			return false;
		}
	}

	return true;
}

static void emit_frame_event(struct wlr_qnx_screen_output *output) {
	if (!output->frame_event_pending) {
		// Already emitted for this frame, by whichever of the vsync tick and
		// the pacing timer got here first.
		return;
	}
	output->frame_event_pending = false;
	/* Record when the frame event goes out: the compositor's render and the
	 * resulting commit both happen after this, so it is the start of the frame
	 * period the pacer in output_commit() measures against. */
	clock_gettime(CLOCK_MONOTONIC, &output->frame_event_at);
	wlr_output_send_frame(&output->wlr_output);
}

static int finish_frame_handler(void *data) {
	struct wlr_qnx_screen_output *output = data;
	emit_frame_event(output);
	/*
	 * The timer won this frame, so a vsync tick may still be sitting in the
	 * pipe: the notifier consumed its arm to write it, and nothing will read
	 * it now. Left there, it would make the fd readable at the next commit
	 * and emit that frame immediately, off vsync. Discard it.
	 */
	if (output->vsync != NULL) {
		wlr_qnx_screen_vsync_notifier_drain(output->vsync);
	}
	return 0;
}

static int vsync_handler(int fd, uint32_t mask, void *data) {
	struct wlr_qnx_screen_output *output = data;
	(void)fd; (void)mask;

	wlr_qnx_screen_vsync_notifier_drain(output->vsync);

	if (wlr_qnx_screen_vsync_notifier_failed(output->vsync)) {
		// Never fires again; leave the pacing timer in sole charge.
		wlr_log(WLR_INFO, "qnx-screen: vsync pacing unavailable, "
			"using the timer");
		return 0;
	}

	/*
	 * The frame event is being emitted off the display's own vsync, so cancel
	 * the timer that was armed for this frame; leaving it armed would emit a
	 * second event and let the compositor run ahead of the panel.
	 *
	 * No re-arm here on purpose. Commit() arms once per frame, which is how a
	 * page-flip backend behaves (one completion per flip): while the
	 * compositor has nothing to render no tick is requested, so the event loop
	 * is not woken at refresh rate for no work.
	 */
	if (output->frame_event_pending
			&& wl_event_source_timer_update(output->frame_timer, 0) != 0) {
		wlr_log(WLR_ERROR, "qnx-screen: failed to cancel frame timer");
	}
	emit_frame_event(output);
	return 0;
}


// GLES2 present path.
//
// The frame was rendered by the GPU into a DMA-BUF, and a Screen window buffer
// is plain CPU memory, so the pixels have to cross from one to the other. This
// asks the GL driver for them (wlr_texture_read_pixels(), the same path
// wlroots' own screenshots use), which costs ~16ms per 1080p frame on
// virtio/virgl.
//
// The alternative, gbm_bo_map(), was measured at ~36ms for the identical frame:
// the buffer allocator's read-back transfer is roughly 2.2x the driver's own,
// which is what capped this backend near 19fps versus the ~31fps here.
//
// The read-back is limited to the committed damage, because it is by far the
// most expensive step and it scales with area (~1.2 GB/s measured). Only the
// damaged boxes are read out of GL; the rest of the window buffer is filled on
// the CPU from the buffer that was posted last frame, which holds exactly the
// frame being updated. A full-frame CPU memcpy is ~1.5ms against ~16ms for the
// same pixels through the driver, so filling everything and then overwriting
// the damage is cheaper than computing the undamaged complement, and much
// harder to get wrong.
static bool copy_buffer_to_window_gles(struct wlr_qnx_screen_output *output,
		struct wlr_buffer *buffer, const pixman_region32_t *damage) {
	struct wlr_renderer *wlr_renderer = output->wlr_output.renderer;

	size_t row = (size_t)buffer->width * 4;
	size_t need = row * buffer->height;
	// Grow-only: resizing the output to something larger reallocates, but
	// shrinking keeps the (oversized) allocation, which output_destroy() frees.
	if (need > output->readback_cap) {
		free(output->readback_px);
		output->readback_px = malloc(need);
		output->readback_cap =
			output->readback_px ? need : 0;
		if (output->readback_px == NULL) {
			return false;
		}
	}

	struct wlr_texture *tex = wlr_texture_from_buffer(wlr_renderer, buffer);
	if (tex == NULL) {
		wlr_log(WLR_ERROR, "qnx-screen: cannot import output buffer as a "
			"texture");
		return false;
	}
	screen_buffer_t native = output->buffers[output->buffer_index];
	void *dst = NULL;
	int dst_stride = 0;
	screen_get_buffer_property_pv(native, SCREEN_PROPERTY_POINTER, &dst);
	screen_get_buffer_property_iv(native, SCREEN_PROPERTY_STRIDE, &dst_stride);
	if (dst == NULL || dst_stride <= 0) {
		wlr_log(WLR_ERROR, "qnx-screen: no access to the window buffer");
		wlr_texture_destroy(tex);
		return false;
	}

	// Fill the areas the damage does not cover from the buffer that is
	// currently on screen. That buffer holds the frame we are updating, so
	// this leaves the target holding a complete, correct frame before the
	// damaged boxes are painted over it -- which matters because Screen keeps
	// the whole window buffer and may recomposite all of it later, not just the
	// rectangles handed to screen_post_window().
	//
	// It is safe to read: the buffer we are writing is by construction not the
	// one being displayed (that is what the rotation in output_post_window()
	// is for), and the one being read is only ever written by us.
	int prev = (output->buffer_index + output->n_buffers - 1)
		% output->n_buffers;
	void *src_prev = NULL;
	int src_prev_stride = 0;
	if (output->baseline_valid && output->n_buffers > 1) {
		screen_get_buffer_property_pv(output->buffers[prev],
			SCREEN_PROPERTY_POINTER, &src_prev);
		screen_get_buffer_property_iv(output->buffers[prev],
			SCREEN_PROPERTY_STRIDE, &src_prev_stride);
	}

	bool partial = false;
	int box_count = 0;
	pixman_box32_t *boxes = NULL;
	if (src_prev != NULL && src_prev_stride > 0 && damage != NULL) {
		boxes = pixman_region32_rectangles((pixman_region32_t *)damage,
			&box_count);
		partial = box_count > 0;
	}

	if (partial) {
		size_t fill = row < (size_t)dst_stride ? row : (size_t)dst_stride;
		if (fill > (size_t)src_prev_stride) {
			fill = (size_t)src_prev_stride;
		}
		for (int y = 0; y < buffer->height; y++) {
			memcpy((uint8_t *)dst + (size_t)y * dst_stride,
				(const uint8_t *)src_prev + (size_t)y * src_prev_stride,
				fill);
		}
	}

	// No vertical flip: measured on target with a RED-top/BLUE-BOTTOM client,
	// wlr_texture_read_pixels() already yields the buffer top-down, and adding
	// the flip that glReadPixels' bottom-up origin suggests puts the panel
	// upside down. That was measured full-frame; see the note below on why a
	// sub-box is safe.
	size_t copy_bytes = row < (size_t)dst_stride ? row : (size_t)dst_stride;

	if (!partial) {
		struct wlr_texture_read_pixels_options opts = {
			.data = output->readback_px,
			.format = DRM_FORMAT_XRGB8888,
			.stride = (uint32_t)row,
			.src_box = { .width = buffer->width, .height = buffer->height },
		};
		bool ok = wlr_texture_read_pixels(tex, &opts);
		if (!ok) {
			wlr_log(WLR_ERROR, "qnx-screen: glReadPixels read-back failed");
		}
		for (int y = 0; ok && y < buffer->height; y++) {
			memcpy((uint8_t *)dst + (size_t)y * dst_stride,
				(const uint8_t *)output->readback_px + (size_t)y * row,
				copy_bytes);
		}
		wlr_texture_destroy(tex);
		if (ok) {
			output->baseline_valid = true;
		}
		return ok;
	}

	// One box at a time. With stride == width*4 and dst_x == 0 the GLES2
	// backend takes its single-glReadPixels path rather than reading row by
	// row (see gles2_texture_read_pixels()).
	for (int i = 0; i < box_count; i++) {
		int x1 = boxes[i].x1, y1 = boxes[i].y1;
		int x2 = boxes[i].x2, y2 = boxes[i].y2;
		if (x1 < 0) x1 = 0;
		if (y1 < 0) y1 = 0;
		if (x2 > buffer->width) x2 = buffer->width;
		if (y2 > buffer->height) y2 = buffer->height;
		if (x2 <= x1 || y2 <= y1) {
			continue;
		}
		int bw = x2 - x1, bh = y2 - y1;
		size_t brow = (size_t)bw * 4;

		struct wlr_texture_read_pixels_options opts = {
			.data = output->readback_px,
			.format = DRM_FORMAT_XRGB8888,
			.stride = (uint32_t)brow,
			.src_box = { .x = x1, .y = y1, .width = bw, .height = bh },
		};
		if (!wlr_texture_read_pixels(tex, &opts)) {
			wlr_log(WLR_ERROR, "qnx-screen: glReadPixels read-back failed "
				"for damage box %d,%d %dx%d", x1, y1, bw, bh);
			wlr_texture_destroy(tex);
			return false;
		}
		size_t bcopy = brow < (size_t)dst_stride ? brow : (size_t)dst_stride;
		for (int y = 0; y < bh; y++) {
			memcpy((uint8_t *)dst + (size_t)(y1 + y) * dst_stride + (size_t)x1 * 4,
				(const uint8_t *)output->readback_px + (size_t)y * brow,
				bcopy);
		}
	}

	output->baseline_valid = true;
	wlr_texture_destroy(tex);
	return true;
}

// Pixman present path.
//
// The frame was rendered on the CPU, so the buffer already is CPU memory the
// memcpy below can read directly -- there is no transfer to avoid and nothing
// to read back.
static bool copy_buffer_to_window_cpu(struct wlr_qnx_screen_output *output,
		struct wlr_buffer *buffer) {
	void *src;
	uint32_t format;
	size_t src_stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &src, &format, &src_stride)) {
		wlr_log(WLR_ERROR, "qnx-screen: pixman output buffer is not "
			"CPU-readable; the generic allocator must be used with pixman");
		return false;
	}

	screen_buffer_t native = output->buffers[output->buffer_index];
	void *dst = NULL;
	int dst_stride = 0;
	screen_get_buffer_property_pv(native, SCREEN_PROPERTY_POINTER, &dst);
	screen_get_buffer_property_iv(native, SCREEN_PROPERTY_STRIDE, &dst_stride);
	if (dst == NULL || dst_stride <= 0) {
		wlr_log(WLR_ERROR, "qnx-screen: no access to the window buffer");
		wlr_buffer_end_data_ptr_access(buffer);
		return false;
	}

	size_t row_bytes = (size_t)buffer->width * 4; // XRGB8888 is always 4 bpp
	size_t copy_bytes = row_bytes < (size_t)dst_stride ? row_bytes : (size_t)dst_stride;
	if (copy_bytes > src_stride) {
		copy_bytes = src_stride;
	}

	for (int y = 0; y < buffer->height; y++) {
		memcpy((uint8_t *)dst + (size_t)y * dst_stride,
			(const uint8_t *)src + (size_t)y * src_stride, copy_bytes);
	}

	wlr_buffer_end_data_ptr_access(buffer);
	return true;
}

/*
 * Hand the window buffer that was just filled (by whichever path) to Screen,
 * then rotate to the next one. screen_post_window() takes dirty rectangles in
 * the same top-down window coordinate system that both present paths fill the
 * buffers in.
 */
static bool output_post_window(struct wlr_qnx_screen_output *output,
		const struct wlr_output_state *state) {
	if (output->n_buffers == 0) {
		return false;
	}

	screen_buffer_t native = output->buffers[output->buffer_index];

	int nrects = 0;
	int *rects = NULL;
	int full_rect[4] = { 0, 0, output->win_width, output->win_height };

	if (state->committed & WLR_OUTPUT_STATE_DAMAGE) {
		int box_count = 0;
		pixman_box32_t *boxes =
			pixman_region32_rectangles((pixman_region32_t *)&state->damage,
				&box_count);
		if (box_count > 0) {
			rects = calloc((size_t)box_count, 4 * sizeof(int));
			if (rects != NULL) {
				for (int i = 0; i < box_count; i++) {
					rects[i * 4 + 0] = boxes[i].x1;
					rects[i * 4 + 1] = boxes[i].y1;
					rects[i * 4 + 2] = boxes[i].x2;
					rects[i * 4 + 3] = boxes[i].y2;
				}
				nrects = box_count;
			}
		}
	}

	if (rects == NULL) {
		rects = full_rect;
		nrects = 1;
	}

	int ret = screen_post_window(output->window, native, nrects, rects, 0);
	int err = errno;

	if (rects != full_rect) {
		free(rects);
	}

	if (ret != 0) {
		wlr_log(WLR_ERROR, "screen_post_window failed: %s", strerror(err));
		return false;
	}

	output->buffer_index = (output->buffer_index + 1) % output->n_buffers;
	return true;
}

static bool output_commit(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	struct wlr_qnx_screen_output *output = output_from_wlr_output(wlr_output);

	if (!output_test(wlr_output, state)) {
		return false;
	}

	bool pending_enabled = output_pending_enabled(wlr_output, state);

	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		int32_t width = state->custom_mode.width;
		int32_t height = state->custom_mode.height;
		if (width != output->win_width || height != output->win_height) {
			output->win_width = width;
			output->win_height = height;
			if (output->window != NULL) {
				// Screen window buffers cannot be resized in place, so
				// rebuild the window at the new size.
				qnx_screen_output_destroy_window(output);
				if (!qnx_screen_output_create_window(output)) {
					return false;
				}
			}

			int32_t refresh = state->custom_mode.refresh;
			output->frame_delay_ms =
				refresh > 0 ? 1000000 / refresh : 1000 / 60;
		}
	}

	if (wlr_output->enabled && !pending_enabled) {
		qnx_screen_output_destroy_window(output);
	} else if (pending_enabled && output->window == NULL) {
		if (!qnx_screen_output_create_window(output)) {
			return false;
		}
	}

	/*
	 * When the fake vsync starts, so the delay below can be charged against
	 * the frame instead of added on top of it. See the pacer note at the
	 * bottom of this function.
	 */
	if ((state->committed & WLR_OUTPUT_STATE_BUFFER) && output->window != NULL) {
		// Which copy depends only on the renderer: GLES2 rendered the frame on
		// the GPU into a DMA-BUF, pixman rendered it on the CPU and can be
		// memcpy'd straight out of its buffer. With GLES2 the frame can stay on
		// the GPU (see gl_present.c) and only falls back to the read-back when
		// that path declines a frame.
		bool ok = false;
		const pixman_region32_t *frame_damage =
			(state->committed & WLR_OUTPUT_STATE_DAMAGE) ? &state->damage : NULL;

		if (wlr_renderer_is_gles2(wlr_output->renderer)) {
			/* Created lazily because the renderer is not known when the
			 * output is; only allocated when it will actually be used. */
			if (output->gl_present == NULL
					&& qnx_screen_output_wants_gl_window(output)) {
				output->gl_present = wlr_qnx_gl_present_create(output);
			}

			if (output->gl_present != NULL
					&& qnx_screen_output_wants_gl_window(output)) {
				if (output_ensure_window_backing(output, true)) {
					ok = wlr_qnx_gl_present(output, state->buffer);
				}
				if (!ok) {
					/* Retire it for good: a failure here is a property of this
					 * driver or window, not of one frame. Note it does not
					 * touch the window; the next branch rebuilds it. */
					wlr_qnx_gl_present_disable_and_rebuild(output);
				}
			}

			if (!ok) {
				if (!output_ensure_window_backing(output, false)) {
					return false;
				}
				ok = copy_buffer_to_window_gles(output, state->buffer,
					frame_damage);
			}
		} else {
			if (!output_ensure_window_backing(output, false)) {
				return false;
			}
			ok = copy_buffer_to_window_cpu(output, state->buffer);
		}
		if (!ok || !output_post_window(output, state)) {
			return false;
		}
	}

	if (pending_enabled) {
		struct wlr_output_event_present present_event = {
			.commit_seq = wlr_output->commit_seq + 1,
			.presented = true,
		};
		output_defer_present(wlr_output, present_event);

		/*
		 * Frame pacing. wlroots expects an output to emit a frame event once
		 * per display refresh, and that event is what releases the next frame
		 * through the compositor; an output that stops emitting it stalls. A
		 * DRM backend gets this for free from the page-flip completion the
		 * kernel reports. A Screen window has no such completion: post_window
		 * returns as soon as the buffer is queued (measured 0.04ms), so the
		 * cadence has to be manufactured from something else, in two layers:
		 *
		 *  - a thread parked in screen_wait_vsync() (vsync_notifier.c), which
		 *    is the only display-locked source here and wins whenever it
		 *    fires -- about 95% of frames measured on virtio/virgl;
		 *  - this timer, as the fallback whenever vsync is unavailable (it
		 *    fails with ENOTSUP on a mirrored display) or a tick is missed.
		 *    Losing frame events stalls an output permanently, so the timer
		 *    is armed for every frame and is never disabled.
		 *
		 * screen_notify(SCREEN_NOTIFY_UPDATE) would be the true analogue of a
		 * page-flip event -- it fires when Screen finishes composition -- but
		 * it cannot be registered on this driver at all: it returns EPERM for
		 * both a window and a display target, even as root, and ENOENT for a
		 * context-wide registration. screen_wait_vsync() is what is left, and
		 * it is accurate: 59.97Hz, +/-0.65ms jitter, zero drift over 5s.
		 *
		 * The timer has to be a period TARGET, not a fixed post-commit delay:
		 * the frame callback is what unblocks the next frame, so a flat delay
		 * is added to the frame time rather than overlapping with it. With a
		 * flat 16ms the measured period was commit + 16ms exactly -- 33ms
		 * (29fps) once the GLES2 read-back had eaten 17ms, and 17ms (55fps)
		 * on the pixman path, where commit is under 1ms. Charging elapsed
		 * time against the period makes the backend hold the frame rate
		 * instead of degrading with it.
		 *
		 * Clamped to at least 1ms on purpose: wl_event_source_timer_update()
		 * treats 0 as "disable the timer", which would stall the output loop
		 * entirely once commit got fast enough.
		 */
		int32_t period = output->frame_delay_ms > 0 ? output->frame_delay_ms : 16;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		/*
		 * Only a commit that carried a buffer consumed the frame. Anything
		 * else (a modeset, an enable) has no render behind it, so charging it
		 * would shorten the wait for no reason and, if such commits ever
		 * arrived back to back, push the output loop to its 1ms floor.
		 */
		int64_t spent = 0;
		if (state->committed & WLR_OUTPUT_STATE_BUFFER
				&& output->frame_event_at.tv_sec > 0) {
			spent = (int64_t)(now.tv_sec - output->frame_event_at.tv_sec) * 1000
				+ (now.tv_nsec - output->frame_event_at.tv_nsec) / 1000000;
			if (spent < 0) {
				spent = 0;
			}
		}
		int64_t delay = (int64_t)period - spent;
		if (delay < 1) {
			delay = 1;
		} else if (delay > period) {
			delay = period;
		}
		/*
		 * Both paces are armed and the first to fire emits the frame event
		 * (see emit_frame_event()). On a display where screen_wait_vsync()
		 * works, the tick usually wins and the frame lands on a real vsync;
		 * the timer is what keeps the output alive when it does not, when the
		 * display is mirrored (ENOTSUP), or when a tick is ever missed.
		 */
		output->frame_event_pending = true;
		if (output->vsync != NULL
				&& !wlr_qnx_screen_vsync_notifier_failed(output->vsync)) {
			/*
			 * Drop any tick left in the pipe before asking for a new one.
			 * A byte written just after the fallback timer emitted would
			 * otherwise leave the fd readable, and the fd handler would
			 * release this frame immediately instead of on its own vsync.
			 */
			wlr_qnx_screen_vsync_notifier_drain(output->vsync);
			wlr_qnx_screen_vsync_notifier_arm(output->vsync);
		}
		wl_event_source_timer_update(output->frame_timer, (int32_t)delay);
	}

	return true;
}

static bool output_set_cursor(struct wlr_output *wlr_output,
		struct wlr_buffer *buffer, int hotspot_x, int hotspot_y) {
	// No hardware cursor plane; the compositor is expected to fall back to
	// rendering the cursor as regular scene content. SCREEN_CURSOR_SHAPE_NONE
	// (set on the session at window-creation time) keeps QNX Screen's own
	// pointer glyph out of the way.
	return false;
}

static bool output_move_cursor(struct wlr_output *wlr_output, int x, int y) {
	return false;
}

static const struct wlr_drm_format_set *output_get_primary_formats(
		struct wlr_output *wlr_output, uint32_t buffer_caps) {
	struct wlr_qnx_screen_output *output = output_from_wlr_output(wlr_output);
	struct wlr_qnx_screen_backend *backend = output->backend;

	// buffer_caps comes from the allocator the compositor picked, which
	// already reflects the renderer: the gles2 renderer only accepts DMA-BUF
	// render targets, so with it wlr_allocator_autocreate() gives us a GBM
	// allocator and DMABUF shows up here; pixman accepts CPU pointers, so its
	// SHM allocator reaches the SHM branch and the plain memcpy path.
	if ((buffer_caps & WLR_BUFFER_CAP_DMABUF) && backend->drm_fd >= 0) {
		return &backend->dmabuf_formats;
	}
	if (buffer_caps & WLR_BUFFER_CAP_SHM) {
		return &backend->shm_formats;
	}
	return NULL;
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_qnx_screen_output *output = output_from_wlr_output(wlr_output);

	wlr_output_finish(wlr_output);

	wlr_pointer_finish(&output->pointer);
	wlr_touch_finish(&output->touch);

	struct wlr_qnx_screen_touchpoint *tp, *tp_tmp;
	wl_list_for_each_safe(tp, tp_tmp, &output->touchpoints, link) {
		wl_list_remove(&tp->link);
		free(tp);
	}

	qnx_screen_output_destroy_window(output);
	wlr_qnx_gl_present_destroy(output->gl_present);
	output->gl_present = NULL;

	free(output->readback_px);
	output->readback_px = NULL;
	output->readback_cap = 0;

	wl_list_remove(&output->link);

	if (output->vsync_source != NULL) {
		wl_event_source_remove(output->vsync_source);
		output->vsync_source = NULL;
	}
	// Joins the vsync thread, which can take up to one refresh.
	wlr_qnx_screen_vsync_notifier_destroy(output->vsync);
	output->vsync = NULL;

	if (output->frame_timer != NULL) {
		wl_event_source_remove(output->frame_timer);
	}

	free(output);
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
	.set_cursor = output_set_cursor,
	.move_cursor = output_move_cursor,
	.get_primary_formats = output_get_primary_formats,
};

bool wlr_output_is_qnx_screen(const struct wlr_output *wlr_output) {
	return wlr_output->impl == &output_impl;
}

bool wlr_qnx_screen_output_set_fullscreen(struct wlr_output *wlr_output,
		bool fullscreen) {
	struct wlr_qnx_screen_output *output = output_from_wlr_output(wlr_output);
	if (output->window != NULL) {
		wlr_log(WLR_ERROR,
			"wlr_qnx_screen_output_set_fullscreen must be called before "
			"the output is enabled");
		return false;
	}
	output->fullscreen = fullscreen;
	return true;
}

bool wlr_qnx_screen_output_set_position(struct wlr_output *wlr_output,
		int32_t x, int32_t y) {
	struct wlr_qnx_screen_output *output = output_from_wlr_output(wlr_output);
	output->x = x;
	output->y = y;
	if (output->window != NULL) {
		int position[] = { x, y };
		return screen_set_window_property_iv(output->window,
			SCREEN_PROPERTY_POSITION, position) == 0;
	}
	return true;
}

bool wlr_qnx_screen_output_set_display(struct wlr_output *wlr_output,
		int display_id) {
	struct wlr_qnx_screen_output *output = output_from_wlr_output(wlr_output);
	if (output->window != NULL) {
		wlr_log(WLR_ERROR,
			"wlr_qnx_screen_output_set_display must be called before "
			"the output is enabled");
		return false;
	}

	if (display_id == 0) {
		output->display = NULL;
		output->requested_display_id = 0;
		return true;
	}

	struct wlr_qnx_screen_backend *b = output->backend;
	int count = 0;
	screen_get_context_property_iv(b->context, SCREEN_PROPERTY_DISPLAY_COUNT,
		&count);
	if (count <= 0) {
		return false;
	}

	screen_display_t *displays = calloc((size_t)count, sizeof(*displays));
	if (displays == NULL) {
		return false;
	}
	screen_get_context_property_pv(b->context, SCREEN_PROPERTY_DISPLAYS,
		(void **)displays);

	bool found = false;
	for (int i = 0; i < count; i++) {
		int id = 0;
		screen_get_display_property_iv(displays[i], SCREEN_PROPERTY_ID, &id);
		if (id == display_id) {
			output->display = displays[i];
			output->requested_display_id = display_id;
			found = true;
			break;
		}
	}

	free(displays);
	return found;
}

struct wlr_output *wlr_qnx_screen_output_create(struct wlr_backend *wlr_backend) {
	struct wlr_qnx_screen_backend *backend =
		qnx_screen_backend_from_backend(wlr_backend);

	struct wlr_qnx_screen_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_qnx_screen_output");
		return NULL;
	}
	output->backend = backend;
	wl_list_init(&output->touchpoints);

	screen_display_mode_t screen_mode;
	screen_display_t display = backend->default_display;
	screen_get_display_property_pv(display, SCREEN_PROPERTY_MODE, (void *)&screen_mode);

	output->win_width = screen_mode.width;
	output->win_height = screen_mode.height;

	struct wlr_output *wlr_output = &output->wlr_output;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, screen_mode.width, screen_mode.height, 0);

	wlr_output_init(wlr_output, &backend->backend, &output_impl,
		backend->event_loop, &state);
	wlr_output_state_finish(&state);

	size_t output_num = ++last_output_num;

	char name[64];
	snprintf(name, sizeof(name), "QNX-SCREEN-%zu", output_num);
	wlr_output_set_name(wlr_output, name);

	char description[128];
	snprintf(description, sizeof(description), "QNX Screen output %zu",
		output_num);
	wlr_output_set_description(wlr_output, description);

	output->frame_timer = wl_event_loop_add_timer(backend->event_loop,
		finish_frame_handler, output);

	/*
	 * Prefer pacing frame events off the display's real vsync. Only offered
	 * when the display can report one; screen_wait_vsync() fails with ENOTSUP
	 * on a mirrored display, and on drivers where the call is unsupported,
	 * which leaves the timer to do all the work.
	 */
	screen_display_t vsync_display = output->display != NULL
		? output->display : backend->default_display;
	if (vsync_display != NULL) {
		output->vsync = wlr_qnx_screen_vsync_notifier_create(vsync_display);
	}
	if (output->vsync != NULL) {
		output->vsync_source = wl_event_loop_add_fd(backend->event_loop,
			output->vsync->pipe_fds[0], WL_EVENT_READABLE, vsync_handler,
			output);
		if (output->vsync_source == NULL) {
			wlr_log(WLR_ERROR, "qnx-screen: failed to watch the vsync fd, "
				"using the timer");
			wlr_qnx_screen_vsync_notifier_destroy(output->vsync);
			output->vsync = NULL;
		}
	}

	wlr_pointer_init(&output->pointer, &qnx_screen_pointer_impl, "qnx-screen-pointer");
	output->pointer.output_name = strdup(wlr_output->name);

	wlr_touch_init(&output->touch, &qnx_screen_touch_impl, "qnx-screen-touch");
	output->touch.output_name = strdup(wlr_output->name);

	wl_list_insert(&backend->outputs, &output->link);

	if (backend->started) {
		wl_signal_emit_mutable(&backend->backend.events.new_output, wlr_output);
		wl_signal_emit_mutable(&backend->backend.events.new_input,
			&output->pointer.base);
		wl_signal_emit_mutable(&backend->backend.events.new_input,
			&output->touch.base);
	}

	return wlr_output;
}

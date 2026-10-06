#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <screen/screen.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/render/dmabuf.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#include "backend/qnx_screen.h"
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
 * SCREEN_PROPERTY_BUFFER_SIZE and re-creating its render buffers. There's no
 * standing EGL surface here; the pixman renderer draws into wlr_buffer
 * objects handed to us by the compositor (via the generic SHM allocator),
 * and output_commit() below is responsible for copying finished frames into
 * one of the window's native buffers before calling screen_post_window().
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

	memset(output->buffers, 0, sizeof(output->buffers));
	if (screen_get_window_property_pv(output->window,
			SCREEN_PROPERTY_RENDER_BUFFERS, (void **)output->buffers) != 0) {
		wlr_log(WLR_ERROR,
			"screen_get_window_property_pv(RENDER_BUFFERS) failed");
		return false;
	}
	output->n_buffers = QNX_SCREEN_NUM_BUFFERS;
	output->buffer_index = 0;

	return true;
}

bool qnx_screen_output_create_window(struct wlr_qnx_screen_output *output) {
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

	int usage[] = { SCREEN_USAGE_NATIVE | SCREEN_USAGE_READ | SCREEN_USAGE_WRITE };
	screen_set_window_property_iv(output->window, SCREEN_PROPERTY_USAGE, usage);

	int format[] = { SCREEN_FORMAT_RGBX8888 };
	screen_set_window_property_iv(output->window, SCREEN_PROPERTY_FORMAT, format);

	if (output->fullscreen && display != NULL) {
		int display_size[] = { output->win_width, output->win_height };
		screen_get_display_property_iv(display, SCREEN_PROPERTY_SIZE,
			display_size);

		output->win_width = display_size[0];
		output->win_height = display_size[1];
		output->x = 0;
		output->y = 0;
	}

	int position[] = { output->x, output->y };
	screen_set_window_property_iv(output->window, SCREEN_PROPERTY_POSITION,
		position);

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

	int window_id = 0;
	screen_get_window_property_iv(output->window, SCREEN_PROPERTY_ID,
		&window_id);
	wlr_log(WLR_DEBUG, "QNX Screen output %dx%d, window id %d",
		output->win_width, output->win_height, window_id);

	return true;
}

void qnx_screen_output_destroy_window(struct wlr_qnx_screen_output *output) {
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

		struct wlr_shm_attributes shm;
		struct wlr_dmabuf_attributes dmabuf;
		if (wlr_buffer_get_shm(state->buffer, &shm)) {
			// OK: CPU-copied straight into the QNX Screen window buffer.
		} else if (wlr_buffer_get_dmabuf(state->buffer, &dmabuf)) {
			// OK, as long as it's something we can mmap() and walk row by
			// row: single-plane, linear (matches the LINEAR-only modifier
			// we advertise in output_get_primary_formats(), so a
			// conforming allocator shouldn't hand us anything else, but
			// don't trust that blindly).
			if (dmabuf.n_planes != 1 || dmabuf.modifier != DRM_FORMAT_MOD_LINEAR) {
				wlr_log(WLR_DEBUG,
					"Unsupported dmabuf layout (n_planes=%d modifier=0x%"PRIx64")",
					dmabuf.n_planes, (uint64_t)dmabuf.modifier);
				return false;
			}
		} else {
			wlr_log(WLR_DEBUG,
				"QNX Screen output only supports shared-memory or "
				"single-plane linear dmabuf buffers");
			return false;
		}
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

static int finish_frame_handler(void *data) {
	struct wlr_qnx_screen_output *output = data;
	wlr_output_send_frame(&output->wlr_output);
	return 0;
}

struct qnx_screen_buffer_access {
	bool is_shm;
	struct gbm_bo *gbm_bo;
	void *gbm_map_data;
};

// Gets a CPU-readable pointer + row stride to a wlr_buffer's pixel data,
// regardless of whether it's SHM-backed (the common case today) or
// dmabuf-backed (once wlr_allocator_autocreate() picks GBM, see backend.c).
// QNX Screen buffers are always window-bound, so there's no way to make the
// renderer draw directly into one -- we always end up doing this copy.
static bool begin_buffer_access(struct wlr_qnx_screen_backend *backend,
		struct wlr_buffer *buffer, void **src_out, size_t *stride_out,
		struct qnx_screen_buffer_access *access) {
	memset(access, 0, sizeof(*access));

	void *src;
	uint32_t format;
	size_t stride;
	if (wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &src, &format, &stride)) {
		access->is_shm = true;
		*src_out = src;
		*stride_out = stride;
		return true;
	}

	struct wlr_dmabuf_attributes dmabuf;
	if (!wlr_buffer_get_dmabuf(buffer, &dmabuf) || dmabuf.n_planes != 1) {
		return false;
	}

	struct gbm_device *gbm_device = qnx_screen_backend_ensure_gbm_device(backend);
	if (gbm_device == NULL) {
		wlr_log(WLR_ERROR,
			"Got a dmabuf-backed output buffer but have no usable gbm_device "
			"to read it back with");
		return false;
	}

	// A bare mmap() on dmabuf.fd[0] is deliberately NOT used here. On some
	// drivers (virtio-gpu/virgl in particular -- the buffer may live
	// host-side in a VM) a raw mmap() bypasses whatever transfer the driver
	// needs to actually get rendered content into a CPU-visible mapping,
	// and reads back as stale/zeroed (black) regardless of dma-buf fencing.
	// gbm_bo_map() is the portable, driver-correct way to read a GBM
	// buffer's contents back to the CPU -- it's what handles that transfer
	// (or does nothing extra, on drivers where a plain mmap would've been
	// fine) internally, the same way wlroots' own renderer never does a raw
	// mmap() either (it goes through glReadPixels/wlr_texture_read_pixels).
	struct gbm_import_fd_modifier_data import_data = {
		.width = (uint32_t)buffer->width,
		.height = (uint32_t)buffer->height,
		.format = dmabuf.format,
		.num_fds = 1,
		.modifier = dmabuf.modifier,
	};
	import_data.fds[0] = dmabuf.fd[0];
	import_data.strides[0] = dmabuf.stride[0];
	import_data.offsets[0] = dmabuf.offset[0];

	struct gbm_bo *bo = gbm_bo_import(gbm_device,
		GBM_BO_IMPORT_FD_MODIFIER, &import_data, GBM_BO_USE_RENDERING);
	if (bo == NULL) {
		wlr_log(WLR_ERROR, "gbm_bo_import failed");
		return false;
	}

	uint32_t map_stride = 0;
	void *map_data = NULL;
	void *ptr = gbm_bo_map(bo, 0, 0, (uint32_t)buffer->width,
		(uint32_t)buffer->height, GBM_BO_TRANSFER_READ, &map_stride,
		&map_data);
	if (ptr == NULL) {
		wlr_log(WLR_ERROR, "gbm_bo_map failed");
		gbm_bo_destroy(bo);
		return false;
	}

	access->gbm_bo = bo;
	access->gbm_map_data = map_data;

	*src_out = ptr;
	*stride_out = map_stride;
	return true;
}

static void end_buffer_access(struct wlr_buffer *buffer,
		struct qnx_screen_buffer_access *access) {
	if (access->is_shm) {
		wlr_buffer_end_data_ptr_access(buffer);
		return;
	}
	if (access->gbm_bo != NULL) {
		gbm_bo_unmap(access->gbm_bo, access->gbm_map_data);
		gbm_bo_destroy(access->gbm_bo);
	}
}

static bool copy_buffer_to_window(struct wlr_qnx_screen_output *output,
		struct wlr_buffer *buffer, const struct wlr_output_state *state) {
	void *src;
	size_t src_stride;
	struct qnx_screen_buffer_access access;
	if (!begin_buffer_access(output->backend, buffer, &src, &src_stride,
			&access)) {
		wlr_log(WLR_ERROR, "Failed to access buffer contents for copy");
		return false;
	}

	screen_buffer_t native = output->buffers[output->buffer_index];

	void *dst = NULL;
	int dst_stride = 0;
	screen_get_buffer_property_pv(native, SCREEN_PROPERTY_POINTER, &dst);
	screen_get_buffer_property_iv(native, SCREEN_PROPERTY_STRIDE, &dst_stride);

	if (dst == NULL || dst_stride <= 0) {
		wlr_log(WLR_ERROR, "Failed to access QNX Screen native buffer");
		end_buffer_access(buffer, &access);
		return false;
	}

	size_t row_bytes = (size_t)buffer->width * 4; // XRGB8888 is always 4 bpp
	size_t copy_bytes = row_bytes < (size_t)dst_stride ? row_bytes : (size_t)dst_stride;
	if (copy_bytes > src_stride) {
		copy_bytes = src_stride;
	}

	for (int y = 0; y < buffer->height; y++) {
		memcpy((uint8_t *)dst + (size_t)y * dst_stride,
			(uint8_t *)src + (size_t)y * src_stride, copy_bytes);
	}

	end_buffer_access(buffer, &access);

	// Compute dirty rectangles from the damage region, in the same
	// [x1, y1, x2, y2] layout screen_post_window() expects.
	int nrects = 0;
	int *rects = NULL;
	int full_rect[4] = { 0, 0, buffer->width, buffer->height };

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

	if (rects != full_rect) {
		free(rects);
	}

	if (ret != 0) {
		wlr_log(WLR_ERROR, "screen_post_window failed");
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
				if (!qnx_screen_output_recreate_buffers(output)) {
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

	if ((state->committed & WLR_OUTPUT_STATE_BUFFER) && output->window != NULL) {
		if (!copy_buffer_to_window(output, state->buffer, state)) {
			return false;
		}
	}

	if (pending_enabled) {
		struct wlr_output_event_present present_event = {
			.commit_seq = wlr_output->commit_seq + 1,
			.presented = true,
		};
		output_defer_present(wlr_output, present_event);

		int32_t delay = output->frame_delay_ms > 0 ? output->frame_delay_ms : 16;
		wl_event_source_timer_update(output->frame_timer, delay);
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

	// Advertise dmabuf/GBM formats based on drm_fd availability, not on
	// whether backend->gbm_device has been created yet -- it's created
	// lazily on first actual buffer readback (see
	// qnx_screen_backend_ensure_gbm_device() in backend.c), which hasn't
	// happened yet the first time this gets called: wlr_allocator_autocreate()
	// queries formats BEFORE any buffer exists, to decide whether to try
	// GBM at all. If our lazy gbm_device creation later fails anyway,
	// copy_buffer_to_window() fails that specific commit loudly rather
	// than silently -- better than never trying GBM in the first place.
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

	wl_list_remove(&output->link);

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

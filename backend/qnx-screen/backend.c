#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <screen/screen.h>
#include <wayland-server-core.h>

#include <wlr/backend/interface.h>
#include <wlr/backend/qnx_screen.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#include "backend/qnx_screen.h"

/*
 * QNX Screen windows have no native concept of dmabuf/GBM -- but if this
 * system has a working DRM render node behind /dev/dri (a real one, not a
 * QNX Screen abstraction), wlroots' own wlr_renderer_autocreate()/
 * wlr_allocator_autocreate() will use it to select the GLES2 renderer and a
 * GBM allocator entirely on their own, the moment wlr_backend_get_drm_fd()
 * returns something >= 0. That in turn is what makes labwc advertise
 * zwp_linux_dmabuf_v1 to clients (see wlr_linux_dmabuf_v1_create_with_renderer()
 * in wlroots' types/wlr_linux_dmabuf_v1.c) -- we don't implement that
 * protocol ourselves, we just need to stop returning -1 here.
 *
 * Our own output_commit() still has to get pixels onto an actual QNX Screen
 * window buffer, which stays CPU-side (see copy_buffer_to_window() in
 * output.c) -- but everything upstream of that (client rendering, the
 * renderer's internal scene compositing) is now real GLES2/dmabuf.
 *
 * Override with $WLR_QNX_SCREEN_DRM_DEVICE if the render node isn't at one
 * of the usual /dev/dri/renderD1xx paths.
 */
static int open_drm_render_node(void) {
	const char *override = getenv("WLR_QNX_SCREEN_DRM_DEVICE");
	if (override != NULL && override[0] != '\0') {
		int fd = open(override, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			wlr_log_errno(WLR_ERROR,
				"Failed to open WLR_QNX_SCREEN_DRM_DEVICE=%s", override);
		}
		return fd;
	}

	DIR *dir = opendir("/dev/dri");
	if (dir == NULL) {
		wlr_log(WLR_INFO,
			"/dev/dri not present; qnx-screen backend will use SHM/pixman only");
		return -1;
	}

	int fd = -1;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, "renderD", 7) != 0) {
			continue;
		}

		char path[256];
		snprintf(path, sizeof(path), "/dev/dri/%s", ent->d_name);

		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			wlr_log(WLR_INFO, "Using DRM render node %s", path);
			break;
		}
		wlr_log_errno(WLR_DEBUG, "Failed to open %s", path);
	}
	closedir(dir);

	if (fd < 0) {
		wlr_log(WLR_INFO,
			"No usable /dev/dri/renderD* node found; qnx-screen backend "
			"will use SHM/pixman only");
	}
	return fd;
}

struct wlr_qnx_screen_backend *qnx_screen_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend_is_qnx_screen(wlr_backend));
	struct wlr_qnx_screen_backend *backend =
		wl_container_of(wlr_backend, backend, backend);
	return backend;
}

struct wlr_qnx_screen_output *qnx_screen_output_from_window(
		struct wlr_qnx_screen_backend *backend, screen_window_t window) {
	struct wlr_qnx_screen_output *output;
	wl_list_for_each(output, &backend->outputs, link) {
		if (output->window == window) {
			return output;
		}
	}
	return NULL;
}

static screen_display_t get_default_display(struct wlr_qnx_screen_backend *b) {
	int count = 0;
	screen_get_context_property_iv(b->context,
		SCREEN_PROPERTY_DISPLAY_COUNT, &count);
	if (count <= 0) {
		return NULL;
	}

	screen_display_t *displays = calloc(count, sizeof(*displays));
	if (displays == NULL) {
		return NULL;
	}

	screen_get_context_property_pv(b->context, SCREEN_PROPERTY_DISPLAYS,
		(void **)displays);
	screen_display_t result = displays[0];
	free(displays);
	return result;
}

static int handle_screen_events(int fd, uint32_t mask, void *data) {
	struct wlr_qnx_screen_backend *b = data;

	// Drain the wake-up byte(s) written by the monitor thread.
	char ch;
	while (read(fd, &ch, sizeof(ch)) == sizeof(ch)) {
		// no-op
	}
	wlr_qnx_screen_event_monitor_arm(b->event_monitor);

	screen_event_t event = NULL;
	if (screen_create_event(&event) != 0 || event == NULL) {
		wlr_log(WLR_ERROR, "screen_create_event failed");
		return 0;
	}

	int count = 0;
	while (screen_get_event(b->context, event, 0) == 0) {
		int type = SCREEN_EVENT_NONE;
		screen_get_event_property_iv(event, SCREEN_PROPERTY_TYPE, &type);
		if (type == SCREEN_EVENT_NONE) {
			break;
		}

		switch (type) {
		case SCREEN_EVENT_POINTER:
			qnx_screen_handle_pointer_event(b, event);
			count++;
			break;
		case SCREEN_EVENT_KEYBOARD:
			qnx_screen_handle_keyboard_event(b, event);
			count++;
			break;
		case SCREEN_EVENT_MTOUCH_TOUCH:
			qnx_screen_handle_touch_event(b, event, 0);
			count++;
			break;
		case SCREEN_EVENT_MTOUCH_MOVE:
			qnx_screen_handle_touch_event(b, event, 1);
			count++;
			break;
		case SCREEN_EVENT_MTOUCH_RELEASE:
			qnx_screen_handle_touch_event(b, event, 2);
			count++;
			break;
		case SCREEN_EVENT_PROPERTY:
			qnx_screen_handle_property_event(b, event);
			count++;
			break;
		default:
			break;
		}
	}

	screen_destroy_event(event);
	return count;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_qnx_screen_backend *backend =
		qnx_screen_backend_from_backend(wlr_backend);

	wlr_log(WLR_INFO, "Starting QNX Screen backend");
	backend->started = true;

	wl_signal_emit_mutable(&backend->backend.events.new_input,
		&backend->keyboard.base);

	struct wlr_qnx_screen_output *output;
	wl_list_for_each(output, &backend->outputs, link) {
		wl_signal_emit_mutable(&backend->backend.events.new_output,
			&output->wlr_output);
		wl_signal_emit_mutable(&backend->backend.events.new_input,
			&output->pointer.base);
		wl_signal_emit_mutable(&backend->backend.events.new_input,
			&output->touch.base);
	}

	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	if (wlr_backend == NULL) {
		return;
	}

	struct wlr_qnx_screen_backend *backend =
		qnx_screen_backend_from_backend(wlr_backend);

	struct wlr_qnx_screen_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &backend->outputs, link) {
		wlr_output_destroy(&output->wlr_output);
	}

	wlr_keyboard_finish(&backend->keyboard);

	wlr_backend_finish(wlr_backend);

	wl_list_remove(&backend->event_loop_destroy.link);

	if (backend->screen_source) {
		wl_event_source_remove(backend->screen_source);
	}
	if (backend->event_monitor) {
		wlr_qnx_screen_event_monitor_destroy(backend->event_monitor);
	}

	wlr_drm_format_set_finish(&backend->shm_formats);
	wlr_drm_format_set_finish(&backend->dmabuf_formats);

	if (backend->gbm_device != NULL) {
		gbm_device_destroy(backend->gbm_device);
	}
	if (backend->gbm_fd >= 0) {
		close(backend->gbm_fd);
	}
	if (backend->drm_fd >= 0) {
		close(backend->drm_fd);
	}

	if (backend->context) {
		screen_destroy_context(backend->context);
	}

	free(backend);
}

struct gbm_device *qnx_screen_backend_ensure_gbm_device(
		struct wlr_qnx_screen_backend *backend) {
	if (backend->gbm_device_init_attempted) {
		return backend->gbm_device; // NULL if the first attempt failed
	}
	backend->gbm_device_init_attempted = true;

	if (backend->drm_fd < 0) {
		return NULL;
	}

	// output.c's copy_buffer_to_window() uses this (via
	// gbm_bo_import()/gbm_bo_map()) to read GPU-rendered dmabuf output
	// buffers back to the CPU. This matters more than it might look: a
	// bare mmap() on the dmabuf fd is NOT sufficient on every driver --
	// e.g. virtio-gpu (virgl) resources live host-side and need the
	// driver's own transfer path, which only gbm_bo_map() is guaranteed
	// to trigger. Without a working gbm_device here, dmabuf output reads
	// back as black/garbage regardless of dma-buf fencing.
	//
	// Deliberately dup()'d: wlr_allocator_autocreate() creates its OWN
	// independent gbm_device against backend->drm_fd. Two struct
	// gbm_device instances sharing one fd is a real conflict on some
	// drivers -- observed in practice as wlroots' own "Failed to create
	// allocator" once this backend also held a gbm_device open on the
	// same fd number. Each gbm_device gets its own fd instead.
	//
	// Deliberately lazy (called from output.c on first dmabuf commit, not
	// from wlr_qnx_screen_backend_create()): creating a gbm_device before
	// EGL has ever initialized against this device has been observed to
	// fail outright on this Mesa/virtio-gpu build ("Can't open 'dri'
	// library!" from Mesa's own DRI loader). By the time we get here, a
	// dmabuf-backed wlr_buffer already exists, which means the GLES2
	// renderer (and therefore EGL) already initialized successfully.
	int gbm_fd = fcntl(backend->drm_fd, F_DUPFD_CLOEXEC, 0);
	if (gbm_fd < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to dup DRM fd for our own gbm_device");
		return NULL;
	}

	backend->gbm_device = gbm_create_device(gbm_fd);
	if (backend->gbm_device == NULL) {
		wlr_log(WLR_ERROR,
			"gbm_create_device failed on the DRM render node; dmabuf "
			"output buffers won't be readable");
		close(gbm_fd);
		return NULL;
	}

	backend->gbm_fd = gbm_fd;
	return backend->gbm_device;
}

static int backend_get_drm_fd(struct wlr_backend *wlr_backend) {
	struct wlr_qnx_screen_backend *backend =
		qnx_screen_backend_from_backend(wlr_backend);
	// -1 if no usable /dev/dri/renderD* node was found at creation time; see
	// open_drm_render_node() above. That's a perfectly normal, supported
	// configuration -- wlr_renderer_autocreate()/wlr_allocator_autocreate()
	// fall back to pixman + the generic SHM allocator automatically in that
	// case.
	//
	// Deliberately NOT gated on backend->gbm_device here (that would be
	// tempting, since output.c's copy_buffer_to_window() needs a working
	// gbm_device to actually read GPU-rendered buffers back). But this
	// backend's own gbm_device is created lazily, on first use, in
	// output.c -- specifically because creating one eagerly here, before
	// EGL has ever touched the device, has been observed to fail outright
	// on this Mesa/virtio-gpu build (Mesa's GBM "dri" backend needs EGL to
	// have already initialized against the device first). Reporting -1
	// here before that's had a chance to happen would prevent
	// wlr_allocator_autocreate() from ever trying GBM at all, even though
	// its own (separate, later) gbm_device -- created only after the GLES2
	// renderer already exists -- works fine.
	return backend->drm_fd;
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_drm_fd = backend_get_drm_fd,
};

bool wlr_backend_is_qnx_screen(const struct wlr_backend *backend) {
	return backend->impl == &backend_impl;
}

static void handle_event_loop_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_qnx_screen_backend *backend =
		wl_container_of(listener, backend, event_loop_destroy);
	backend_destroy(&backend->backend);
}

struct wlr_backend *wlr_qnx_screen_backend_create(struct wl_event_loop *loop) {
	wlr_log(WLR_INFO, "Creating QNX Screen backend");

	struct wlr_qnx_screen_backend *backend = calloc(1, sizeof(*backend));
	if (backend == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_backend_init(&backend->backend, &backend_impl);
	// Advertise both: wlr_allocator_autocreate() picks GBM (dmabuf) only if
	// backend_get_drm_fd() actually returns a valid fd below; otherwise it
	// falls back to SHM automatically. Safe to advertise unconditionally.
	backend->backend.buffer_caps = WLR_BUFFER_CAP_SHM | WLR_BUFFER_CAP_DMABUF;
	backend->gbm_fd = -1;
	backend->drm_fd = open_drm_render_node();
	// backend->gbm_device is NOT created here -- see qnx_screen_get_gbm_device()
	// in output.c, which creates it lazily on first use instead. Doing it
	// this early (before EGL has ever touched the device) has been observed
	// to fail outright on some Mesa/virtio-gpu builds.

	backend->event_loop = loop;
	wl_list_init(&backend->outputs);

	if (screen_create_context(&backend->context, SCREEN_APPLICATION_CONTEXT) < 0) {
		wlr_log_errno(WLR_ERROR, "screen_create_context failed");
		goto error_backend;
	}

	backend->default_display = get_default_display(backend);

	// Each QNX Screen window is created with a single, fixed native pixel
	// format (SCREEN_PROPERTY_FORMAT), so we only advertise one DRM format
	// here. SHM buffers don't have modifiers; INVALID means "implicit/host
	// native layout", which is what we get from screen_create_window_buffers().
	wlr_drm_format_set_add(&backend->shm_formats, DRM_FORMAT_XRGB8888,
		DRM_FORMAT_MOD_INVALID);

	// Explicit LINEAR (not INVALID/"driver's choice") so the GBM allocator
	// can't hand us a tiled/compressed layout we can't safely mmap+memcpy
	// from in copy_buffer_to_window().
	wlr_drm_format_set_add(&backend->dmabuf_formats, DRM_FORMAT_XRGB8888,
		DRM_FORMAT_MOD_LINEAR);

	wlr_keyboard_init(&backend->keyboard, &qnx_screen_keyboard_impl,
		qnx_screen_keyboard_impl.name);

	backend->event_monitor = wlr_qnx_screen_event_monitor_create(backend->context);
	if (backend->event_monitor == NULL) {
		wlr_log(WLR_ERROR, "Failed to create QNX Screen event monitor");
		goto error_context;
	}

	backend->screen_source = wl_event_loop_add_fd(loop,
		backend->event_monitor->pipe_fds[0], WL_EVENT_READABLE,
		handle_screen_events, backend);
	if (backend->screen_source == NULL) {
		wlr_log(WLR_ERROR, "Failed to create QNX Screen event source");
		goto error_monitor;
	}
	wl_event_source_check(backend->screen_source);

	backend->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(loop, &backend->event_loop_destroy);

	return &backend->backend;

error_monitor:
	wlr_qnx_screen_event_monitor_destroy(backend->event_monitor);
error_context:
	wlr_keyboard_finish(&backend->keyboard);
	wlr_drm_format_set_finish(&backend->shm_formats);
	wlr_drm_format_set_finish(&backend->dmabuf_formats);
	screen_destroy_context(backend->context);
error_backend:
	if (backend->gbm_device != NULL) {
		gbm_device_destroy(backend->gbm_device);
	}
	if (backend->gbm_fd >= 0) {
		close(backend->gbm_fd);
	}
	if (backend->drm_fd >= 0) {
		close(backend->drm_fd);
	}
	wlr_backend_finish(&backend->backend);
	free(backend);
	return NULL;
}

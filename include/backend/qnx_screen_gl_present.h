#ifndef WLR_BACKEND_QNX_SCREEN_GL_PRESENT_H
#define WLR_BACKEND_QNX_SCREEN_GL_PRESENT_H

#include <stdbool.h>

struct wlr_buffer;
struct wlr_qnx_screen_output;
struct wlr_qnx_gl_present;

/*
 * GPU-side present: blit the compositor's output buffer into the Screen window
 * render buffer instead of reading it back through the CPU. See gl_present.c for
 * why, and for the two things that fail silently if got wrong.
 */

/* Whether the output's window must be created with GL buffer usage, which is a
 * precondition for GL writes to a window render buffer to be composited at all,
 * and therefore whether the GL present is used at all. There is no run-time
 * switch: true for the GLES2 renderer until the GL present gives up for this
 * output (see wlr_qnx_gl_present_disable_and_rebuild). The read-back path
 * remains in the tree for the pixman renderer and for after that retirement. */
bool qnx_screen_output_wants_gl_window(struct wlr_qnx_screen_output *output);

struct wlr_qnx_gl_present *wlr_qnx_gl_present_create(
	struct wlr_qnx_screen_output *output);
void wlr_qnx_gl_present_destroy(struct wlr_qnx_gl_present *gp);

/* Drop the EGLImage/FBO imports of the window render buffers, e.g. after the
 * window or its buffers are recreated. Safe without a current GL context. */
void qnx_gl_present_invalidate(struct wlr_qnx_screen_output *output);

/* Copy buffer into buffers[output->buffer_index] on the GPU. Returns false if
 * this frame has to go the read-back way; the caller must then rebuild the
 * window CPU-backed (see qnx_screen_output_wants_gl_window). */
bool wlr_qnx_gl_present(struct wlr_qnx_screen_output *output,
	struct wlr_buffer *buffer);

/* Permanently disable the GL present for this output and rebuild its window
 * CPU-backed so the read-back path can write it again. */
void wlr_qnx_gl_present_disable_and_rebuild(struct wlr_qnx_screen_output *output);

#endif

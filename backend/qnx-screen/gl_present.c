/* GPU-side present for the QNX Screen backend.
 *
 * WHY
 * ---
 * With the GLES2 renderer the compositor's finished frame lives in a GBM
 * DMA-BUF (the renderer can only draw into a DMA-BUF), while a Screen window
 * render buffer is memory Screen composites directly. The read-back present
 * (copy_buffer_to_window_gles in output.c) bridges them by walking every
 * damaged pixel GPU->CPU: glReadPixels into a staging buffer, then memcpy into
 * the window buffer. Measured on this driver that is ~13-16 ms for a 1080p
 * frame, and it is charged once per damage *rectangle* (~0.3-0.6 ms per box
 * whatever its size), so a frame carrying 32 boxes costs ~71 ms -- about 12 fps
 * and the reason moving the pointer over a busy screen is so expensive.
 *
 * This file replaces the crossing with a GPU->GPU blit. Screen 4.3's EGL driver
 * advertises EGL_QNX_image_native_buffer, and a window render buffer imported
 * through it is a valid GL texture, so the compositor's buffer can be drawn
 * straight into the buffer screen_post_window() will show:
 *
 *   DMA-BUF -> EGLImage -> GL texture --blit--> FBO(EGLImage(window buffer))
 *
 * Measured cost of the same frame: ~0.5 ms.
 *
 * STATUS
 * The only present path for the GLES2 renderer; there is no run-time switch.
 * Measured (tinywl, 1080p, 32 damage rectangles): 58 fps against 12 fps for the
 * read-back, present cost 0.5 ms against 71 ms. Checked on the panel by eye and
 * on the real desktop (the real Wayland desktop session) with the pointer driven
 * at 125 Hz. It was additionally checked against an in-process verifier that
 * screenshots the panel and compares it with the output buffer (reported no
 * differing sample points on this path, the same as on the read-back and pixman
 * paths); that verifier is deliberately not part of this patch, since it costs
 * ~12 ms per frame -- see gl-present-debug.patch.
 *
 * The one escape hatch left is automatic: a frame this path cannot do retires
 * it for that output, rebuilds the window CPU-backed and retries the same
 * commit, so a driver where the import misbehaves ends up on the read-back path
 * with a logged error rather than a dead or black output. To force the read-back
 * from the start, revert the patch.
 *
 * TWO THINGS THAT FAIL SILENTLY IF GOT WRONG
 * ------------------------------------------
 * 1. Window usage. A GL write into an imported window buffer only reaches the
 *    panel if the window was created with a GL usage bit. With
 *    SCREEN_USAGE_NATIVE|READ|WRITE the FBO is FRAMEBUFFER_COMPLETE, GL reports
 *    no error, and the composited image never changes -- because Screen keeps a
 *    separate CPU-side view of that buffer, which is what it composites. So the
 *    window is created NATIVE|OPENGL_ES2 here, which in turn has no
 *    SCREEN_PROPERTY_POINTER, meaning the read-back path cannot write it. The
 *    two presents therefore need different windows, and a failure of this path
 *    rebuilds the window CPU-backed instead of silently painting black.
 * 2. Orientation. A DMA-BUF imported as a GL texture is sampled bottom-up
 *    relative to image order, so the blit needs a vertical flip. The read-back
 *    path needs the *opposite* (its glReadPixels already yields top-down), so
 *    the two paths must not share a shader or UV set.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <pixman.h>
#include <screen/screen.h>

#include <wlr/render/egl.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#include "backend/qnx_screen.h"
#include "backend/qnx_screen_gl_present.h"
#include "render/egl.h"
#include "render/gles2.h"

#ifndef EGL_NATIVE_BUFFER_QNX
#define EGL_NATIVE_BUFFER_QNX 0x3551
#endif

/* glBlitFramebuffer is ES3+ and this file is built against the GLES2 headers
 * (like the rest of the renderer), so the few ES3 tokens it needs are declared
 * here rather than by pulling GLES3/gl3.h into a GLES2 translation unit. The
 * entry point itself is fetched with eglGetProcAddress and only used when the
 * live context reports ES 3.x. */
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
typedef void (*qnx_blit_framebuffer_proc)(GLint, GLint, GLint, GLint, GLint, GLint,
	GLint, GLint, GLbitfield, GLenum);

typedef EGLImageKHR (*qnx_create_image_proc)(EGLDisplay, EGLContext, EGLenum,
	const void *, const EGLint *);
typedef EGLBoolean (*qnx_destroy_image_proc)(EGLDisplay, EGLImageKHR);

struct wlr_qnx_gl_present {
	struct wlr_qnx_screen_output *output;

	/* Per window render buffer: the EGLImage of the buffer, bound as a texture
	 * (which doubles as the blit source when the buffer is the front buffer)
	 * and attached to an FBO. tracked[] records which screen_buffer_t each slot
	 * describes, because Screen may rotate SCREEN_PROPERTY_RENDER_BUFFERS and a
	 * region tracked by array index would then describe the wrong pixels. */
	EGLImageKHR image[QNX_SCREEN_NUM_BUFFERS];
	GLuint tex[QNX_SCREEN_NUM_BUFFERS];
	GLuint fbo[QNX_SCREEN_NUM_BUFFERS];
	screen_buffer_t tracked[QNX_SCREEN_NUM_BUFFERS];

	GLuint program;
	GLint u_tex, u_pos, u_uv;

	qnx_blit_framebuffer_proc blitFramebuffer;
	bool have_blit;
	bool blit_checked;

	qnx_create_image_proc create_image;
	qnx_destroy_image_proc destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;

};

bool qnx_screen_output_wants_gl_window(struct wlr_qnx_screen_output *output) {
	/* No switch to enable this: with the GLES2 renderer it is the only present
	 * path, until it fails (see the header for what happens then). To get the
	 * read-back path back, revert the patch. */
	if (output->gl_present_failed) {
		return false;
	}
	/* Only the GLES2 renderer puts its frames in a DMA-BUF that a GPU blit can
	 * read. pixman renders into shared memory, which the CPU copy path already
	 * reads directly and for which there is nothing to avoid. */
	return output->wlr_output.renderer != NULL
		&& wlr_renderer_is_gles2(output->wlr_output.renderer);
}

static void invalidate_slots(struct wlr_qnx_gl_present *gp) {
	/* The buffers these objects were built on are gone. The GL objects are
	 * deliberately leaked rather than deleted: no GL context is guaranteed to
	 * be current at the call sites (window teardown), and leaking a handful of
	 * names per modeset is cheaper than getting that wrong. */
	for (int i = 0; i < QNX_SCREEN_NUM_BUFFERS; i++) {
		gp->image[i] = EGL_NO_IMAGE_KHR;
		gp->tex[i] = 0;
		gp->fbo[i] = 0;
		gp->tracked[i] = NULL;
	}
}

void qnx_gl_present_invalidate(struct wlr_qnx_screen_output *output) {
	if (output->gl_present != NULL) {
		invalidate_slots(output->gl_present);
	}
}

static void gl_present_reset(struct wlr_qnx_gl_present *gp) {
	invalidate_slots(gp);
	if (gp->program != 0) {
		glDeleteProgram(gp->program);
		gp->program = 0;
	}
}

/* glBlitFramebuffer is ES3+. The extension string is the reliable test on Mesa:
 * an ES2 context does not list ES3, and eglQueryContext's client version needs
 * the context to be current, which it is wherever this is called from. */
static bool wlr_gles2_renderer_supports_blit(struct wlr_gles2_renderer *gr) {
	const char *str = (const char *)glGetString(GL_VERSION);
	if (str == NULL) {
		return false;
	}
	/* "OpenGL ES 3.2 Mesa 25.1.9" / "OpenGL ES 2.0 ..." */
	int major = 0, minor = 0;
	if (sscanf(str, "OpenGL ES %d.%d", &major, &minor) != 2) {
		return false;
	}
	return major >= 3;
}

static GLuint compile(GLenum type, const char *src) {
	GLuint sh = glCreateShader(type);
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char buf[512];
		glGetShaderInfoLog(sh, sizeof(buf), NULL, buf);
		wlr_log(WLR_ERROR, "qnx-screen/gl-present: shader compile failed: %s", buf);
	}
	return sh;
}

static bool build_program(struct wlr_qnx_gl_present *gp) {
	if (gp->program != 0) {
		return true;
	}
	/* v_uv comes from an attribute so the flip lives in the vertex data, where
	 * it cannot be mixed up with the un-flipped read-back path. */
	static const char *vs =
		"attribute vec2 a_pos;\n"
		"attribute vec2 a_uv;\n"
		"varying vec2 v_uv;\n"
		"void main() {\n"
		"  v_uv = a_uv;\n"
		"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
		"}\n";
	static const char *fs =
		"precision mediump float;\n"
		"varying vec2 v_uv;\n"
		"uniform sampler2D tex;\n"
		"void main() {\n"
		"  gl_FragColor = texture2D(tex, v_uv);\n"
		"}\n";
	GLuint prog = glCreateProgram();
	GLuint v = compile(GL_VERTEX_SHADER, vs);
	GLuint f = compile(GL_FRAGMENT_SHADER, fs);
	glAttachShader(prog, v);
	glAttachShader(prog, f);
	glBindAttribLocation(prog, 0, "a_pos");
	glBindAttribLocation(prog, 1, "a_uv");
	glLinkProgram(prog);
	glDeleteShader(v);
	glDeleteShader(f);
	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char buf[512];
		glGetProgramInfoLog(prog, sizeof(buf), NULL, buf);
		wlr_log(WLR_ERROR, "qnx-screen/gl-present: link failed: %s", buf);
		glDeleteProgram(prog);
		return false;
	}
	gp->program = prog;
	gp->u_pos = glGetAttribLocation(prog, "a_pos");
	gp->u_uv = glGetAttribLocation(prog, "a_uv");
	gp->u_tex = glGetUniformLocation(prog, "tex");
	return true;
}

static void drop_slot(struct wlr_qnx_gl_present *gp, int i) {
	struct wlr_gles2_renderer *gr =
		gles2_get_renderer(gp->output->wlr_output.renderer);
	EGLDisplay dpy = gr != NULL && gr->egl != NULL
		? wlr_egl_get_display(gr->egl) : EGL_NO_DISPLAY;
	if (gp->tex[i] != 0) {
		glDeleteTextures(1, &gp->tex[i]);
		gp->tex[i] = 0;
	}
	if (gp->fbo[i] != 0) {
		glDeleteFramebuffers(1, &gp->fbo[i]);
		gp->fbo[i] = 0;
	}
	if (gp->image[i] != EGL_NO_IMAGE_KHR && dpy != EGL_NO_DISPLAY
			&& gp->destroy_image != NULL) {
		gp->destroy_image(dpy, gp->image[i]);
	}
	gp->image[i] = EGL_NO_IMAGE_KHR;
	gp->tracked[i] = NULL;
}

/* Make sure slot idx holds a complete FBO over output->buffers[idx]. */
static bool ensure_slot(struct wlr_qnx_gl_present *gp, int idx) {
	struct wlr_qnx_screen_output *output = gp->output;
	if (idx < 0 || (size_t)idx >= output->n_buffers) {
		return false;
	}
	screen_buffer_t native = output->buffers[idx];
	if (gp->fbo[idx] != 0 && gp->tracked[idx] == native) {
		return true;
	}
	drop_slot(gp, idx);

	struct wlr_gles2_renderer *gr =
		gles2_get_renderer(output->wlr_output.renderer);
	if (gr == NULL || gr->egl == NULL || gp->create_image == NULL
			|| gp->image_target == NULL) {
		return false;
	}
	EGLDisplay dpy = wlr_egl_get_display(gr->egl);
	EGLContext ctx = wlr_egl_get_context(gr->egl);

	EGLImageKHR img = gp->create_image(dpy, ctx, EGL_NATIVE_BUFFER_QNX,
		native, NULL);
	if (img == EGL_NO_IMAGE_KHR) {
		wlr_log(WLR_ERROR, "qnx-screen/gl-present: eglCreateImageKHR("
			"EGL_NATIVE_BUFFER_QNX) failed: 0x%x", eglGetError());
		return false;
	}

	GLuint tex = 0, fbo = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	gp->image_target(GL_TEXTURE_2D, (GLeglImageOES)img);
	if (glGetError() != GL_NO_ERROR) {
		wlr_log(WLR_ERROR, "qnx-screen/gl-present: binding the window buffer as "
			"a texture failed");
		glDeleteTextures(1, &tex);
		gp->destroy_image(dpy, img);
		return false;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, tex, 0);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		wlr_log(WLR_ERROR, "qnx-screen/gl-present: FBO over the window buffer "
			"is incomplete: 0x%x", status);
		glDeleteFramebuffers(1, &fbo);
		glDeleteTextures(1, &tex);
		gp->destroy_image(dpy, img);
		return false;
	}

	gp->image[idx] = img;
	gp->tex[idx] = tex;
	gp->fbo[idx] = fbo;
	gp->tracked[idx] = native;
	return true;
}

/* Draw the whole source texture into fbo. Only used when the context cannot
 * blit (pre-ES3); the caller must have bound the source to texture unit 0. */
static void blit_boxes(struct wlr_qnx_gl_present *gp, GLuint fbo,
		int dst_w, int dst_h) {
	/* Full-screen quad, uv flipped vertically: the imported DMA-BUF texture is
	 * bottom-up, Screen window buffers are top-down. */
	static const float verts[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	static const float uv[8] = { 0, 1, 1, 1, 0, 0, 1, 0 };

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, dst_w, dst_h);
	glUseProgram(gp->program);
	glUniform1i(gp->u_tex, 0);
	glVertexAttribPointer(gp->u_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glEnableVertexAttribArray(gp->u_pos);
	glVertexAttribPointer(gp->u_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glEnableVertexAttribArray(gp->u_uv);
	glDisable(GL_SCISSOR_TEST);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* The renderer draws into a framebuffer object over a *renderbuffer* import of
 * the output DMA-BUF (gles2_buffer_get_fbo). Sampling a separate *texture*
 * import of the same buffer (gles2_buffer_get_or_create->buffer->tex) reads a
 * different host resource on this driver and can lag or come back empty, so the
 * blit source is the renderer's own FBO. */
static bool wlr_gles2_renderer_supports_blit(struct wlr_gles2_renderer *gr);

static GLuint renderer_fbo_for(struct wlr_qnx_screen_output *o,
		struct wlr_buffer *buffer) {
	struct wlr_gles2_renderer *gr = gles2_get_renderer(o->wlr_output.renderer);
	if (gr == NULL) {
		return 0;
	}
	struct wlr_gles2_buffer *gb = gles2_buffer_get_or_create(gr, buffer);
	if (gb == NULL) {
		return 0;
	}
	return gles2_buffer_get_fbo(gb);
}

struct wlr_qnx_gl_present *wlr_qnx_gl_present_create(
		struct wlr_qnx_screen_output *output) {
	struct wlr_qnx_gl_present *gp = calloc(1, sizeof(*gp));
	if (gp == NULL) {
		/* Loud, because the caller reacts by presenting this frame through the
		 * read-back and will keep doing so quietly for the whole session. */
		wlr_log(WLR_ERROR, "qnx-screen/gl-present: out of memory; this output "
			"will present by CPU read-back");
		return NULL;
	}
	gp->output = output;
	return gp;
}

void wlr_qnx_gl_present_destroy(struct wlr_qnx_gl_present *gp) {
	if (gp == NULL) {
		return;
	}
	struct wlr_gles2_renderer *gr =
		gles2_get_renderer(gp->output->wlr_output.renderer);
	if (gr != NULL && gr->egl != NULL) {
		struct wlr_egl_context prev;
		if (wlr_egl_make_current(gr->egl, &prev)) {
			for (int i = 0; i < QNX_SCREEN_NUM_BUFFERS; i++) {
				drop_slot(gp, i);
			}
			if (gp->program != 0) {
				glDeleteProgram(gp->program);
			}
			wlr_egl_restore_context(&prev);
		}
	}
	free(gp);
}

bool wlr_qnx_gl_present(struct wlr_qnx_screen_output *output,
		struct wlr_buffer *buffer) {
	struct wlr_qnx_screen_output *o = output;
	struct wlr_renderer *wlr_renderer = o->wlr_output.renderer;
	struct wlr_gles2_renderer *gr = gles2_get_renderer(wlr_renderer);
	if (gr == NULL || gr->egl == NULL || o->n_buffers == 0) {
		return false;
	}

	struct wlr_qnx_gl_present *gp = o->gl_present;
	if (gp == NULL || !qnx_screen_output_wants_gl_window(o)) {
		return false;
	}
	if (gp->create_image == NULL) {
		gp->create_image = (qnx_create_image_proc)(void *)
			eglGetProcAddress("eglCreateImageKHR");
		gp->destroy_image = (qnx_destroy_image_proc)(void *)
			eglGetProcAddress("eglDestroyImageKHR");
		gp->image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(void *)
			eglGetProcAddress("glEGLImageTargetTexture2DOES");
		gp->blitFramebuffer = (qnx_blit_framebuffer_proc)(void *)
			eglGetProcAddress("glBlitFramebuffer");
		if (gp->create_image == NULL || gp->image_target == NULL) {
			wlr_log(WLR_ERROR, "qnx-screen/gl-present: EGLImage entry points "
				"missing, disabling the GL present");
			o->gl_present_failed = true;
			return false;
		}
	}

	struct wlr_egl_context prev_ctx;
	if (!wlr_egl_make_current(gr->egl, &prev_ctx)) {
		wlr_log(WLR_ERROR, "qnx-screen/gl-present: cannot make the GL context "
			"current");
		return false;
	}

	bool ok = false;
	GLuint src_tex = 0;
	struct wlr_texture *wlr_tex = NULL;

	/* Everything below changes GL state the renderer owns; save what matters
	 * and put it back, since the compositor keeps drawing after commit. */
	GLint prev_fbo = 0, prev_program = 0, prev_active_tex = 0, prev_array_buf = 0;
	GLint prev_scissor = 0;
	GLint prev_viewport[4] = {0};
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buf);
	glGetIntegerv(GL_SCISSOR_TEST, &prev_scissor);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	GLint prev_tex0_id = 0;
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0_id);

	if (!build_program(gp)) {
		goto out;
	}

	if (!gp->blit_checked) {
		gp->blit_checked = true;
		/* glGetString needs the context that is current *now*, which is why
		 * this cannot be decided where the other entry points are fetched. An
		 * ES2 context exposes the name but not the function, so the version
		 * string is what makes the call safe. */
		gp->have_blit = gp->blitFramebuffer != NULL
			&& wlr_gles2_renderer_supports_blit(gr);
	}

	wlr_tex = wlr_texture_from_buffer(wlr_renderer, buffer);
	if (wlr_tex == NULL) {
		wlr_log(WLR_ERROR, "qnx-screen/gl-present: cannot import the output "
			"buffer as a texture");
		goto out;
	}
	struct wlr_gles2_texture *gtex = gles2_get_texture(wlr_tex);
	if (gtex->target != GL_TEXTURE_2D) {
		/* The whole blit assumes a plain 2D texture; anything else (an external
		 * or array import) would sample wrong rather than fail. */
		goto out;
	}
	src_tex = gtex->tex;

	const int tgt_i = (int)o->buffer_index;

	if (!ensure_slot(gp, tgt_i)) {
		goto out;
	}

	glActiveTexture(GL_TEXTURE0);

	const int dst_w = buffer->width;
	const int dst_h = buffer->height;
	/* The renderer's framebuffer is sized to the output buffer it was begun
	 * for; the output buffer and the window are the same size (output_test
	 * rejects any other combination). */
	const int src_w = buffer->width;
	const int src_h = buffer->height;

	/* The frame must be copied whole.
	 *
	 * A Screen window buffer has to be complete at post time (dirty_rects is a
	 * guide only), and with two buffers the target still holds frame N-2, so a
	 * damage-only copy would first have to carry everything else over from the
	 * front buffer. Carrying over on the GPU means sampling one imported window
	 * buffer as a texture, which this driver composites as black -- the damage
	 * lands and the rest goes dark. A whole-frame copy sidesteps that entirely
	 * and costs the same (~0.2 ms at 1080p against ~16 ms of read-back), so
	 * damage is deliberately not consulted here.
	 *
	 * The source is the renderer's own framebuffer, not a second import of its
	 * DMA-BUF: on this driver a texture import of a buffer the renderer draws
	 * into through a renderbuffer import reads a different host resource, and
	 * the copy comes out stale or empty. That is invisible to the eye when both
	 * are moving and is exactly what the read-back path does not suffer from.
	 *
	 * glBlitFramebuffer is the natural copy (no texture sampling, no flip to
	 * get wrong) but needs ES3; GLES2 has no blit, so fall back to drawing the
	 * source attached as a texture, with the vertical flip that implies. */
	GLuint src_fbo = renderer_fbo_for(o, buffer);
	if (src_fbo != 0 && gp->have_blit) {
		GLuint dst = gp->fbo[tgt_i];
		glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
		/* Both framebuffers have their origin at the bottom-left, so the
		 * copy is identity -- no flip, and none of the orientation risk the
		 * texture path carries. */
		gp->blitFramebuffer(0, 0, src_w, src_h,
			0, 0, dst_w, dst_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
		if (glGetError() != GL_NO_ERROR) {
			wlr_log(WLR_ERROR, "qnx-screen/gl-present: glBlitFramebuffer "
				"failed, falling back to the read-back present");
			ok = false;
			goto out;
		}
	} else {
		/* No ES3 blit: sample the source instead. This is the one path with an
		 * orientation to get wrong (a texture import is bottom-up, the window
		 * buffer is top-down), which is why blit_boxes carries the flip. */
		glBindTexture(GL_TEXTURE_2D, src_tex);
		blit_boxes(gp, gp->fbo[tgt_i], dst_w, dst_h);
	}

	glFinish();
	glBindTexture(GL_TEXTURE_2D, 0);
	ok = glGetError() == GL_NO_ERROR;
	if (!ok) {
		wlr_log(WLR_ERROR, "qnx-screen/gl-present: GL error after the blit");
	}

	glDisableVertexAttribArray(gp->u_pos);
	glDisableVertexAttribArray(gp->u_uv);

out:
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
	glUseProgram((GLuint)prev_program);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_array_buf);
	if (prev_scissor) {
		glEnable(GL_SCISSOR_TEST);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2],
		prev_viewport[3]);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex0_id);
	glActiveTexture((GLenum)prev_active_tex);

	if (wlr_tex != NULL) {
		wlr_texture_destroy(wlr_tex);
	}
	wlr_egl_restore_context(&prev_ctx);
	return ok;
}

void wlr_qnx_gl_present_disable_and_rebuild(
		struct wlr_qnx_screen_output *output) {
	struct wlr_qnx_gl_present *gp = output->gl_present;
	if (gp == NULL || output->gl_present_failed) {
		return;
	}
	wlr_log(WLR_ERROR, "qnx-screen/gl-present: disabling, rebuilding the output "
		"window CPU-backed so the read-back present can write it");
	output->gl_present_failed = true;
	if (gp->program != 0 || gp->fbo[0] != 0) {
		struct wlr_gles2_renderer *gr =
			gles2_get_renderer(output->wlr_output.renderer);
		if (gr != NULL && gr->egl != NULL) {
			struct wlr_egl_context prev;
			if (wlr_egl_make_current(gr->egl, &prev)) {
				gl_present_reset(gp);
				wlr_egl_restore_context(&prev);
			}
		}
	}
	/* The window is left alone: the caller compares its backing against
	 * qnx_screen_output_wants_gl_window() and rebuilds it CPU-backed if it is
	 * still the GL kind. */
}

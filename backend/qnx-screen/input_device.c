#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/keycodes.h>
#include <time.h>

#include <screen/screen.h>
#include <wayland-server-protocol.h>

#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/util/log.h>

#include "backend/qnx_screen.h"

// Index into struct wlr_keyboard.mod_indexes[], matching the mod_names[]
// order wlroots itself uses in wlr_keyboard_set_keymap() (types/wlr_keyboard.c).
enum {
	MOD_IDX_SHIFT = 0,
	MOD_IDX_CAPS = 1,
	MOD_IDX_CTRL = 2,
	MOD_IDX_ALT = 3,
	MOD_IDX_MOD2 = 4, // NumLock
	MOD_IDX_MOD3 = 5,
	MOD_IDX_LOGO = 6, // Super/Meta
	MOD_IDX_MOD5 = 7,
};

static uint32_t get_time_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void keyboard_led_update(struct wlr_keyboard *keyboard, uint32_t leds) {
	// QNX Screen doesn't expose a keyboard-LED write API through libscreen;
	// LED state is left to the platform's input driver.
}

const struct wlr_keyboard_impl qnx_screen_keyboard_impl = {
	.name = "qnx-screen-keyboard",
	.led_update = keyboard_led_update,
};

const struct wlr_pointer_impl qnx_screen_pointer_impl = {
	.name = "qnx-screen-pointer",
};

const struct wlr_touch_impl qnx_screen_touch_impl = {
	.name = "qnx-screen-touch",
};

static struct wlr_qnx_screen_output *output_from_event_window(
		struct wlr_qnx_screen_backend *backend, screen_event_t event) {
	screen_window_t window = NULL;
	screen_get_event_property_pv(event, SCREEN_PROPERTY_WINDOW,
		(void **)&window);
	if (window == NULL) {
		return NULL;
	}
	return qnx_screen_output_from_window(backend, window);
}

static uint32_t xkb_mask_for(struct wlr_keyboard *kb, size_t mod_index) {
	xkb_mod_index_t idx = kb->mod_indexes[mod_index];
	if (idx == XKB_MOD_INVALID) {
		return 0;
	}
	return 1u << idx;
}

/*
 * QNX Screen reports the *current, absolute* modifier state on every
 * SCREEN_EVENT_KEYBOARD/POINTER/MTOUCH_* event via SCREEN_PROPERTY_MODIFIERS,
 * separately from the individual scan-code press/release stream. On this
 * platform the discrete key-up event for a modifier key (Ctrl/Alt in
 * particular) isn't reliable enough to drive xkb_state's own per-keycode
 * modifier tracking -- a missed release leaves the modifier stuck down.
 *
 * qnx-ports/weston's qnx-screen backend works around exactly this by making
 * SCREEN_PROPERTY_MODIFIERS the sole source of truth for modifier state
 * (weston: update_xkb_state_from_screen() + notify_key(..., STATE_UPDATE_NONE)).
 * We do the wlroots equivalent here: qnx_screen_handle_keyboard_event() calls
 * wlr_keyboard_notify_key() with update_state=false (so xkb's own per-key
 * modifier bookkeeping never runs), and this function is the only thing that
 * ever calls wlr_keyboard_notify_modifiers().
 */
void qnx_screen_sync_modifiers(struct wlr_qnx_screen_backend *backend,
		int screen_modifiers) {
	struct wlr_keyboard *kb = &backend->keyboard;
	if (kb->keymap == NULL) {
		// No keymap loaded yet (compositor hasn't called
		// wlr_keyboard_set_keymap() in response to new_input yet); the
		// mod_indexes table below isn't populated yet, nothing to do.
		return;
	}

	// SCREEN_PROPERTY_MODIFIERS doesn't reliably reflect the Meta/Super
	// ("GUI") key on this platform either; track its left/right scan codes
	// separately in qnx_screen_handle_keyboard_event() and fold them in
	// here, mirroring weston's b->left_gui_state/right_gui_state.
	if (backend->left_gui_state || backend->right_gui_state) {
		screen_modifiers |= KEYMOD_MOD8;
	}

	if (screen_modifiers == backend->prev_screen_modifiers) {
		return;
	}
	backend->prev_screen_modifiers = screen_modifiers;

	uint32_t depressed = 0;
	if (screen_modifiers & KEYMOD_SHIFT) {
		depressed |= xkb_mask_for(kb, MOD_IDX_SHIFT);
	}
	if (screen_modifiers & KEYMOD_CTRL) {
		depressed |= xkb_mask_for(kb, MOD_IDX_CTRL);
	}
	if (screen_modifiers & KEYMOD_ALT) {
		depressed |= xkb_mask_for(kb, MOD_IDX_ALT);
	}
	if (screen_modifiers & KEYMOD_MOD8) {
		depressed |= xkb_mask_for(kb, MOD_IDX_LOGO);
	}

	// QNX's *_LOCK modifier bits are "sticky/latched" toggle state, not
	// literal keyboard lock LEDs -- map them to xkb's latched group,
	// matching weston's get_xkb_latched_mods().
	uint32_t latched = 0;
	if (screen_modifiers & KEYMOD_SHIFT_LOCK) {
		latched |= xkb_mask_for(kb, MOD_IDX_SHIFT);
	}
	if (screen_modifiers & KEYMOD_CTRL_LOCK) {
		latched |= xkb_mask_for(kb, MOD_IDX_CTRL);
	}
	if (screen_modifiers & KEYMOD_ALT_LOCK) {
		latched |= xkb_mask_for(kb, MOD_IDX_ALT);
	}
	if (screen_modifiers & KEYMOD_MOD8_LOCK) {
		latched |= xkb_mask_for(kb, MOD_IDX_LOGO);
	}

	uint32_t locked = 0;
	if (screen_modifiers & KEYMOD_CAPS_LOCK) {
		locked |= xkb_mask_for(kb, MOD_IDX_CAPS);
	}
	if (screen_modifiers & KEYMOD_NUM_LOCK) {
		locked |= xkb_mask_for(kb, MOD_IDX_MOD2);
	}
	if (screen_modifiers & KEYMOD_SCROLL_LOCK) {
		locked |= xkb_mask_for(kb, MOD_IDX_MOD3);
	}

	wlr_keyboard_notify_modifiers(kb, depressed, latched, locked, 0);
}

void qnx_screen_handle_pointer_event(struct wlr_qnx_screen_backend *backend,
		screen_event_t event) {
	int screen_modifiers = 0;
	screen_get_event_property_iv(event, SCREEN_PROPERTY_MODIFIERS,
		&screen_modifiers);
	qnx_screen_sync_modifiers(backend, screen_modifiers);

	struct wlr_qnx_screen_output *output =
		output_from_event_window(backend, event);
	if (output == NULL) {
		return;
	}

	uint32_t time_msec = get_time_msec();

	int position[2] = { 0, 0 };
	screen_get_event_property_iv(event, SCREEN_PROPERTY_SOURCE_POSITION,
		position);

	int width = output->win_width > 0 ? output->win_width : 1;
	int height = output->win_height > 0 ? output->win_height : 1;

	double x = (double)position[0] / (double)width;
	double y = (double)position[1] / (double)height;
	if (x < 0.0) x = 0.0;
	if (x > 1.0) x = 1.0;
	if (y < 0.0) y = 0.0;
	if (y > 1.0) y = 1.0;

	struct wlr_pointer_motion_absolute_event motion_event = {
		.pointer = &output->pointer,
		.time_msec = time_msec,
		.x = x,
		.y = y,
	};
	wl_signal_emit_mutable(&output->pointer.events.motion_absolute,
		&motion_event);

	int buttons = 0;
	screen_get_event_property_iv(event, SCREEN_PROPERTY_BUTTONS, &buttons);

	static const struct {
		int mask;
		uint32_t code;
	} button_map[] = {
		// QNX Screen doesn't expose named constants for these in
		// <screen/screen.h> on all versions; use the raw bitmasks
		// (matches qnx-ports/weston's qnx-screen backend).
		{ 0x1, BTN_LEFT },
		{ 0x2, BTN_MIDDLE },
		{ 0x4, BTN_RIGHT },
	};

	uint32_t changed = (uint32_t)buttons ^ output->prev_buttons;
	for (size_t i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
		if (!(changed & button_map[i].mask)) {
			continue;
		}
		bool pressed = (buttons & button_map[i].mask) != 0;
		struct wlr_pointer_button_event button_event = {
			.pointer = &output->pointer,
			.time_msec = time_msec,
			.button = button_map[i].code,
			.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED :
				WL_POINTER_BUTTON_STATE_RELEASED,
		};
		wlr_pointer_notify_button(&output->pointer, &button_event);
	}
	output->prev_buttons = (uint32_t)buttons;

	int wheel = 0;
	screen_get_event_property_iv(event, SCREEN_PROPERTY_MOUSE_WHEEL, &wheel);
	if (wheel != 0) {
		struct wlr_pointer_axis_event axis_event = {
			.pointer = &output->pointer,
			.time_msec = time_msec,
			.source = WL_POINTER_AXIS_SOURCE_WHEEL,
			.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL,
			.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
			.delta = wheel * 15.0,
			.delta_discrete = wheel * WLR_POINTER_AXIS_DISCRETE_STEP,
		};
		wl_signal_emit_mutable(&output->pointer.events.axis, &axis_event);
		wl_signal_emit_mutable(&output->pointer.events.frame, &output->pointer);
	} else {
		wl_signal_emit_mutable(&output->pointer.events.frame, &output->pointer);
	}
}

void qnx_screen_handle_keyboard_event(struct wlr_qnx_screen_backend *backend,
		screen_event_t event) {
	int flags = 0;
	screen_get_event_property_iv(event, SCREEN_PROPERTY_FLAGS, &flags);

	int scan = 0;
	screen_get_event_property_iv(event, SCREEN_PROPERTY_SCAN, &scan);

	int screen_modifiers = 0;
	screen_get_event_property_iv(event, SCREEN_PROPERTY_MODIFIERS,
		&screen_modifiers);

	// NOTE: SCREEN_FLAG_KEY_DOWN (from <screen/screen.h>, the "pressed" bit
	// in the flags word) is NOT the same symbol as the Linux evdev KEY_DOWN
	// (the "Down arrow" keycode, pulled in above via
	// <linux/input-event-codes.h>). Don't conflate the two.
	bool pressed = (flags & SCREEN_FLAG_KEY_DOWN) != 0;

	// SCREEN_PROPERTY_MODIFIERS doesn't reliably reflect Meta/Super on this
	// platform (see qnx_screen_sync_modifiers()); track its scan codes here.
	if (scan == KEY_LEFTMETA) {
		backend->left_gui_state = pressed;
	} else if (scan == KEY_RIGHTMETA) {
		backend->right_gui_state = pressed;
	}

	qnx_screen_sync_modifiers(backend, screen_modifiers);

	struct wlr_keyboard_key_event key_event = {
		.time_msec = get_time_msec(),
		// QNX Screen scan codes on this platform are Linux evdev keycodes;
		// wlr_keyboard's xkb_state (once a keymap is loaded by the
		// compositor) expects the same numbering libinput uses.
		.keycode = (uint32_t)scan,
		// Modifier state is driven exclusively by
		// qnx_screen_sync_modifiers() from SCREEN_PROPERTY_MODIFIERS, not by
		// xkb's own per-keycode tracking: on this platform, discrete
		// key-release events for modifier keys (Ctrl/Alt in particular)
		// aren't reliable enough to drive that tracking, and a missed
		// release leaves the modifier stuck down in the compositor. Passing
		// update_state=true here would let xkb_state_update_key() fight
		// with the modifiers we set explicitly.
		.update_state = false,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED :
			WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(&backend->keyboard, &key_event);
}

static struct wlr_qnx_screen_touchpoint *touchpoint_get(
		struct wlr_qnx_screen_output *output, int screen_id, bool create) {
	struct wlr_qnx_screen_touchpoint *tp;
	wl_list_for_each(tp, &output->touchpoints, link) {
		if (tp->screen_id == screen_id) {
			return tp;
		}
	}
	if (!create) {
		return NULL;
	}

	tp = calloc(1, sizeof(*tp));
	if (tp == NULL) {
		return NULL;
	}
	tp->screen_id = screen_id;

	int next_id = 0;
	struct wlr_qnx_screen_touchpoint *other;
	wl_list_for_each(other, &output->touchpoints, link) {
		if (other->wayland_id >= next_id) {
			next_id = other->wayland_id + 1;
		}
	}
	tp->wayland_id = next_id;

	wl_list_insert(&output->touchpoints, &tp->link);
	return tp;
}

void qnx_screen_handle_touch_event(struct wlr_qnx_screen_backend *backend,
		screen_event_t event, int type) {
	int screen_modifiers = 0;
	screen_get_event_property_iv(event, SCREEN_PROPERTY_MODIFIERS,
		&screen_modifiers);
	qnx_screen_sync_modifiers(backend, screen_modifiers);

	struct wlr_qnx_screen_output *output =
		output_from_event_window(backend, event);
	if (output == NULL) {
		return;
	}

	int touch_id = 0;
	screen_get_event_property_iv(event, SCREEN_PROPERTY_TOUCH_ID, &touch_id);

	int position[2] = { 0, 0 };
	screen_get_event_property_iv(event, SCREEN_PROPERTY_SOURCE_POSITION,
		position);

	int width = output->win_width > 0 ? output->win_width : 1;
	int height = output->win_height > 0 ? output->win_height : 1;
	double x = (double)position[0] / (double)width;
	double y = (double)position[1] / (double)height;

	uint32_t time_msec = get_time_msec();

	// type: 0 = touch (down), 1 = move, 2 = release (up)
	if (type == 0) {
		struct wlr_qnx_screen_touchpoint *tp =
			touchpoint_get(output, touch_id, true);
		if (tp == NULL) {
			return;
		}
		struct wlr_touch_down_event down_event = {
			.touch = &output->touch,
			.time_msec = time_msec,
			.touch_id = tp->wayland_id,
			.x = x,
			.y = y,
		};
		wl_signal_emit_mutable(&output->touch.events.down, &down_event);
	} else if (type == 1) {
		struct wlr_qnx_screen_touchpoint *tp =
			touchpoint_get(output, touch_id, false);
		if (tp == NULL) {
			return;
		}
		struct wlr_touch_motion_event motion_event = {
			.touch = &output->touch,
			.time_msec = time_msec,
			.touch_id = tp->wayland_id,
			.x = x,
			.y = y,
		};
		wl_signal_emit_mutable(&output->touch.events.motion, &motion_event);
	} else {
		struct wlr_qnx_screen_touchpoint *tp =
			touchpoint_get(output, touch_id, false);
		if (tp == NULL) {
			return;
		}
		struct wlr_touch_up_event up_event = {
			.touch = &output->touch,
			.time_msec = time_msec,
			.touch_id = tp->wayland_id,
		};
		wl_signal_emit_mutable(&output->touch.events.up, &up_event);

		wl_list_remove(&tp->link);
		free(tp);
	}

	wl_signal_emit_mutable(&output->touch.events.frame, NULL);
}

void qnx_screen_handle_property_event(struct wlr_qnx_screen_backend *backend,
		screen_event_t event) {
	// Reserved for close-button / visibility / display-hotplug handling.
	// Left as a hook: SCREEN_PROPERTY_WINDOW + SCREEN_PROPERTY_NAME on the
	// event tell you which property of which window changed, e.g. you can
	// detect SCREEN_PROPERTY_ID changes on a screen_display_t to notice
	// display hotplug and call wl_signal_emit_mutable() on the backend's
	// new_output/output destroy paths accordingly.
}

bool wlr_input_device_is_qnx_screen(struct wlr_input_device *wlr_device) {
	switch (wlr_device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		return ((struct wlr_keyboard *)wlr_device)->impl ==
			&qnx_screen_keyboard_impl;
	case WLR_INPUT_DEVICE_POINTER:
		return ((struct wlr_pointer *)wlr_device)->impl ==
			&qnx_screen_pointer_impl;
	case WLR_INPUT_DEVICE_TOUCH:
		return ((struct wlr_touch *)wlr_device)->impl ==
			&qnx_screen_touch_impl;
	default:
		return false;
	}
}

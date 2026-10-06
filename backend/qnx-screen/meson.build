# Self-hosted QNX build: we're compiling natively with clang + lld via a
# plain meson cross/native file that already points at a QNX sysroot
# (screen/screen.h and libscreen.so under the usual /usr/include,
# /usr/lib paths). No QNX_HOST/QNX_TARGET/qcc, no Momentics recipe build.
qnx_screen_libs = []

msg = ['Required for the QNX Screen backend.']
if 'qnx-screen' in backends
	msg += 'Install the QNX Screen (libscreen) development package, or disable the qnx-screen backend.'
endif

screen_dep = cc.find_library('screen',
	required: 'qnx-screen' in backends,
)
if not screen_dep.found()
	warning('\n'.join(msg))
	subdir_done()
endif

if not cc.has_header('screen/screen.h', required: 'qnx-screen' in backends)
	warning('\n'.join(msg))
	subdir_done()
endif

qnx_screen_libs += screen_dep

wlr_files += files(
	'backend.c',
	'event_monitor.c',
	'input_device.c',
	'output.c',
)
wlr_deps += qnx_screen_libs
features += { 'qnx-screen-backend': true }

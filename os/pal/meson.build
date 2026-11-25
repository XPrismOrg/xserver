# XPrism Platform Abstraction Layer, see os/pal/pal.h and doc/pal.md.
# Backends are picked here, at build time.

pal_system = host_machine.system()

pal_platform_names = {
    'linux': 'linux',
    'freebsd': 'freebsd',
    'openbsd': 'openbsd',
    'netbsd': 'netbsd',
    'dragonfly': 'dragonfly',
    'sunos': 'sunos',
    'gnu': 'gnu',
}
pal_input_backends = {
    'linux': 'libinput',
    'freebsd': 'libinput',
    'dragonfly': 'libinput',
    'openbsd': 'wscons',
    'netbsd': 'wscons',
}

srcs_pal = [
    'pal/identity.c',
    'pal/platform.c',
    'pal/process.c',
    'pal/stub/session.c',
]

if pal_system == 'linux'
    srcs_pal += 'pal/linux/process.c'
else
    srcs_pal += 'pal/generic/process.c'
endif

pal_c_args = [
    '-DPAL_PLATFORM_NAME="@0@"'.format(pal_platform_names.get(pal_system, 'unknown')),
    '-DPAL_INPUT_BACKEND="@0@"'.format(pal_input_backends.get(pal_system, 'none')),
]

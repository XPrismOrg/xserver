srcs_composite = [
    'compalloc.c',
    'compcolor.c',
    'compcolorext.c',
    'compext.c',
    'compinit.c',
    'compoverlay.c',
    'compscene.c',
    'compwindow.c',
]

libxserver_composite = static_library('xserver_composite',
    srcs_composite,
    include_directories: inc,
    dependencies: [common_dep, m_dep],   # m: scene transforms
)

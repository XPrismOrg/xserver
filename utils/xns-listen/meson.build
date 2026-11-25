# xns-listen: launcher-side helper for X-NAMESPACE listeners
xns_listen_xcb = dependency('xcb', version: '>= 1.13', required: get_option('xns_listen'))
if xns_listen_xcb.found()
    xns_listen = executable(
        'xns-listen',
        'xns-listen.c',
        dependencies: xns_listen_xcb,
        install: true,
    )
endif

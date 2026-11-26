# xprism-permission-agent: asks the user about missing capabilities
# (X-NAMESPACE 1.2 permission prompts); built along with xns-listen
xprism_agent_xcb = dependency('xcb', version: '>= 1.13', required: get_option('xns_listen'))
if xprism_agent_xcb.found()
    xprism_permission_agent = executable(
        'xprism-permission-agent',
        'xprism-permission-agent.c',
        dependencies: xprism_agent_xcb,
        install: true,
    )
endif

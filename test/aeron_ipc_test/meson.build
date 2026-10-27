if build_aeron_ipc
    if get_option('buildtype') == 'debug'
        aeron_ipc_test_link = tm_transport_lib_debug
    else
        aeron_ipc_test_link = tm_transport_lib
    endif
    executable('aeron_ipc_test', 'aeron_ipc_test.cpp'
        , include_directories: inc
        , link_with: aeron_ipc_test_link
        , dependencies: common_deps + [dependency('protobuf')]
        , install: false)
    executable('aeron_ipc_graph_test', 'aeron_ipc_graph_test.cpp'
        , include_directories: inc
        , link_with: aeron_ipc_test_link
        , dependencies: common_deps + [dependency('protobuf')]
        , install: false)
endif

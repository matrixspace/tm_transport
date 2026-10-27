#tm_kit_transport's static library references protobuf (grpc_interop) without
#declaring it in common_deps, so the test executable adds it explicitly
if get_option('buildtype') == 'debug'
    shm_cursor_test_lib = tm_transport_lib_debug
else
    shm_cursor_test_lib = tm_transport_lib
endif
executable('shm_cursor_test'
    , ['shm_cursor_test.cpp']
    , include_directories: inc
    , dependencies: common_deps + [dependency('protobuf'), declare_dependency(link_args: ['-lrt'])]
    , link_with: shm_cursor_test_lib
    , install: false
)

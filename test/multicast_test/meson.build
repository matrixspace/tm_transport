#tm_kit_transport's static library references protobuf (grpc_interop) without
#declaring it in common_deps, so the test executable adds it explicitly
if get_option('buildtype') == 'debug'
    multicast_test_lib = tm_transport_lib_debug
else
    multicast_test_lib = tm_transport_lib
endif
executable('multicast_test'
    , ['MulticastTest.cpp']
    , include_directories: inc
    , dependencies: common_deps + [dependency('protobuf'), declare_dependency(link_args: ['-lrt'])]
    , link_with: multicast_test_lib
    , install: false
)

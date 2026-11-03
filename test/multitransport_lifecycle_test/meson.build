if get_option('buildtype') == 'debug'
    lifecycle_lib = tm_transport_lib_debug
else
    lifecycle_lib = tm_transport_lib
endif
executable('multitransport_lifecycle_test', 'MultiTransportLifecycleTest.cpp'
    , include_directories: inc
    , dependencies: common_deps + [dependency('protobuf'), declare_dependency(link_args: ['-lrt'])]
    , link_with: lifecycle_lib, install: false)

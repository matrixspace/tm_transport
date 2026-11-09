#tm_transport's static library references protobuf (grpc_interop) but its
#common_deps do not carry it; executables have to add it, as tm_examples does.
th_transport_test_deps = [common_deps, dependency('protobuf'), declare_dependency(link_args: ['-lrt'])]

if get_option('buildtype') == 'debug'
    th_transport_test_lib = tm_transport_lib_debug
else
    th_transport_test_lib = tm_transport_lib
endif

executable('threaded_handler_transport_test'
    , ['ThreadedHandlerTransportTest.cpp']
    , include_directories: inc
    , dependencies: th_transport_test_deps
    , link_with: th_transport_test_lib
    , install: false
)

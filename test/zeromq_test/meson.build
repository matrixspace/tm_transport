#tm_transport's static library references protobuf (grpc_interop) but its
#common_deps do not carry it; executables have to add it, as tm_examples does.
zeromq_test_deps = [common_deps, dependency('protobuf'), declare_dependency(link_args: ['-lrt'])]

if get_option('buildtype') == 'debug'
    zeromq_test_lib = tm_transport_lib_debug
else
    zeromq_test_lib = tm_transport_lib
endif

executable('zeromq_test'
    , ['ZeroMQTest.cpp']
    , include_directories: inc
    , dependencies: zeromq_test_deps
    , link_with: zeromq_test_lib
    , install: false
)

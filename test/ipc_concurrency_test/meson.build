#Concurrency test for the IPC transports (multi-thread / multi-process read and write).
#tm_transport's static library references protobuf (grpc_interop) without
#declaring it in common_deps, so the test executable adds it explicitly.
if get_option('buildtype') == 'debug'
    ipc_concurrency_test_lib = tm_transport_lib_debug
else
    ipc_concurrency_test_lib = tm_transport_lib
endif
executable('ipc_concurrency_test'
    , ['IpcConcurrencyTest.cpp']
    , include_directories: inc
    , dependencies: common_deps + [dependency('protobuf'), declare_dependency(link_args: ['-lrt'])]
    , link_with: ipc_concurrency_test_lib
    , install: false
)

#tm_transport's static library references protobuf (grpc_interop) but its
#common_deps do not carry it; executables have to add it, as tm_examples does.
shm_seqlock_test_deps = [common_deps, dependency('protobuf'), declare_dependency(link_args: ['-lrt'])]

if get_option('buildtype') == 'debug'
    shm_seqlock_test_lib = tm_transport_lib_debug
else
    shm_seqlock_test_lib = tm_transport_lib
endif

executable('shm_seqlock_test'
    , ['ShmSeqlockTest.cpp']
    , include_directories: inc
    , dependencies: shm_seqlock_test_deps
    , link_with: shm_seqlock_test_lib
    , install: false
)

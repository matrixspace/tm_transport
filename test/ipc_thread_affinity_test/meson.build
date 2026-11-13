# tm_kit_transport's static library references protobuf (grpc_interop) without
# declaring it in common_deps, so the test executable adds it explicitly
if get_option('buildtype') == 'debug'
    ipc_thread_affinity_test_lib = tm_transport_lib_debug
else
    ipc_thread_affinity_test_lib = tm_transport_lib
endif
ipc_thread_affinity_test_deps = common_deps + [dependency('protobuf'), declare_dependency(link_args: ['-lrt'])]
ipc_thread_affinity_test_cpp_args = []
hwloc_test_dep = dependency('hwloc', required: false)
if hwloc_test_dep.found()
    ipc_thread_affinity_test_deps += hwloc_test_dep
    ipc_thread_affinity_test_cpp_args += ['-DTEST_HAS_HWLOC']
endif
executable('ipc_thread_affinity_test'
    , ['IpcThreadAffinityTest.cpp']
    , include_directories: inc
    , dependencies: ipc_thread_affinity_test_deps
    , cpp_args: ipc_thread_affinity_test_cpp_args
    , link_with: ipc_thread_affinity_test_lib
    , install: false
)

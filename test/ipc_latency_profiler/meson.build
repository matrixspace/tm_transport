#tm_transport's static library references protobuf (grpc_interop) but its
#common_deps do not carry it; executables have to add it, as tm_examples does.
ipc_latency_profiler_deps = [common_deps, dependency('protobuf'), declare_dependency(link_args: ['-lrt'])]

if get_option('buildtype') == 'debug'
    ipc_latency_profiler_lib = tm_transport_lib_debug
else
    ipc_latency_profiler_lib = tm_transport_lib
endif

executable('ipc_latency_profiler'
    , ['IpcLatencyProfiler.cpp', 'ProfilerCommon.cpp', 'ProfilerFloor.cpp']
    , include_directories: inc
    , dependencies: ipc_latency_profiler_deps
    , link_with: ipc_latency_profiler_lib
    , install: false
)

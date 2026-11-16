# Focused integration build: intentionally excludes unrelated network backends.
project('shm-seqlock-system-suite', 'cpp', default_options: ['cpp_std=c++17', 'warning_level=2'])
root = '../../../'
install_prefix = run_command('sh', '-c', 'printf "%s" "$INSTALL_PREFIX"', check: true).stdout()
vcpkg_root = run_command('sh', '-c', 'printf "%s" "$VCPKG_ROOT"', check: true).stdout()
if install_prefix == '' or vcpkg_root == ''
    error('source build-common/build-env-setting.sh before configuring')
endif
inc = include_directories(root, root+'external/certify/include', install_prefix/'include', vcpkg_root/'installed/x64-linux/include')
cpp = meson.get_compiler('cpp')
system_deps = [dependency('threads'), cpp.find_library('rt')]
basic_name = get_option('buildtype') == 'debug' ? 'tm_kit_basic_debug' : 'tm_kit_basic'
basic_dep = dependency(basic_name)
component_sources = [root+'src/shm_seqlock/ShmSeqlockComponent.cpp', root+'src/ConnectionLocator.cpp']
production_component = static_library('production_shm_seqlock', component_sources, include_directories: inc, dependencies: system_deps)
instrumented_component = static_library('instrumented_shm_seqlock', component_sources, include_directories: inc,
    cpp_args: ['-DTM_KIT_SHM_SEQLOCK_TEST_PROBES=1'], dependencies: system_deps)
executable('shm_seqlock_system_worker', '../SystemTestWorker.cpp', include_directories: inc, dependencies: system_deps,
    cpp_args: ['-DTM_KIT_SHM_SEQLOCK_TEST_PROBES=1'], link_with: instrumented_component)
executable('shm_seqlock_blackbox_worker', '../SystemTestWorker.cpp', include_directories: inc, dependencies: system_deps, link_with: production_component)
executable('shm_seqlock_graph_system_test', '../../shm_seqlock_graph_system_test/ShmSeqlockGraphSystemTest.cpp',
    include_directories: inc, dependencies: system_deps+[basic_dep], link_with: production_component)
executable('shm_seqlock_heartbeat_system_worker',
    ['../../shm_seqlock_heartbeat_system_test/HeartbeatSystemWorker.cpp', root+'src/HeartbeatMessage.cpp', root+'src/AlertMessage.cpp', root+'src/HeartbeatAndAlertComponent.cpp', root+'src/HostNameUtil.cpp'],
    include_directories: inc, dependencies: system_deps+[basic_dep, dependency('protobuf'), dependency('uuid')], link_with: production_component)
executable('shm_seqlock_resource_system_test', '../../shm_seqlock_resource_system_test/ResourceSystemTest.cpp',
    cpp_args: ['-DTM_SHM_RESOURCE_BUILD_TYPE="'+get_option('buildtype')+'"'],
    include_directories: inc, dependencies: system_deps, link_with: production_component)
executable('shm_seqlock_stability_worker', '../../shm_seqlock_stability_test/StabilityWorker.cpp',
    include_directories: inc, dependencies: system_deps, link_with: production_component)
executable('shm_seqlock_graph_bench', '../../shm_seqlock_graph_bench/ShmSeqlockGraphBench.cpp',
    include_directories: inc, dependencies: system_deps+[basic_dep], link_with: production_component)
executable('shm_seqlock_test', '../../shm_seqlock_test/ShmSeqlockTest.cpp', include_directories: inc,
    dependencies: system_deps+[basic_dep], link_with: production_component)
executable('ipc_concurrency_test', ['../../ipc_concurrency_test/IpcConcurrencyTest.cpp', root+'src/shm_cursor/ShmCursorComponent.cpp', root+'src/shared_memory_broadcast/SharedMemoryBroadcastComponent.cpp'],
    include_directories: inc, dependencies: system_deps+[basic_dep], link_with: production_component)

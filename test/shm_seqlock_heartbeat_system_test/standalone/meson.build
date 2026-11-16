project('shm-seqlock-heartbeat-system-test', 'cpp', default_options: ['cpp_std=c++17', 'warning_level=2'])
root = '../../../'
basic_name = get_option('buildtype') == 'debug' ? 'tm_kit_basic_debug' : 'tm_kit_basic'
executable('shm_seqlock_heartbeat_system_worker',
    ['../HeartbeatSystemWorker.cpp', root+'src/shm_seqlock/ShmSeqlockComponent.cpp',
     root+'src/ConnectionLocator.cpp', root+'src/HeartbeatMessage.cpp',
     root+'src/AlertMessage.cpp', root+'src/HeartbeatAndAlertComponent.cpp', root+'src/HostNameUtil.cpp'],
    include_directories: include_directories(root, root+'external/certify/include'),
    dependencies: [dependency('threads'), dependency(basic_name), dependency('protobuf'), dependency('uuid')],
    cpp_args: ['-Wno-unknown-pragmas', '-Wno-unused-function', '-Wno-reorder'],
    link_args: ['-lrt'], install: false)

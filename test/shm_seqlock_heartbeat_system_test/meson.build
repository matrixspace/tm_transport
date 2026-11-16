# Compile the tested sources directly to keep this executable independent of
# unrelated network backends and of the leader's instrumented component target.
executable('shm_seqlock_heartbeat_system_worker',
    ['HeartbeatSystemWorker.cpp', '../../src/shm_seqlock/ShmSeqlockComponent.cpp',
     '../../src/ConnectionLocator.cpp', '../../src/HeartbeatMessage.cpp',
     '../../src/AlertMessage.cpp', '../../src/HeartbeatAndAlertComponent.cpp', '../../src/HostNameUtil.cpp'],
    include_directories: inc,
    dependencies: common_deps + [dependency('protobuf'), declare_dependency(link_args: ['-lrt'])],
    install: false)

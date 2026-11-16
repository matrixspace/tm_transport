# Intentionally compiles the real component without any fault-probe macro.
executable('shm_seqlock_resource_system_test',
    ['ResourceSystemTest.cpp', '../../src/shm_seqlock/ShmSeqlockComponent.cpp', '../../src/ConnectionLocator.cpp'],
    cpp_args: ['-DTM_SHM_RESOURCE_BUILD_TYPE="' + get_option('buildtype') + '"'],
    include_directories: inc,
    dependencies: [thread_dep, boost_dep, tm_infra_dep, tm_basic_dep, declare_dependency(link_args: ['-lrt'])],
    install: false)

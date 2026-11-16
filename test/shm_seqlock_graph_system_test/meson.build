# Keep this graph suite independent of the large transport archive: only the
# component under test and locator implementation are compiled into this target.
executable('shm_seqlock_graph_system_test',
    ['ShmSeqlockGraphSystemTest.cpp', '../../src/shm_seqlock/ShmSeqlockComponent.cpp', '../../src/ConnectionLocator.cpp'],
    include_directories: inc,
    dependencies: [thread_dep, tm_basic_dep, declare_dependency(link_args: ['-lrt'])],
    install: false)

# The production library never receives TM_KIT_SHM_SEQLOCK_TEST_PROBES.
# Compile only the component and locator needed by this isolated process driver.
shm_system_sources = ['SystemTestWorker.cpp', '../../src/shm_seqlock/ShmSeqlockComponent.cpp', '../../src/ConnectionLocator.cpp']
shm_system_deps = [thread_dep, boost_dep, tm_infra_dep, tm_basic_dep, declare_dependency(link_args: ['-lrt'])]
executable('shm_seqlock_system_worker', shm_system_sources,
    cpp_args: ['-DTM_KIT_SHM_SEQLOCK_TEST_PROBES=1'],
    include_directories: inc, dependencies: shm_system_deps, install: false)
executable('shm_seqlock_blackbox_worker', shm_system_sources,
    include_directories: inc, dependencies: shm_system_deps, install: false)

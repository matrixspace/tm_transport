project('shm-seqlock-system-tests', 'cpp', default_options: ['cpp_std=c++17', 'warning_level=3'])
cpp = meson.get_compiler('cpp')
inc = include_directories('../../..', get_option('tm_include'), get_option('boost_include'))
deps = [dependency('threads'), cpp.find_library('rt')]
sources = ['../SystemTestWorker.cpp', '../../../src/shm_seqlock/ShmSeqlockComponent.cpp', '../../../src/ConnectionLocator.cpp']
executable('shm_seqlock_system_worker', sources, include_directories: inc, dependencies: deps,
    cpp_args: ['-DTM_KIT_SHM_SEQLOCK_TEST_PROBES=1'])
executable('shm_seqlock_blackbox_worker', sources, include_directories: inc, dependencies: deps)

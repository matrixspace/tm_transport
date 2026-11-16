project('shm-seqlock-graph-bench', 'cpp', default_options: ['cpp_std=c++17', 'warning_level=2'])
if get_option('buildtype') == 'debug'
    basic_dep = dependency('tm_kit_basic_debug')
else
    basic_dep = dependency('tm_kit_basic')
endif
executable('shm_seqlock_graph_bench',
    ['../ShmSeqlockGraphBench.cpp', '../../../src/shm_seqlock/ShmSeqlockComponent.cpp', '../../../src/ConnectionLocator.cpp'],
    include_directories: include_directories('../../..', '../../../external/certify/include'),
    dependencies: [dependency('threads'), basic_dep],
    link_args: ['-lrt'], install: false)

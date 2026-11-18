db_poll_stop_exe = executable('db_poll_stop_test', 'DBPollStopTest.cpp'
    , include_directories: inc
    , dependencies: [thread_dep, boost_dep, soci_dep, tm_infra_dep, tm_basic_dep]
    , install: false
)
test('db_poll_stop_test', db_poll_stop_exe, timeout: 30)

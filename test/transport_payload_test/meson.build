if get_option('buildtype') == 'debug'
    payload_test_lib = tm_transport_lib_debug
else
    payload_test_lib = tm_transport_lib
endif
payload_test_exe = executable('transport_payload_test', 'TransportPayloadTest.cpp'
    , include_directories: inc
    , dependencies: common_deps + [dependency('protobuf'), declare_dependency(link_args: ['-lrt'])]
    , link_with: payload_test_lib
    , install: false)
test('transport_payload_test', payload_test_exe, timeout: 180)

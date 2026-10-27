project('TM kit transport', 'cpp', default_options: ['cpp_std=c++17', 'warning_level=3'])

inc = include_directories('.', 'external/certify/include')
thread_dep = dependency('threads')
boost_dep = dependency('boost')
rabbitmq_dep = dependency('librabbitmq')
zmq_dep = [dependency('libzmq'), dependency('cppzmq')]
redis_dep = [dependency('hiredis'), dependency('hiredis_ssl', required: false)]
nng_dep = [dependency('nngpp')]
crossguid_dep = dependency('crossguid')
grpc_dep = dependency('grpc')
etcd_dep = dependency('libetcdcpp')
soci_dep = dependency('soci')
libsodium_dep = dependency('libsodium')
curlpp_dep = dependency('curlpp')
ixwebsocket_dep = dependency('ixwebsocket')
libnats_dep = dependency('libnats')
if get_option('buildtype') == 'debug'
    tm_infra_dep = dependency('tm_kit_infra_debug')
    tm_basic_dep = dependency('tm_kit_basic_debug')
else
    tm_infra_dep = dependency('tm_kit_infra')
    tm_basic_dep = dependency('tm_kit_basic')
endif

common_deps = [thread_dep, boost_dep, rabbitmq_dep, zmq_dep, redis_dep, nng_dep, crossguid_dep, grpc_dep, etcd_dep, soci_dep, libsodium_dep, tm_infra_dep, tm_basic_dep, curlpp_dep, ixwebsocket_dep, libnats_dep]

if build_machine.system() == 'windows'
    common_deps += declare_dependency(link_args: 'bcrypt.lib')
    common_deps += declare_dependency(link_args: 'crypt32.lib')
    add_global_arguments('/bigobj', language: 'cpp')
    add_global_arguments('-DWINVER=0x0601', language: 'cpp')
    add_global_arguments('-D_WIN32_WINNT=0x0601', language: 'cpp')
    add_global_arguments('-DBOOST_USE_WINAPI_VERSION=BOOST_WINAPI_VERSION_WIN7', language: 'cpp')
endif

if build_machine.system() == 'linux'
    add_global_arguments('-Wno-unknown-pragmas', language: 'cpp')
    add_global_arguments('-Wno-unused-function', language: 'cpp')
    add_global_arguments('-Wno-reorder', language: 'cpp')
    if get_option('buildtype') != 'release'
        add_global_arguments('-fno-omit-frame-pointer', language: 'cpp')
        add_global_arguments('-fno-omit-frame-pointer', language: 'c')
    endif
endif

pkg = import('pkgconfig')

#Aeron is compiled into tm_kit_transport itself when the dependency can be found
#(Linux only). When it is not found, the aeron_ipc sources are not compiled and the
#shared transport headers gate every aeron_ipc reference with TM_KIT_TRANSPORT_AERON_IPC.
aeron_ipc_deps = []
build_aeron_ipc = false
if host_machine.system() == 'linux'
    aeron_dep = dependency('aeron', method: 'cmake'
        , modules: ['aeron::aeron_client_wrapper', 'aeron::aeron_static', 'aeron::aeron_driver_static']
        , required: false)
    uuid_dep = dependency('uuid', required: false)
    if aeron_dep.found() and uuid_dep.found()
        aeron_ipc_deps = [aeron_dep, uuid_dep]
        build_aeron_ipc = true
        common_deps += aeron_ipc_deps
        add_global_arguments('-DTM_KIT_TRANSPORT_AERON_IPC=1', language: 'cpp')
    endif
endif

subdir('build_environment_tools')
subdir('tm_kit/transport')
subdir('src')
subdir('test')

tm_transport_sources = [
      'ConnectionLocator.cpp'
      , 'HeartbeatMessage.cpp'
      , 'AlertMessage.cpp'
      , 'HeartbeatAndAlertComponent.cpp'
      , 'multicast/InterfaceToIP.cpp'
      , 'multicast/MulticastComponent.cpp'
      , 'rabbitmq/RabbitMQComponent.cpp'
      , 'zeromq/ZeroMQComponent.cpp'
      , 'redis/RedisComponent.cpp'
      , 'nats/NATSComponent.cpp'
      , 'nng/NNGComponent.cpp'
      , 'security/SignatureHelper.cpp'
      , 'shared_memory_broadcast/SharedMemoryBroadcastComponent.cpp'
      , 'socket_rpc/SocketRPCComponent.cpp'
      , 'grpc_interop/GrpcInteropComponent.cpp'
      , 'grpc_interop/GrpcConnectionLocatorUtils.cpp'
      , 'json_rest/JsonRESTComponent.cpp'
      , 'websocket/WebSocketComponent.cpp'
      , 'HostNameUtil.cpp'
      , 'ConvertChainIDStringToGroup.cpp'
      , tm_transport_grpc_version_info_hpp
      , 'singlecast/SinglecastComponent.cpp'
      , 'BoostCertifyAdaptor.cpp'
]
if build_machine.system() == 'linux'
      tm_transport_sources += ['shm_seqlock/ShmSeqlockComponent.cpp']
      tm_transport_sources += ['shm_cursor/ShmCursorComponent.cpp']
endif
if build_aeron_ipc
      tm_transport_sources += ['aeron_ipc/AeronIpcComponent.cpp']
endif
aeron_ipc_pc_cflags = []
if build_aeron_ipc
    aeron_ipc_pc_cflags = ['-DTM_KIT_TRANSPORT_AERON_IPC=1']
endif
if get_option('buildtype') == 'debug'
      tm_transport_lib_debug = static_library('tm_kit_transport_debug', tm_transport_sources, include_directories: inc, dependencies: common_deps, install: true)
      pkg.generate(tm_transport_lib_debug, extra_cflags: aeron_ipc_pc_cflags)
else
      tm_transport_lib = static_library('tm_kit_transport', tm_transport_sources, include_directories: inc, dependencies: common_deps, install: true)
      pkg.generate(tm_transport_lib, extra_cflags: aeron_ipc_pc_cflags)
endif


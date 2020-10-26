This is unstable software, with absolutely no warranty or guarantee whatsoever, whether express or implied, including but not limited to those of merchantability, fitness for specific purpose, or non-infringement.

This package builds upon tm_infra and tm_basic, and provides the following pre-packaged functionalities for codes written with those two packages:

* Tagging with UUID

* Communications through
  
  - RabbitMQ (via rabbitmq-c and SimpleAmqpClient libraries)

  - Multicast (via boost asio library)

  - ZeroMQ (via zmq and cppzmq libraries)

  - Redis (via hiredis library)

* Shared chain implemented with

  - Etcd3 (via Offscale libetcd library)

  - Redis (via hiredis library)

  - Shared memory (via boost interprocess library)

* Heartbeat and alert support, and service discovery through heartbeat

* Certain pre-defined node combinations

* Digital signature based identity attaching in facility calls

INSTALLATION NOTES:

The requirements of tm_transport are, in addition to requirements of tm_infra and tm_basic:

* tm_infra and tm_basic

* boost >= 1.73.0

* librabbitmq-c (https://github.com/alanxz/rabbitmq-c)

* SimpleAmqpClient (https://github.com/alanxz/SimpleAmqpClient)

* libzmq

* CppZmq (https://github.com/zeromq/cppzmq)

* hiredis

* libnng

* nngpp (https://github.com/cwzx/nngpp)

* CrossGuid (https://github.com/graeme-hill/crossguid)

* Offscale Etcd C++ client library (https://github.com/offscale/libetcd-cpp)

* libsodium

Most of these can be installed through vcpkg. SimpleAmqpClient does not seem to have a vcpkg package as of the date of this writing, but can be installed from source by following the instructions of the package site. After installing the packages through vcpkg, sometimes a hand-written pkg-config file might be needed for meson to find the package, especially on Windows.

On Ubuntu 18.04 at least, if Offscale Etcd C++ client library is built through vcpkg using the default toolchain (gcc), then clang will not be able to use the library in linking. However, if it is build through vcpkg using a customized toolchain that uses clang (11.0.0 tested), then both clang and gcc (10.1.0 tested) will be able to use it in the linker. 

The Typescript code included in this package has been tested with Nodejs 14.12.0 and Typescript 3.9.3. (For possible issues with etcd3 package, please refer to the comment at the beginning of TMTransport.ts)

The Python code included in this package has been tested with Python 3.9.0rc1.

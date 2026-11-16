# Configure after sourcing the workspace's build-common/build-env-setting.sh.
# This focused target avoids unrelated transport dependency discovery.
project('Shm seqlock resource system test', 'cpp', default_options: ['cpp_std=c++17', 'warning_level=3'])
install_prefix = run_command('sh', '-c', 'printf "%s" "$INSTALL_PREFIX"', check: true).stdout()
vcpkg_root = run_command('sh', '-c', 'printf "%s" "$VCPKG_ROOT"', check: true).stdout()
if install_prefix == '' or vcpkg_root == ''
    error('source build-common/build-env-setting.sh before configuring')
endif
resource_inc = include_directories('../../..', install_prefix / 'include', vcpkg_root / 'installed/x64-linux/include')
cpp = meson.get_compiler('cpp')
executable('shm_seqlock_resource_system_test',
    ['../ResourceSystemTest.cpp', '../../../src/shm_seqlock/ShmSeqlockComponent.cpp', '../../../src/ConnectionLocator.cpp'],
    cpp_args: ['-DTM_SHM_RESOURCE_BUILD_TYPE="' + get_option('buildtype') + '"'],
    include_directories: resource_inc,
    dependencies: [dependency('threads'), cpp.find_library('rt')],
    install: false)

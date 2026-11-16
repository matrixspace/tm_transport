# Configure after sourcing the workspace's build-common/build-env-setting.sh.
# The stability worker links the production component with no fault probes.
project('Shm seqlock stability test', 'cpp', default_options: ['cpp_std=c++17', 'warning_level=3'])
install_prefix = run_command('sh', '-c', 'printf "%s" "$INSTALL_PREFIX"', check: true).stdout()
vcpkg_root = run_command('sh', '-c', 'printf "%s" "$VCPKG_ROOT"', check: true).stdout()
if install_prefix == '' or vcpkg_root == ''
    error('source build-common/build-env-setting.sh before configuring')
endif
stability_inc = include_directories('../../..', install_prefix / 'include', vcpkg_root / 'installed/x64-linux/include')
cpp = meson.get_compiler('cpp')
executable('shm_seqlock_stability_worker',
    ['../StabilityWorker.cpp', '../../../src/shm_seqlock/ShmSeqlockComponent.cpp', '../../../src/ConnectionLocator.cpp'],
    include_directories: stability_inc,
    dependencies: [dependency('threads'), cpp.find_library('rt')],
    install: false)

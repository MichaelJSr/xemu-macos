project('dsp56300', version: '0.1.3')

fs = import('fs')

host_cpu = host_machine.cpu_family()
host_os = host_machine.system()

if host_os == 'darwin'
  os_suffix = 'apple-darwin'
elif host_os == 'windows'
  os_suffix = (host_cpu == 'aarch64') ? 'pc-windows-gnullvm' : 'pc-windows-gnu'
elif host_os == 'linux'
  os_suffix = 'unknown-linux-gnu'
else
  error('Unsupported host OS: ' + host_os)
endif
rust_target = host_cpu + '-' + os_suffix

ver = meson.project_version()
prefix = 'dsp56300-@0@-@1@'.format(ver, rust_target)
archive = '@0@.tar.gz'.format(prefix)
url = 'https://github.com/mborgerson/dsp56300/releases/download/v@0@/@1@'.format(ver, archive)
lib = meson.current_build_dir() / prefix / 'lib' / 'libdsp56300_emu_ffi.a'

if not fs.exists(lib)
  src_archive = meson.current_source_dir() / archive
  if not fs.exists(src_archive)
    message('Downloading dsp56300 @0@ for @1@...'.format(ver, rust_target))
    run_command(find_program('curl'), '-fsSL', url, '-o', src_archive, check: true)
  endif
  tar_args = ['xzf', src_archive, '-C', meson.current_build_dir()]
  if build_machine.system() == 'windows'
    # MSYS2/GNU tar parses "C:/path" as host:path (remote archive);
    # --force-local keeps it a file. This is a property of the tar
    # meson invokes -- i.e. the BUILD machine, not the target: the
    # paths above come from meson.current_{source,build}_dir(), which
    # are only Windows-absolute when meson itself runs on Windows. A
    # Windows cross build hosted on macOS would otherwise hand
    # --force-local to bsdtar, which lacks the option, and check: true
    # turns that into a hard configure error.
    tar_args += ['--force-local']
  endif
  run_command(find_program('tar'), tar_args, check: true)
endif

link_args = []
if host_os == 'linux'
  link_args = ['-lpthread', '-ldl', '-lm']
elif host_os == 'windows'
  link_args = ['-lbcrypt', '-lntdll', '-luserenv', '-lws2_32']
elif host_os == 'darwin'
  link_args = ['-lpthread', '-lm', '-framework', 'Security']
endif

dsp56300_dep = declare_dependency(
  link_args: [lib] + link_args,
  include_directories: include_directories(prefix / 'include'),
)

meson.override_dependency('dsp56300-emu-ffi', dsp56300_dep)

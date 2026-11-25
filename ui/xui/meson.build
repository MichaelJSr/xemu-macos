xemu_ss.add(files(
  'actions.cc',
  'animation.cc',
  'compat.cc',
  'debug.cc',
  'disc-image.cc',
  'font-manager.cc',
  'game-library.cc',
  'game-library-window.cc',
  'game-patches.cc',
  'game-patches-window.cc',
  'gl-helpers.cc',
  'input-manager.cc',
  'main-menu.cc',
  'main.cc',
  'misc.cc',
  'menubar.cc',
  'monitor.cc',
  'notifications.cc',
  'popup-menu.cc',
  'reporting.cc',
  'scene-components.cc',
  'scene-manager.cc',
  'scene.cc',
  'snapshot-manager.cc',
  'viewport-manager.cc',
  'welcome.cc',
  'widgets.cc',
  'xbox-drive.cc',
  'xbox-drive-explorer.cc',
  'xbox-drive-qemu.cc',
  'xbox-hdd-blk.c',
))

xemu_ss.add([curl, files('github-issue.cc')])

if host_os == 'windows'
  xemu_ss.add(files('win32-dxgi-present.cc'))
endif

if host_os == 'darwin'
  # Metal twin of gl-helpers (decals, compositor, ImGui Metal bridge)
  xemu_ss.add(files('metal-helpers.mm'))
endif

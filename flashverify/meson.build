project('flashverify', 'c',
  version: '1.0.0',
  default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')

ssl  = dependency('openssl',  required: true)
glib = dependency('glib-2.0', required: false)
gtk4 = dependency('gtk4',     required: false)

# suppress GTK4 deprecation warnings (GtkComboBox, GtkMessageDialog still work)
add_project_arguments('-DGDK_DISABLE_DEPRECATION_WARNINGS', language: 'c')
add_project_arguments('-DGTK_DISABLE_DEPRECATION_WARNINGS', language: 'c')
add_project_arguments('-Wno-deprecated-declarations', language: 'c')

src_core = files(
  'src/main.c',
  'src/rawio.c',
  'src/device.c',
  'src/test.c',
  'src/hash.c',
  'src/random.c',
  'src/report.c',
)

deps = [ssl]

if gtk4.found()
  message('GTK4 found — building with GUI')
  deps += gtk4
  add_project_arguments('-DWITH_GTK4', language: 'c')
  src_core += files('src/ui.c')
else
  message('GTK4 NOT found — building CLI-only')
endif

if glib.found()
  deps += glib
endif

if host_machine.system() == 'windows'
  deps += cc.find_library('advapi32', required: true)
  deps += cc.find_library('setupapi', required: true)
endif

# Windows: embed UAC manifest (requireAdministrator)
if host_machine.system() == 'windows'
  windows = import('windows')
  src_core += windows.compile_resources('src/flashverify.rc')
endif

executable('flashverify', src_core,
  dependencies: deps,
  install: true)

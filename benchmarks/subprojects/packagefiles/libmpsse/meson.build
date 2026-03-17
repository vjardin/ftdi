project('libmpsse', 'c',
  version : '1.4',
  default_options : ['warning_level=1', 'c_std=gnu11'],
)

libftdi1 = dependency('libftdi1')

libmpsse_lib = static_library('mpsse',
  'src/mpsse.c',
  'src/support.c',
  'src/fast.c',
  c_args : ['-DLIBFTDI1=1', '-Wno-deprecated-declarations',
             '-Wno-implicit-function-declaration',
             '-Wno-builtin-declaration-mismatch'],
  dependencies : [libftdi1],
  override_options : ['warning_level=0'],
)

libmpsse_dep = declare_dependency(
  include_directories : include_directories('src'),
  link_with : libmpsse_lib,
  dependencies : [libftdi1],
  compile_args : ['-DLIBFTDI1=1'],
)

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 Vincent Jardin <vjardin@free.fr>

project('i2c-benchmarks', 'c',
  version : '1.0',
  license : 'AGPL-3.0-or-later',
  default_options : ['warning_level=2', 'c_std=gnu11'],
)

# --- kernel driver benchmark (ioctl I2C_RDWR) ---

executable('i2c_bench_kernel',
  'i2c_bench_kernel.c',
  install : false,
)

# --- libmpsse benchmark (userspace USB) ---

libmpsse_proj = subproject('libmpsse')
libmpsse_dep = libmpsse_proj.get_variable('libmpsse_dep')

executable('i2c_bench_libmpsse',
  'i2c_bench_libmpsse.c',
  dependencies : [libmpsse_dep],
  install : false,
)

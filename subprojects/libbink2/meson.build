project('libbink2', 'c',
  version: '0.1.0',
  license: 'LGPL-2.1-or-later',
  meson_version: '>=1.1',
  default_options: ['c_std=c99', 'warning_level=1', 'werror=false'],
)

cc = meson.get_compiler('c')
libm = cc.find_library('m', required: false)

# The flags upstream builds the decoder with; the decoder's float math is written for them.
libbink2 = static_library('bink2',
  'src/bink2.c',
  'src/bink2_audio.c',
  'src/bink2_bits.c',
  'src/bink2_dsp.c',
  'src/bink2_frames.c',
  'src/bink2_idct.c',
  'src/bink2_tables.c',
  'src/bink2_video.c',
  'src/bink2_video_new.c',
  'src/bink2_video_old.c',
  c_args: ['-fno-math-errno', '-fno-signed-zeros'] + cc.get_supported_arguments('-Wno-array-parameter'),
  dependencies: libm,
  # Debug builds keep the decoder fast enough to play video; the debug information stays.
  override_options: ['optimization=2'],
  # Not installed: the pinned copy is linked into the program.
  install: false,
)

libbink2_dep = declare_dependency(
  link_with: libbink2,
  include_directories: include_directories('src', is_system: true),
)

# The license ships with the program that links the library.
install_data('LICENSE', rename: 'libbink2-LICENSE', install_dir: get_option('datadir') / 'licenses/luciole')

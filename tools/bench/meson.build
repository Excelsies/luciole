# Headless benchmark drivers; built only with -Dbenchmarks=true and never installed.
executable('luciole-listing-bench',
  'listing_bench.cpp',
  include_directories: src_inc,
  dependencies: [browser_dep, event_loop_dep, noctalia_ui_dep, gio],
)
executable('luciole-transfer-bench',
  'transfer_bench.cpp',
  include_directories: src_inc,
  dependencies: [operations_dep, event_loop_dep, noctalia_ui_dep, gio],
)
if media_playback
  executable('luciole-bink2-bench',
    'bink2_bench.cpp',
    'bink2_bench_play.cpp',
    include_directories: src_inc,
    dependencies: [media_dep],
  )
endif

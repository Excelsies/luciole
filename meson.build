project('luciole', ['c', 'cpp'],
  version: files('VERSION'),
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=3',
    'werror=true',
    'b_ndebug=if-release',
  ],
)

cpp = meson.get_compiler('cpp')

wayland_protocols = dependency('wayland-protocols', version: '>=1.49')
scanner = find_program('wayland-scanner')
protocol_root = wayland_protocols.get_variable('pkgdatadir')
protocol_sources = []
protocol_headers = []
foreach protocol : [
  ['xdg-decoration-unstable-v1', 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'],
  ['xdg-foreign-unstable-v2', 'unstable/xdg-foreign/xdg-foreign-unstable-v2.xml'],
  ['xdg-dialog-v1', 'staging/xdg-dialog/xdg-dialog-v1.xml'],
]
  header = custom_target(protocol[0] + '-header',
    input: protocol_root / protocol[1],
    output: protocol[0] + '-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  source = custom_target(protocol[0] + '-source',
    input: protocol_root / protocol[1],
    output: protocol[0] + '-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    depends: header,
  )
  protocol_sources += source
  protocol_headers += header
endforeach
alias_target('generated-headers', protocol_headers)

gio = dependency('gio-2.0', version: '>=2.76')
gio_unix = dependency('gio-unix-2.0', version: '>=2.76')
curl = dependency('libcurl', version: '>=7.85')
xml = dependency('libxml-2.0', version: '>=2.9')
gdk_pixbuf = dependency('gdk-pixbuf-2.0', version: '>=2.42')
glycin = dependency('glycin-2', version: '>=2.0.1')
if not cpp.has_header('stb/stb_image_resize2.h')
  error('system stb/stb_image_resize2.h is required')
endif
tomlplusplus = dependency('tomlplusplus', version: '>=3.4')

if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

# Recent GLib headers use __COUNTER__, which clang 23 rejects as a C2y extension under
# -Wpedantic -Werror. lib/ adds GLib with -I and its own -Wpedantic after project arguments,
# so mark the GLib include directory as a system directory instead (-isystem wins over -I).
add_project_arguments('-isystem', dependency('glib-2.0').get_variable(pkgconfig: 'includedir') / 'glib-2.0', language: 'cpp')

if get_option('native_optimizations')
  add_project_arguments('-march=native', '-mtune=native', language: 'cpp')
endif

if get_option('buildtype') == 'release'
  add_project_arguments('-ffunction-sections', '-fdata-sections', language: 'cpp')
  add_project_link_arguments('-Wl,--gc-sections', '-Wl,--as-needed', language: 'cpp')
else
  add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
endif

subdir('lib')

build_config = configuration_data()
build_config.set_quoted('LUCIOLE_VERSION', meson.project_version())
build_config.set_quoted('LUCIOLE_BUILD_REVISION', get_option('build_revision'))
build_config.set_quoted('LUCIOLE_BUILD_TYPE', get_option('buildtype'))
build_config.set_quoted('LUCIOLE_COMPILER', cpp.get_id() + ' ' + cpp.version())
build_config.set_quoted('LUCIOLE_INSTALL_ASSETSDIR', get_option('prefix') / get_option('datadir') / 'luciole/assets')
build_info = configure_file(
  input: 'src/app/build_info.h.in',
  output: 'luciole_build_info.h',
  configuration: build_config,
)
fs = import('fs')
completion_config = configuration_data()
completion_config.set('BASH_COMPLETIONS', fs.read('assets/completions/luciole.bash'))
completion_config.set('ZSH_COMPLETIONS', fs.read('assets/completions/_luciole'))
completion_config.set('FISH_COMPLETIONS', fs.read('assets/completions/luciole.fish'))
completions = configure_file(
  input: 'src/app/completion_scripts.h.in',
  output: 'luciole_completions.h',
  configuration: completion_config,
)
app_icon = configure_file(
  input: 'src/app/app_icon.h.in',
  output: 'luciole_app_icon.h',
  configuration: {'APP_ICON': fs.read('assets/icons/dev.noctalia.luciole.svg')},
)
src_inc = include_directories('src')

# One static library per module. A module's dependencies list only the modules
# its sources include, following the layering in CONTRIBUTING.md. Modules that
# must not see lib/ (fs, model, config, browser, operations, workbench, formats, editing)
# never depend on noctalia_ui_dep, so including a lib/ header from them fails to compile.
model_dep = declare_dependency(include_directories: src_inc)

workbench_lib = static_library('luciole-workbench',
  files(
    'src/workbench/atlas_camera.cpp',
    'src/workbench/atlas_hud.cpp',
    'src/workbench/atlas_label_shapes.cpp',
    'src/workbench/atlas_labels.cpp',
    'src/workbench/atlas_motion.cpp',
    'src/workbench/atlas_navigation.cpp',
    'src/workbench/atlas_scene.cpp',
    'src/workbench/atlas_text.cpp',
    'src/workbench/audio_analysis.cpp',
    'src/workbench/audio_history.cpp',
    'src/workbench/bar_scene.cpp',
    'src/workbench/circle_pack.cpp',
    'src/workbench/color.cpp',
    'src/workbench/column_plan.cpp',
    'src/workbench/corona_field.cpp',
    'src/workbench/entry_text.cpp',
    'src/workbench/flame_bands.cpp',
    'src/workbench/flame_row.cpp',
    'src/workbench/flame_scene.cpp',
    'src/workbench/flame_strata.cpp',
    'src/workbench/flow_hit.cpp',
    'src/workbench/flow_layout.cpp',
    'src/workbench/flow_stem.cpp',
    'src/workbench/flow_topology.cpp',
    'src/workbench/format.cpp',
    'src/workbench/halo_field.cpp',
    'src/workbench/hit_flash.cpp',
    'src/workbench/iris_facts.cpp',
    'src/workbench/iris_hit.cpp',
    'src/workbench/iris_keys.cpp',
    'src/workbench/iris_overlay.cpp',
    'src/workbench/iris_overlay_shapes.cpp',
    'src/workbench/iris_scene.cpp',
    'src/workbench/jump_rank.cpp',
    'src/workbench/ladder_plan.cpp',
    'src/workbench/measure.cpp',
    'src/workbench/nest_card.cpp',
    'src/workbench/nest_keys.cpp',
    'src/workbench/nest_labels.cpp',
    'src/workbench/nest_rings.cpp',
    'src/workbench/nest_scene.cpp',
    'src/workbench/nest_texture.cpp',
    'src/workbench/nest_tiles.cpp',
    'src/workbench/not_counted.cpp',
    'src/workbench/pane_layout.cpp',
    'src/workbench/preview_home.cpp',
    'src/workbench/pillar_ring.cpp',
    'src/workbench/radar_sweep.cpp',
    'src/workbench/ridge_field.cpp',
    'src/workbench/ring_field.cpp',
    'src/workbench/ring_geometry.cpp',
    'src/workbench/ring_leaders.cpp',
    'src/workbench/scene_shapes.cpp',
    'src/workbench/scene_shapes_atlas.cpp',
    'src/workbench/scene_shapes_bars.cpp',
    'src/workbench/scene_shapes_flow.cpp',
    'src/workbench/scene_shapes_iris.cpp',
    'src/workbench/scene_style.cpp',
    'src/workbench/shape.cpp',
    'src/workbench/shape_mesh.cpp',
    'src/workbench/shape_tween.cpp',
    'src/workbench/spectrum_meter.cpp',
    'src/workbench/stargate_motion.cpp',
    'src/workbench/stargate_rows.cpp',
    'src/workbench/sun_horizon.cpp',
    'src/workbench/treemap.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep],
)
workbench_dep = declare_dependency(link_with: workbench_lib, include_directories: src_inc, dependencies: [model_dep])

formats_lib = static_library('luciole-formats',
  files(
    'src/formats/code_tokens.cpp',
    'src/formats/color_codes.cpp',
    'src/formats/color_notations.cpp',
    'src/formats/delimited_detect.cpp',
    'src/formats/delimited_scan.cpp',
    'src/formats/delimited_table.cpp',
    'src/formats/delimited_write.cpp',
    'src/formats/file_kind.cpp',
    'src/formats/hex_dump.cpp',
    'src/formats/json_text.cpp',
    'src/formats/json_value.cpp',
    'src/formats/language_keywords.cpp',
    'src/formats/line_tokens.cpp',
    'src/formats/markdown.cpp',
    'src/formats/markdown_blocks.cpp',
    'src/formats/markdown_inline.cpp',
    'src/formats/markdown_lines.cpp',
    'src/formats/markdown_text.cpp',
    'src/formats/mesh_gltf.cpp',
    'src/formats/mesh_obj.cpp',
    'src/formats/mesh_ply.cpp',
    'src/formats/mesh_stl.cpp',
    'src/formats/sheet_ods.cpp',
    'src/formats/sheet_xlsx.cpp',
    'src/formats/sheet_xlsx_styles.cpp',
    'src/formats/styled_markdown.cpp',
    'src/formats/styled_text.cpp',
    'src/formats/table_grid.cpp',
    'src/formats/text_facts.cpp',
    'src/formats/text_lines.cpp',
    'src/formats/text_tokens.cpp',
    'src/formats/xml_reader.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep],
)
formats_dep = declare_dependency(link_with: formats_lib, include_directories: src_inc, dependencies: [model_dep])

editing_lib = static_library('luciole-editing',
  files(
    'src/editing/edit_gate.cpp',
    'src/editing/markdown_source.cpp',
    'src/editing/source_lines.cpp',
    'src/editing/table_session.cpp',
    'src/editing/text_buffer.cpp',
    'src/editing/text_history.cpp',
    'src/editing/text_motion.cpp',
  ),
  include_directories: src_inc,
  dependencies: [formats_dep],
)
editing_dep = declare_dependency(link_with: editing_lib, include_directories: src_inc, dependencies: [formats_dep])

event_loop_lib = static_library('luciole-event-loop',
  files('src/event_loop/glib_poll_source.cpp'),
  include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)
event_loop_dep = declare_dependency(link_with: event_loop_lib, include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)

libarchive = dependency('libarchive')
liblzma = dependency('liblzma')
fs_lib = static_library('luciole-fs',
  files(
    'src/fs/archive_type.cpp',
    'src/fs/atomic_file.cpp',
    'src/fs/background_job.cpp',
    'src/fs/cancel_token.cpp',
    'src/fs/directory_entries.cpp',
    'src/fs/directory_session.cpp',
    'src/fs/directory_watch.cpp',
    'src/fs/drive_monitor.cpp',
    'src/fs/entry_access.cpp',
    'src/fs/file_error.cpp',
    'src/fs/file_icon_source.cpp',
    'src/fs/file_filter.cpp',
    'src/fs/file_metadata.cpp',
    'src/fs/file_query.cpp',
    'src/fs/filename.cpp',
    'src/fs/folder_totals.cpp',
    'src/fs/folder_totals_walk.cpp',
    'src/fs/gio_interop.cpp',
    'src/fs/heap_trim.cpp',
    'src/fs/interned_text.cpp',
    'src/fs/location.cpp',
    'src/fs/location_mount.cpp',
    'src/fs/mount_interaction.cpp',
    'src/fs/native_listing.cpp',
    'src/fs/network_listing.cpp',
    'src/fs/ws_discovery.cpp',
    'src/fs/ws_discovery_metadata.cpp',
    'src/fs/ws_discovery_protocol.cpp',
    'src/fs/transfer_location.cpp',
    'src/fs/posix_fd.cpp',
    'src/fs/recursive_search.cpp',
    'src/fs/subtree_measure.cpp',
    'src/fs/subtree_measure_walk.cpp',
    'src/fs/trash_info.cpp',
    'src/fs/trash_availability.cpp',
    'src/fs/user_file_save.cpp',
    'src/fs/xdg_dirs.cpp',
  ),
  include_directories: src_inc,
  dependencies: [gio, libarchive, liblzma, curl, xml],
)
fs_dep = declare_dependency(link_with: fs_lib, include_directories: src_inc, dependencies: [gio, libarchive, liblzma, curl, xml])

config_lib = static_library('luciole-config',
  files(
    'src/config/diagnostic.cpp',
    'src/config/settings.cpp',
    'src/config/shared_bookmarks.cpp',
    'src/config/state_store.cpp',
    'src/config/toml_decoder.cpp',
    'src/config/view_preference_writer.cpp',
    'src/config/visit_service.cpp',
    'src/config/visit_store.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, fs_dep, tomlplusplus],
)
config_dep = declare_dependency(link_with: config_lib, include_directories: src_inc,
  dependencies: [model_dep, fs_dep],
)

browser_lib = static_library('luciole-browser',
  files(
    'src/browser/action_registry.cpp',
    'src/browser/browser_controller.cpp',
    'src/browser/browser_mount.cpp',
    'src/browser/chooser_policy.cpp',
    'src/browser/directory_model.cpp',
    'src/browser/directory_tree.cpp',
    'src/browser/entry_details.cpp',
    'src/browser/selection_details.cpp',
    'src/browser/selection_size_note.cpp',
    'src/browser/job_routing.cpp',
    'src/browser/location_presentation.cpp',
    'src/browser/lookahead_columns.cpp',
    'src/browser/measure_cache.cpp',
    'src/browser/measured_tree.cpp',
    'src/browser/miller_columns.cpp',
    'src/browser/path_completion.cpp',
    'src/browser/places.cpp',
    'src/browser/selection_tools.cpp',
    'src/browser/tab_workspace.cpp',
    'src/browser/workbench_model.cpp',
    'src/browser/workbench_services.cpp',
    'src/browser/workbench_snapshot.cpp',
    'src/browser/workbench_state.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, fs_dep, config_dep, workbench_dep],
)
browser_dep = declare_dependency(link_with: browser_lib, include_directories: src_inc,
  dependencies: [model_dep, fs_dep, config_dep, workbench_dep],
)

libacl = dependency('libacl')
operations_lib = static_library('luciole-operations',
  files(
    'src/operations/archive_icons.cpp',
    'src/operations/archive_input.cpp',
    'src/operations/archive_provider_process.cpp',
    'src/operations/archive_rar_input.cpp',
    'src/operations/archive_sevenzip.cpp',
    'src/operations/archive_xz_input.cpp',
    'src/operations/archive_service.cpp',
    'src/operations/archive_reader.cpp',
    'src/operations/archive_support.cpp',
    'src/operations/archive_writer.cpp',
    'src/operations/batch_rename_service.cpp',
    'src/operations/create_service.cpp',
    'src/operations/create_receipt.cpp',
    'src/operations/move_receipt.cpp',
    'src/operations/move_sequence.cpp',
    'src/operations/copy_receipt.cpp',
    'src/operations/copied_move_receipt.cpp',
    'src/operations/copy_history_tree.cpp',
    'src/operations/creation_stage.cpp',
    'src/operations/history_mount.cpp',
    'src/operations/empty_trash_service.cpp',
    'src/operations/permission_accounts.cpp',
    'src/operations/permission_jobs.cpp',
    'src/operations/permission_service.cpp',
    'src/operations/permission_target.cpp',
    'src/operations/permission_walk.cpp',
    'src/operations/posix_acl.cpp',
    'src/operations/rename_service.cpp',
    'src/operations/rename_receipt.cpp',
    'src/operations/restore_service.cpp',
    'src/operations/template_catalog.cpp',
    'src/operations/transfer_copy.cpp',
    'src/operations/transfer_service.cpp',
    'src/operations/provider_transfer.cpp',
    'src/operations/provider_copy_proof.cpp',
    'src/operations/provider_copy_receipt.cpp',
    'src/operations/transfer_staging.cpp',
    'src/operations/trash_item.cpp',
    'src/operations/trash_service.cpp',
  ),
  include_directories: src_inc,
  dependencies: [fs_dep, libacl, libarchive, liblzma, dependency('threads')],
)
operations_dep = declare_dependency(link_with: operations_lib, include_directories: src_inc,
  dependencies: [fs_dep, libacl, libarchive, liblzma, dependency('threads')],
)

libpng = dependency('libpng')
imaging_lib = static_library('luciole-imaging',
  files(
    'src/imaging/animation_frames.cpp',
    'src/imaging/animation_head.cpp',
    'src/imaging/pixbuf_image_decoder.cpp',
    'src/imaging/png_decoder.cpp',
    'src/imaging/raster_decoder.cpp',
    'src/imaging/svg_rasterizer.cpp',
    'src/imaging/text_document_loader.cpp',
    'src/imaging/text_line_glyphs.cpp',
    'src/imaging/text_line_strip.cpp',
    'src/imaging/text_tile_lines.cpp',
    'src/imaging/text_tile_placement.cpp',
    'src/imaging/text_tile_renderer.cpp',
    'src/imaging/text_tile_service.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, fs_dep, formats_dep, noctalia_ui_dep, gdk_pixbuf, glycin, libpng,
    dependency('librsvg-2.0')],
)
imaging_dep = declare_dependency(link_with: imaging_lib, include_directories: src_inc,
  dependencies: [fs_dep, formats_dep, noctalia_ui_dep, gdk_pixbuf],
)

system_lib = static_library('luciole-system',
  files(
    'src/system/freedesktop_key_file.cpp',
    'src/system/icon_resolver.cpp',
    'src/system/icon_theme_poll_source.cpp',
  ),
  include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)
system_dep = declare_dependency(link_with: system_lib, include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)

thumbnails_lib = static_library('luciole-thumbnails',
  files(
    'src/thumbnails/file_icon_cache.cpp',
    'src/thumbnails/font_facts.cpp',
    'src/thumbnails/font_preview.cpp',
    'src/thumbnails/font_sample.cpp',
    'src/thumbnails/mesh_preview.cpp',
    'src/thumbnails/preview_service.cpp',
    'src/thumbnails/preview_metadata.cpp',
    'src/thumbnails/metadata_probe.cpp',
    'src/thumbnails/metadata_parse.cpp',
    'src/thumbnails/metadata_worker.cpp',
    'src/thumbnails/provider_process.cpp',
    'src/thumbnails/sheet_preview.cpp',
    'src/thumbnails/table_loader.cpp',
    'src/thumbnails/thumbnail_codec.cpp',
    'src/thumbnails/thumbnail_provider.cpp',
    'src/thumbnails/thumbnail_service.cpp',
    'src/thumbnails/thumbnail_store.cpp',
    'src/thumbnails/thumbnail_worker.cpp',
    'src/thumbnails/zip_members.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, fs_dep, formats_dep, imaging_dep, system_dep, noctalia_ui_dep, libarchive, libpng,
    dependency('nlohmann_json')],
)
thumbnails_dep = declare_dependency(link_with: thumbnails_lib, include_directories: src_inc,
  dependencies: [model_dep, fs_dep, imaging_dep, noctalia_ui_dep],
)

wayland_client = dependency('wayland-client')
clipboard_lib = static_library('luciole-clipboard',
  files(
    'src/clipboard/file_payload.cpp',
    'src/clipboard/text_clipboard.cpp',
  ),
  include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client],
)
clipboard_dep = declare_dependency(link_with: clipboard_lib, include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client],
)

platform_lib = static_library('luciole-platform',
  files(
    'src/platform/desktop_integration.cpp',
    'src/platform/file_chooser_portal.cpp',
    'src/platform/extraction_launcher.cpp',
    'src/platform/file_launcher.cpp',
    'src/platform/launch_activation.cpp',
    'src/platform/launch_context.cpp',
    'src/platform/program_launcher.cpp',
    'src/platform/terminal_launcher.cpp',
    'src/platform/terminal_command.cpp',
  ),
  protocol_sources,
  protocol_headers,
  include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client, gio_unix, dependency('wayland-cursor')],
)
platform_dep = declare_dependency(link_with: platform_lib, include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client, gio_unix],
  sources: protocol_headers,
)

# Playback through GStreamer when it is found; without it the player reports that playback is not available.
gstreamer_deps = [
  dependency('gstreamer-1.0', version: '>=1.22', required: get_option('media')),
  dependency('gstreamer-app-1.0', version: '>=1.22', required: get_option('media')),
  dependency('gstreamer-video-1.0', version: '>=1.22', required: get_option('media')),
  dependency('gstreamer-audio-1.0', version: '>=1.22', required: get_option('media')),
]
media_playback = (gstreamer_deps[0].found() and gstreamer_deps[1].found() and gstreamer_deps[2].found()
  and gstreamer_deps[3].found())
# Bink 2 decoder: a pinned copy of libbink2 (LGPL-2.1-or-later), built only where playback is.
bink2_deps = []
if media_playback
  bink2_deps = [subproject('libbink2', default_options: ['warning_level=1', 'werror=false']).get_variable('libbink2_dep')]
endif
media_lib = static_library('luciole-media',
  media_playback ? files('src/media/audio_tap.cpp', 'src/media/bink2_demux.cpp', 'src/media/bink2_demux_loop.cpp',
      'src/media/bink2_demux_seek.cpp', 'src/media/bink2_facts.cpp', 'src/media/bink2_poster.cpp', 'src/media/bink2_poster_worker.cpp',
      'src/media/bink2_pull_reader.cpp', 'src/media/bink2_slice_pool.cpp',
      'src/media/bink2_stream.cpp', 'src/media/frame_sink.cpp', 'src/media/gstreamer_pipeline.cpp',
      'src/media/gstreamer_runtime.cpp', 'src/media/media_player_gstreamer.cpp',
      'src/media/media_probe_gstreamer.cpp', 'src/media/pipeline_thread.cpp')
    : files('src/media/bink2_poster_none.cpp', 'src/media/media_player_none.cpp', 'src/media/media_probe_none.cpp'),
  include_directories: src_inc,
  dependencies: (media_playback ? gstreamer_deps : []) + bink2_deps,
)
media_dep = declare_dependency(link_with: media_lib, include_directories: src_inc,
  dependencies: (media_playback ? gstreamer_deps : []) + bink2_deps,
)

ui_lib = static_library('luciole-ui',
  files(
    'src/ui/browser/action_shortcuts.cpp',
    'src/ui/browser/file_icon.cpp',
    'src/ui/browser/file_tooltip.cpp',
    'src/ui/browser/file_view.cpp',
    'src/ui/browser/file_error_text.cpp',
    'src/ui/browser/places_sidebar.cpp',
    'src/ui/browser/sidebar_tree_view.cpp',
    'src/ui/browser/status_pill.cpp',
    'src/ui/controls/acl_editor.cpp',
    'src/ui/controls/animated_image_view.cpp',
    'src/ui/controls/archive_view.cpp',
    'src/ui/controls/audio_history_canvas.cpp',
    'src/ui/controls/audio_visual_bars.cpp',
    'src/ui/controls/audio_visual_cosmic_peanut.cpp',
    'src/ui/controls/audio_visual_dark_sun.cpp',
    'src/ui/controls/audio_visual_eclipse.cpp',
    'src/ui/controls/audio_visual_factory.cpp',
    'src/ui/controls/audio_visual_halo.cpp',
    'src/ui/controls/audio_visual_lines.cpp',
    'src/ui/controls/audio_visual_mesh.cpp',
    'src/ui/controls/audio_visual_orbit.cpp',
    'src/ui/controls/audio_visual_pulsar.cpp',
    'src/ui/controls/audio_visual_quad.cpp',
    'src/ui/controls/audio_visual_radar.cpp',
    'src/ui/controls/audio_visual_stargate.cpp',
    'src/ui/controls/audio_visual_view.cpp',
    'src/ui/controls/bulk_acl_editor.cpp',
    'src/ui/controls/card_style.cpp',
    'src/ui/controls/code_editor_frame.cpp',
    'src/ui/controls/code_editor_input_method.cpp',
    'src/ui/controls/code_editor_keys.cpp',
    'src/ui/controls/code_editor_lines.cpp',
    'src/ui/controls/code_editor_pointer.cpp',
    'src/ui/controls/code_editor_view.cpp',
    'src/ui/controls/operations_view.cpp',
    'src/ui/controls/file_preview_view.cpp',
    'src/ui/controls/file_metadata_view.cpp',
    'src/ui/controls/folder_totals_view.cpp',
    'src/ui/controls/media_bar_keys.cpp',
    'src/ui/controls/media_position_bar.cpp',
    'src/ui/controls/media_view.cpp',
    'src/ui/controls/media_view_picture.cpp',
    'src/ui/controls/media_view_seek.cpp',
    'src/ui/controls/mesh_program.cpp',
    'src/ui/controls/mesh_view.cpp',
    'src/ui/controls/glyph_picker.cpp',
    'src/ui/controls/miller_view.cpp',
    'src/ui/controls/pane_divider.cpp',
    'src/ui/controls/pane_divider_focus.cpp',
    'src/ui/controls/pane_header.cpp',
    'src/ui/controls/permission_editor.cpp',
    'src/ui/controls/permission_results_view.cpp',
    'src/ui/controls/recursive_permission_editor.cpp',
    'src/ui/controls/selection_contour.cpp',
    'src/ui/controls/shape_batch_node.cpp',
    'src/ui/controls/shape_batch_program.cpp',
    'src/ui/controls/shape_labels.cpp',
    'src/ui/controls/shape_view.cpp',
    'src/ui/controls/table_cell_editor.cpp',
    'src/ui/controls/table_edit_bar.cpp',
    'src/ui/controls/table_view.cpp',
    'src/ui/controls/table_view_canvas.cpp',
    'src/ui/controls/table_view_editing.cpp',
    'src/ui/controls/table_view_pools.cpp',
    'src/ui/controls/text_tile_view.cpp',
    'src/ui/controls/text_tile_view_hex.cpp',
    'src/ui/controls/text_tile_view_style.cpp',
    'src/ui/controls/text_tile_view_tiles.cpp',
    'src/ui/controls/tooltip_presenter.cpp',
    'src/ui/dialogs/archive_dialog.cpp',
    'src/ui/dialogs/batch_rename_dialog.cpp',
    'src/ui/dialogs/dialog_chrome.cpp',
    'src/ui/dialogs/link_dialog.cpp',
    'src/ui/dialogs/name_dialog.cpp',
    'src/ui/dialogs/mount_dialog.cpp',
    'src/ui/dialogs/settings_dialog.cpp',
    'src/ui/dialogs/settings_dialog_workbench.cpp',
    'src/ui/palette_file.cpp',
    'src/ui/workbench/ancestor_strips.cpp',
    'src/ui/workbench/atlas_animation.cpp',
    'src/ui/workbench/atlas_keys.cpp',
    'src/ui/workbench/atlas_layout_job.cpp',
    'src/ui/workbench/atlas_part.cpp',
    'src/ui/workbench/atlas_picture.cpp',
    'src/ui/workbench/column_preview_content.cpp',
    'src/ui/workbench/column_preview_header.cpp',
    'src/ui/workbench/column_preview_loads.cpp',
    'src/ui/workbench/column_preview_page.cpp',
    'src/ui/workbench/column_preview_picture.cpp',
    'src/ui/workbench/column_preview_view.cpp',
    'src/ui/workbench/entry_tooltip.cpp',
    'src/ui/workbench/flame_part.cpp',
    'src/ui/workbench/flow_part.cpp',
    'src/ui/workbench/instrument_view.cpp',
    'src/ui/workbench/iris_part.cpp',
    'src/ui/workbench/jump_list_view.cpp',
    'src/ui/workbench/jump_pane.cpp',
    'src/ui/workbench/kept_snapshots.cpp',
    'src/ui/workbench/ladder_part.cpp',
    'src/ui/workbench/list_rows.cpp',
    'src/ui/workbench/nest_part.cpp',
    'src/ui/workbench/nest_work.cpp',
    'src/ui/workbench/pane_shell.cpp',
    'src/ui/workbench/part_view.cpp',
    'src/ui/workbench/quiet_column_view.cpp',
    'src/ui/workbench/ribbon_view.cpp',
    'src/ui/workbench/scene_part.cpp',
    'src/ui/workbench/snapshot_places.cpp',
    'src/ui/workbench/workbench_center_view.cpp',
    'src/ui/workbench/workbench_chrome.cpp',
    'src/ui/workbench/workbench_columns.cpp',
    'src/ui/workbench/workbench_parts.cpp',
    'src/ui/workbench/workbench_preferences.cpp',
    'src/ui/workbench/workbench_row_tooltip.cpp',
    'src/ui/workbench/workbench_view.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, browser_dep, config_dep, operations_dep, thumbnails_dep, media_dep, editing_dep,
    noctalia_ui_dep, tomlplusplus],
)
ui_dep = declare_dependency(link_with: ui_lib, include_directories: src_inc,
  dependencies: [model_dep, browser_dep, config_dep, operations_dep, thumbnails_dep, media_dep, editing_dep,
    noctalia_ui_dep],
)

app_jobs_lib = static_library('luciole-app-jobs',
  files('src/app/operation_hub.cpp', 'src/app/job_activity.cpp', 'src/app/drive_removal.cpp'),
  include_directories: src_inc,
  dependencies: [browser_dep, operations_dep],
)
app_jobs_dep = declare_dependency(link_with: app_jobs_lib, include_directories: src_inc,
  dependencies: [browser_dep, operations_dep],
)

# The Workbench session a page mounts, apart from the window so its tests can link it.
app_workbench_lib = static_library('luciole-app-workbench',
  files(
    'src/app/page/held_view_change.cpp',
    'src/app/page/preview_edit_controller.cpp',
    'src/app/page/preview_edit_keys.cpp',
    'src/app/page/preview_edit_listing.cpp',
    'src/app/page/preview_edit_question.cpp',
    'src/app/page/preview_edit_table.cpp',
    'src/app/page/preview_edit_text.cpp',
    'src/app/page/workbench_preview.cpp',
    'src/app/page/workbench_session.cpp',
  ),
  include_directories: src_inc,
  dependencies: [ui_dep, browser_dep],
)
app_workbench_dep = declare_dependency(link_with: app_workbench_lib, include_directories: src_inc,
  dependencies: [ui_dep, browser_dep],
)

# The order in which a close asks pages with unsaved changes, apart from the window so its test can link it.
app_close_lib = static_library('luciole-app-close',
  files('src/app/close_gate.cpp'),
  include_directories: src_inc,
)
app_close_dep = declare_dependency(link_with: app_close_lib, include_directories: src_inc)

app = executable('luciole',
  files(
    'src/app/application.cpp',
    'src/app/cli.cpp',
    'src/app/file_chooser_session.cpp',
    'src/app/file_manager_service.cpp',
    'src/app/instance_service.cpp',
    'src/app/library_setup.cpp',
    'src/app/main_loop.cpp',
    'src/app/page/about_controller.cpp',
    'src/app/page/active_popup.cpp',
    'src/app/page/application_chooser_controller.cpp',
    'src/app/page/archive_controller.cpp',
    'src/app/page/batch_rename_controller.cpp',
    'src/app/page/browser_page.cpp',
    'src/app/page/create_controller.cpp',
    'src/app/page/details_header_controller.cpp',
    'src/app/page/dialog_dropdown.cpp',
    'src/app/page/program_controller.cpp',
    'src/app/page/drag_drop_controller.cpp',
    'src/app/page/file_keyboard_controller.cpp',
    'src/app/page/location_bar_controller.cpp',
    'src/app/page/menu_controller.cpp',
    'src/app/page/mount_controller.cpp',
    'src/app/page/preview_controller.cpp',
    'src/app/page/preview_edit_page.cpp',
    'src/app/page/properties_controller.cpp',
    'src/app/page/rename_controller.cpp',
    'src/app/page/settings_controller.cpp',
    'src/app/page/shortcuts_controller.cpp',
    'src/app/page/sidebar_controller.cpp',
    'src/app/page/sidebar_drawer.cpp',
    'src/app/page/status_bar_controller.cpp',
    'src/app/page/submenu_controller.cpp',
    'src/app/page/transfer_controller.cpp',
    'src/app/page/operations_controller.cpp',
    'src/app/page/trash_controller.cpp',
    'src/app/page/workbench_controller.cpp',
    'src/app/page/workbench_controller_menus.cpp',
    'src/app/palette_watcher.cpp',
    'src/app/startup_trace.cpp',
    'src/app/workspace_application.cpp',
    'src/app/workspace_leaving.cpp',
    'src/main.cpp',
  ),
  build_info,
  completions,
  app_icon,
  include_directories: include_directories('src', '.'),
  dependencies: [app_jobs_dep, app_workbench_dep, app_close_dep, ui_dep, clipboard_dep, platform_dep, imaging_dep, system_dep,
    event_loop_dep, browser_dep, config_dep, operations_dep, thumbnails_dep, noctalia_ui_dep, wayland_client,
    dependency('threads')],
  install: true,
)

configure_file(
  input: 'assets/dbus/dev.noctalia.luciole.FileManager1.service.in',
  output: 'dev.noctalia.luciole.FileManager1.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1/services',
)

configure_file(
  input: 'assets/dbus/org.freedesktop.impl.portal.desktop.luciole.service.in',
  output: 'org.freedesktop.impl.portal.desktop.luciole.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1/services',
)
install_data('assets/portal/luciole.portal', install_dir: get_option('datadir') / 'xdg-desktop-portal/portals')

tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (
  tests_opt.auto() and get_option('buildtype') == 'debug' and get_option('b_sanitize') == 'none'
)
if build_tests
  subdir('tests')
endif
if get_option('benchmarks')
  subdir('tools/bench')
endif

install_data('LICENSE', 'docs/third-party-notices.md', install_dir: get_option('datadir') / 'licenses/luciole')
install_subdir(noctalia_ui_assets_dir / 'fonts', install_dir: get_option('datadir') / 'luciole/assets')
install_data('assets/noctalia/colors.toml', install_dir: get_option('datadir') / 'luciole/templates')
install_data('assets/completions/luciole.bash',
  install_dir: get_option('datadir') / 'bash-completion/completions',
  rename: 'luciole',
)
install_data('assets/completions/_luciole', install_dir: get_option('datadir') / 'zsh/site-functions')
install_data('assets/completions/luciole.fish', install_dir: get_option('datadir') / 'fish/vendor_completions.d')
install_data('assets/desktop/dev.noctalia.luciole.desktop', install_dir: get_option('datadir') / 'applications')
install_data('assets/icons/dev.noctalia.luciole.svg', install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')

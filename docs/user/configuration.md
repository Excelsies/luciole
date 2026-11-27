# Configuration reference

Every Luciole preference is a key in `config.toml`; use this page to write the file by hand or to look up a default.

## Files and precedence

Settings load in this order, later steps overriding earlier ones:

1. Built-in defaults.
2. `$XDG_CONFIG_HOME/luciole/config.toml`, your optional base configuration.
3. `$XDG_CONFIG_HOME/luciole/settings.toml`, the overrides written by [Settings](settings.md).

When `XDG_CONFIG_HOME` is unset, `~/.config` is used. Luciole never writes to `config.toml`, so Nix or another declarative tool can own it. A base edit hidden by an override takes effect after you reset that setting. Luciole does not read `/etc/xdg` or a packaged default.

Valid external changes apply automatically. Unknown keys log a warning and are ignored; Settings keeps them when it saves. Wrong types, unsupported values, an incompatible `schema_version` and invalid TOML are errors, and an invalid reload keeps the last working settings.

`schema_version` is optional; when present it must be the integer `1`. This is a valid starting file, not a required one:

```toml
schema_version = 1

[browser]
view_mode = "list"
show_hidden = false
show_preview = true
preview_width = 320
activation_mode = "double_click"
typing_mode = "search"
drop_action = "menu"
switch_to_new_tab = true
confirm_permanent_deletion = true
restore_last_window = false
single_window = false
compact_menus = true
context_menu_icons = true
show_close_buttons = true
show_folder_in_title = true
background_opacity = 1.0
corner_radius_scale = 1.0
sort_by = "name"
directories_first = true
hidden_last = false
```

Check a file with `luciole config validate /path/to/config.toml`; it also reports unknown keys. See [Validate a file](settings.md#validate-a-file).

## Icon theme

File and folder icons follow the desktop icon theme. Switching themes or editing the active theme's SVG or PNG files refreshes icons automatically. Without desktop settings support, Luciole uses the theme named in GTK `settings.ini`.

Luciole picks the theme's drawing for the displayed size, including display scaling, so zooming can change an icon's detail as well as its size.

Themes that declare `FollowsColorScheme=true` in their `index.theme` (Breeze, Tela, Colloid and others) draw recolorable parts with the `colors.toml` palette: folders take the primary color and monochrome icons the text color. Other themes keep their own colors.

## Browser preferences

All keys belong in `[browser]`. Dimensions are logical pixels. View, zoom and sorting keys set the defaults for new pages; existing pages keep their state. Other keys apply to the whole application.

| Key | Default | Accepted values / meaning |
| --- | --- | --- |
| `view_mode` | `"list"` | `"list"`, `"grid"`, `"miller"`, `"workbench"`. File picker windows open `"miller"` and `"workbench"` as `"list"` |
| `remember_folder_views` | `false` | Sort and Grid/List changes apply to the current folder only and are remembered for it; file picker windows keep their own. See [per-folder view and sort](views.md#remember-a-view-per-folder) |
| `grid_icon_size` | `96` | `32`, `48`, `64`, `80`, `96`, `128`, `160`, `192`, `256` |
| `list_icon_size` | `0` | `0` uses the default size, `32`; or `16`, `20`, `24`, `28`, `32`, `40`, `48`, `64`. Rows grow with the icon |
| `miller_icon_size` | `0` | Columns view icon size: `0` uses the density's size (`20` compact, `28` comfortable); otherwise the values of `list_icon_size` |
| `density` | `"compact"` | `"compact"`, `"comfortable"` |
| `filename_truncation` | `"middle"` | `"middle"` keeps both ends of long names; `"end"` keeps the beginning. Grid keeps two-line wrapping |
| `show_size_caption` | `false` | Size caption in Grid view |
| `show_type_caption` | `false` | Type caption in Grid view |
| `show_modified_caption` | `false` | Modified-time caption in Grid view |
| `show_emblems` | `true` | File emblems |
| `show_hidden` | `false` | Hidden entries |
| `show_backups` | `true` | Non-directory backup names ending in `~` |
| `always_show_tab_bar` | `true` | Keep the tab bar visible with a single tab |
| `always_show_status_bar` | `false` | Keep the status bar visible when idle, showing item count and free space |
| `compact_menus` | `true` | Reduce context menu row height and padding without shrinking text |
| `context_menu_icons` | `true` | Show action icons at the start of context menu rows; disabling removes the icon column only |
| `show_close_buttons` | `true` | Show close buttons on tabs, dialogs, panes, the preview and archives. Window controls, `Escape` and Close/Cancel still work when hidden |
| `reduce_animations` | `false` | Skip animated transitions (entries fading in, smooth scrolling, the sidebar drawer). Applies live |
| `use_ssd` | `true` | Let the compositor draw the title bar and borders when it supports them; disable to always draw Luciole's own. Open windows switch live |
| `show_folder_in_title` | `true` | Title each browser window after the folder its active pane shows (`Downloads - Luciole`); disable to title every window `Luciole`. File picker windows keep their own title. Open windows switch live |
| `color_scheme` | `"auto"` | `"auto"` uses Noctalia's current palette from `colors.toml`; without that file it follows the desktop's GTK 4 colors when they are usable (see below), and otherwise uses the built-in dark palette; `"dark"` and `"light"` use that variant, or the built-in dark or light palette when the file has none. Applies live |
| `background_opacity` | `1.0` | `0` (clear) to `1` (opaque) for the window background; menus and dialogs stay opaque |
| `corner_radius_scale` | `1.0` | `0` (square) to `2` (extra rounded), scaling every corner radius. Applies live |
| `activation_mode` | `"double_click"` | `"single_click"`, `"double_click"` |
| `typing_mode` | `"search"` | `"search"` searches filenames recursively; `"filter"` filters the current folder only; `"typeahead"` selects names |
| `drop_action` | `"menu"` | Drop without a modifier: `"menu"` opens a menu at the pointer, `"copy"` copies, `"move"` moves. `Ctrl`, `Shift` and `Alt` take priority; Trash, items from Trash and non-native folders keep fixed behavior |
| `single_click_location_edit` | `false` | Edit the path with a single click on the current breadcrumb or unused space instead of a double-click |
| `confirm_permanent_deletion` | `true` | Ask before permanently deleting, including `Shift+Delete` and selections in Trash; Empty Trash always asks |
| `show_permanent_deletion` | `false` | Show Delete Permanently in context menus outside Trash; `Shift+Delete` and Trash actions work either way |
| `open_as_archive` | the types the system reports as plain archives: `ear`, `aar`, `apks`, `xapk`, `vsix`, `nupkg`, `whl`, `egg`, `ipa`, `crate`, `gem`, `usdz`, `mcpack`, `mcworld`, `mcaddon`, `mctemplate`, `pk3`, `pk4`, `love`, `sb3` | Archive-like file types that open in the archive browser instead of their default application: an array of type names, each a file suffix without the dot, such as `["jar", "apk", "epub"]`. A list replaces the default entirely, so `[]` opens none of them in the archive browser. An unknown name, or one in another letter case, invalidates the setting. Browse Archive, Extract Here and Extract To are in the context menu of these files whatever the list says. The names are listed under [archive-like files](archives.md#open-archive-like-files); Settings → Archive-Like Files has a switch for each |
| `switch_to_new_tab` | `true` | Switch to a folder opened in a new tab; disable to keep browsing the current tab |
| `restore_last_window` | `false` | Reopen the tabs of the last closed window, once, in a new window or at startup without a location |
| `single_window` | `false` | Keep one window: launches with a path and FileManager1 show-items requests open tabs in it, launches without a path raise it, New Window opens a tab, and tab-moving entries leave the menus |
| `terminal_command` | `""` | Terminal command with optional arguments, for example `"foot"`; empty uses `$TERMINAL`, then automatic discovery. See [Opening files](opening-files.md) |
| `record_visits` | `true` | Count how often and how recently folders are entered and files are opened, for the Workbench visits measure. Off, nothing is recorded or written and every visit count reads as zero; counts already saved stay on disk. File picker windows never record |
| `sort_by` | `"name"` | `"name"`, `"type"`, `"size"`, `"modified"`, `"accessed"`, `"created"`, `"owner"`, `"group"`, `"permissions"`, `"location"` |
| `sort_descending` | `false` | Reverse sort direction |
| `directories_first` | `true` | Group folders, and links to local folders, before files |
| `hidden_last` | `false` | Sort shown hidden items after visible ones; `directories_first` applies within each group |
| `show_sidebar` | `true` | Show the docked sidebar in wide windows; narrow windows use a temporary drawer |
| `sidebar_width` | `206` | Integer `160` through `400` |
| `sidebar_tree` | `false` | Sidebar folder tree |
| `show_drives` | `true` | Show drive rows |
| `show_network` | `true` | Show Network in the sidebar; a hidden entry keeps its saved position |
| `expandable_folders` | `false` | Expand folders within List view |
| `show_preview` | `true` | Show the side preview when space permits |
| `chooser_show_preview` | `false` | Show the side preview in file picker windows; toggling it there changes only this key |
| `chooser_start_folder` | `"home"` | `"home"`, `"last"`: where a file picker opens when the application suggests no folder. `"last"` reopens the folder of the last accepted pick; a suggested folder always wins |
| `chooser_view_mode` | `"list"` | `"list"`, `"grid"`; kept apart from `view_mode` |
| `chooser_sort_by`, `chooser_sort_descending`, `chooser_directories_first`, `chooser_hidden_last` | as `sort_by` and its companions | Sorting of file picker windows, kept apart from the browser's |
| `chooser_show_hidden` | `false` | Show hidden items in file picker windows |
| `chooser_grid_icon_size`, `chooser_list_icon_size` | as `grid_icon_size`, `list_icon_size` | Zoom of file picker windows, kept apart from the browser's |
| `preview_width` | `320` | Integer `208` through `800` |
| `miller_column_width` | `260` | Preferred column width, integer `180` through `800`; individual columns can keep their own width |
| `miller_show_parents` | `true` | Include parent folders when entering Column view |

### List columns

Name is always present. Type, Size and Modified are visible by default; Accessed, Created, Owner, Group, Permissions and Location are hidden by default.

Visibility keys are `show_type_column`, `show_size_column`, `show_modified_column`, `show_accessed_column`, `show_created_column`, `show_owner_column`, `show_group_column`, `show_permissions_column` and `show_location_column`.

`column_order` lists unique column names. It must include `"type"`, `"size"` and `"modified"`; the optional names are `"accessed"`, `"created"`, `"owner"`, `"group"`, `"permissions"` and `"location"`, and omitted ones are appended in that order. Name is not an entry.

| Key | Default |
| --- | --- |
| `name_column_width` | `0` (automatic) |
| `type_column_width` | `152` |
| `size_column_width` | `118` |
| `modified_column_width` | `156` |
| `accessed_column_width` | `156` |
| `created_column_width` | `156` |
| `owner_column_width` | `128` |
| `group_column_width` | `128` |
| `permissions_column_width` | `112` |
| `location_column_width` | `260` |

Name accepts `0` for automatic sizing or an integer from `160` through `16384`. Other widths accept integers from `64` through `2048`.

### Workbench

| Key | Default | Accepted values / meaning |
| --- | --- | --- |
| `workbench_instrument` | `"nest"` | `"nest"`, `"iris"`, `"atlas"`: the part above Jump Ahead in the left column |
| `workbench_ribbon` | `"flame"` | `"flame"`, `"flow"`, `"ladder"`: the part above the list |
| `workbench_strips` | `true` | Ancestor Strips beside the list |
| `workbench_size_by` | `"size"` | What sizes shapes and orders entries: `"size"` (bytes), `"files"` (files below), `"items"` (direct entries), `"modified"` (recent changes weigh more), `"visits"`, `"equal"` |
| `workbench_color_by` | `"type"` | `"type"`, `"size"`, `"visits"`, `"modified"` (the newest change is at the top of the ramp) |
| `workbench_emphasis` | `0.5` | Number from `0` to `1`; every weight is raised to this power, so `0` makes all entries equal and `1` keeps true proportion |
| `workbench_audio_visualizer` | `true` | A sound file in the preview shows a visual while it plays |
| `workbench_preview_place` | `"jump_ahead_when_narrow"` | Where a file's preview is shown. `"jump_ahead_when_narrow"`: in its column, and in the place of Jump Ahead when no column has room for it, as in a small window or at a high output scale. `"jump_ahead_always"`: in the place of Jump Ahead at every window size, with no preview column. `"hidden_when_narrow"`: in its column, and not shown when no column has room |
| `workbench_media_loop` | `false` | A video or sound file in the preview plays again from the start when it ends |
| `workbench_audio_visual` | `"cosmic-peanut"` | `"cosmic-peanut"`, `"pulsar"`, `"bars"`, `"orbit"`, `"halo"`, `"radar"`, `"eclipse"`, `"dark-sun"`, `"stargate"`: the visual it shows |

Settings → Views → Workbench sets these keys as Instrument, Ribbon, Ancestor Strips, Size by, Color by, Emphasis, Preview Placement (Replace Jump Ahead when it does not fit, Always replace Jump Ahead, or Hide when it does not fit), Audio Visualizer and Visual, and `record_visits` as Record Visits. Its Clear Visit History button forgets every saved and unsaved visit count; a visit file that cannot be rewritten keeps its counts. When clearing ends, a problem the clear met, if there is one, is shown at the top of Settings; a clear that succeeds shows none.

Pane sizes are logical pixels including the pane header. A size of `0` follows the window; a saved size that does not fit the window is reduced when the panes are laid out, and the saved value is kept.

Workbench writes these keys itself: a size when you let go of its divider, a collapsed flag when the pane collapses or opens, and all of them when you press Reset layout. Its header switches write `workbench_instrument` and `workbench_ribbon`, the size controls of its preview write `workbench_preview_size`, which Reset layout leaves as it is, the visualizer and visual buttons of a sound file write `workbench_audio_visualizer` and `workbench_audio_visual`, and the loop button of a video or sound file writes `workbench_media_loop`. Whether the instrument covers the view is not saved.

| Key | Default | Accepted values / meaning |
| --- | --- | --- |
| `workbench_left_width` | `0` | `0` (automatic) or an integer from `1` through `100000` |
| `workbench_instrument_height` | `0` | `0` (automatic) or an integer from `1` through `100000` |
| `workbench_flame_height` | `0` | `0` (the ribbon's preferred height) or an integer from `1` through `100000` |
| `workbench_flow_height` | `0` | Same as `workbench_flame_height`; each ribbon keeps its own height |
| `workbench_ladder_height` | `0` | Same as `workbench_flame_height` |
| `workbench_instrument_collapsed` | `false` | Collapse the instrument |
| `workbench_jump_collapsed` | `false` | Collapse Jump Ahead |
| `workbench_ribbon_collapsed` | `false` | Collapse the ribbon |
| `workbench_first_column_width` | `260` | Integer `140` through `640` |
| `workbench_second_column_width` | `190` | Integer `140` through `640` |
| `workbench_third_column_width` | `140` | Integer `140` through `640` |
| `workbench_list_width` | `0` | `0` (none) or an integer `320` through `469`: the narrower list width a second column ahead may leave, below the default `470` |
| `workbench_strips_width` | `0` | `0` (the default, `153`) or an integer `70` through `321`: the width of the ancestor strips pane in pixels at the default font size; the pane draws it larger at a larger system font size |
| `workbench_preview_size` | `100` | `70`, `80`, `90`, `100`, `110`, `125`, `150`, `175`, `200`, `250` or `300`: the size in percent of every preview's content |

### Thumbnails

| Key | Default | Accepted values |
| --- | --- | --- |
| `thumbnail_policy` | `"local"` | `"never"`, `"local"`, `"all"` |
| `thumbnail_max_file_mib` | `64` | Image/PDF source limit, integer `1` through `1024`; does not limit video or audio files |
| `thumbnail_cache_mib` | `1024` | Integer `16` through `8192`; bounds the whole shared cache, oldest thumbnails pruned first |

`"local"` skips network shares and FUSE mounts without a local disk, such as rclone, sshfs and gocryptfs. To get thumbnails on mounted shares such as `/mnt/media`, choose **Settings → Thumbnails → Show Thumbnails → All Locations**. Locations without a filesystem path stay cache-only; Luciole never downloads whole remote files for thumbnails.

The size limit applies to image and PDF sources only. There is no pixel limit: the decoder's memory, CPU time and output limits decide, which admits phone photos up to about 200 megapixels. With `ffmpegthumbnailer` installed, new video thumbnails get its filmstrip edges; without it they stay plain. Settings can inspect and clear the cache, and existing shared thumbnails are reused.

## Other writable data

- **Pinned folders:** `$XDG_CONFIG_HOME/gtk-3.0/bookmarks`, default `~/.config/gtk-3.0/bookmarks`. Each line holds a URI and an optional label. See [Pin a folder](sidebar.md#pin-a-folder).
- **Sidebar layout and last window:** `$XDG_STATE_HOME/luciole/state.toml`, default `~/.local/state/luciole/state.toml`. It holds the built-in Places layout, separators and headings, custom pin icons, the tabs of the last window and the last file picker folder. Icons exist only here.
- **Per-folder view and sort:** `$XDG_STATE_HOME/luciole/folder_views.toml`. Luciole keeps the 4096 most recently changed folders.
- **Visit counts:** `$XDG_STATE_HOME/luciole/visits/`, default `~/.local/state/luciole/visits/`, as private files readable only by you. Each location keeps how many times it was entered or opened and when last. A folder counts once it has loaded; a folder that fails to open, a refresh, a duplicated tab or split pane, a new tab or window showing the current folder, and the tabs restored by `restore_last_window` do not count. Locations are spread over 32 files of at most 625 locations each, so at most 20,000 are kept; when a file is full, its least recently visited locations are forgotten. Locations with passwords, queries or fragments are never recorded. A damaged visit file is reported as a warning and its counts start again from zero. A visit file in a format this version of Luciole does not read is left for the version that wrote it, and its locations are not counted here. A visit file larger than 256 KiB or replaced by a link is reported as a warning on every write and left as it is until you remove it while Luciole is closed. Set `browser.record_visits = false` to stop recording; delete the folder while Luciole is closed to forget every count.
- **Thumbnails:** the shared `$XDG_CACHE_HOME/thumbnails/` cache, default `~/.cache/thumbnails/`.
- **Custom actions:** the [scripts directory](scripts.md).
- **Palette:** an optional `colors.toml` beside the settings files, written by Noctalia's [Luciole template](https://docs.noctalia.dev/noctalia/templates/official/luciole/). With `color_scheme = "auto"` and no `colors.toml`, Luciole follows the desktop's GTK 4 colors: it reads `$XDG_CONFIG_HOME/gtk-4.0/gtk.css` and the local files it pulls in with `@import`, where theming tools such as Noctalia's GTK template publish their colors, so no setup is needed. The colors are usable when `window_bg_color`, `window_fg_color` and `accent_bg_color` are all defined, the first two without transparency, as `@define-color` names or `--name` variables; the card, popover, sidebar and header bar colors, `border_color`, the active toggle, error and destructive colors are used too, and every other role is derived from them, with light or dark taken from the colors themselves. See [Packaging](../../PACKAGING.md#configuration-and-writable-data) for ownership and installation.

Pinned folders are shared with GTK applications and file dialogs, and Luciole refuses to edit a read-only or symlinked bookmarks file.

Text follows the system font: family and size come from GNOME's `font-name` setting, or `gtk-font-name` in `~/.config/gtk-3.0/settings.ini` (then `gtk-4.0`). Without either, Luciole uses `sans-serif` at 14 px. Large Text (`text-scaling-factor`) multiplies the size, and open windows update live. Icons keep their size.

# Workbench in Luciole

Workbench is a view mode made of resizable panes: an instrument (Nest, Iris or Atlas), a Jump ahead list, a ribbon above the list (flame graph, flow or ladder), ancestor strips, the details list, and quieter columns ahead that also preview and edit files. Its behavior is specified by the design in `plans/designs/nav-lab/` (`WORKBENCH.md`). This document says how it is built natively.

## Constraints

- `lib/` is a mirror of the Noctalia toolkit and is not edited here. Everything new lives under `src/`.
- Module layering, naming, comment style, lint (clang-tidy with warnings as errors, 250-line functions in `src/`), formatting (clang-format 23) and palette rules are those of `CONTRIBUTING.md`.
- Rendering never does I/O. Workers publish to the main loop and check cancellation, context and owner lifetime.
- Every increment ships tests, passes lint and format, and updates `docs/user/` and `docs/design/` where behavior or contracts change.

## What the toolkit gives and what it lacks

| Need | Toolkit | Built in `src/` |
| --- | --- | --- |
| Custom drawing | `CustomDrawNode`, `RenderExtension`, `ShaderProgram` (GLES2) | One batched shape node: rectangles, rounded rectangles, ring sectors, circles and ribbon strips in one draw call |
| Tweens | `AnimationManager::animate` | One 0 to 1 tween per part, interpolating inside the batch so no layout runs during motion |
| Hit testing | `InputArea` per node | Pure point-in-shape lookup over the part's piece list, behind one `InputArea` per part |
| Lists | `VirtualGridView`, `FileViewAdapter` | Quiet column rows and Jump ahead rows as new adapters |
| Dividers | Hand-built handles (sidebar, Miller, preview) | One pane shell and one divider control |
| Colored text | None: text is rasterized as alpha and tinted one color | Lines rendered with Pango and Cairo to RGBA tiles on workers |
| Multi-line editing | `Input` (wrapped, no monospace, 500 visual lines) | A virtualized monospace editor control |
| Animated images | Not enabled | Frames through Glycin, swapped on a timer |
| Video | None | Optional media module |
| Folder measures | `FolderTotalsService` (one aggregate, Properties only) | Subtree measure service with a cache |
| Visit counts | None | Private visit store |
| Offscreen rendering | None | Real-window checks in a headless compositor |

## Modules

New code is placed by the layering rules. Two new modules are pure (standard library only, no GIO, no `lib/`), so their tests run headless and fast.

| Module | New | Contents |
| --- | --- | --- |
| `src/model` | enums | `ViewMode::Workbench`, instrument, ribbon, measure and color choices |
| `src/workbench` (pure) | new module | The measured tree type, weights and shares, colors, pane layout, column plan, drag math, treemap, ring geometry, circle packing, flame bands, flow layout, ladder plan, jump ranking, and the shape model with its mesh, tween and hit tests |
| `src/formats` (pure) | new module | File kind table, text tokenizer and JSON printer, Markdown parser, color-code scanner, styled text documents, delimited-table reader and writer, XML reader, XLSX and ODS readers, mesh readers, text facts |
| `src/editing` (pure) | new module | Text buffer with undo, source line styles, delimited-table session, the rules of editing in a preview |
| `src/fs` | services | Subtree measure walker and service, user-file text save |
| `src/config` | settings and state | Workbench preferences, visit store |
| `src/browser` | models | Measure cache, lookahead columns, Workbench page state and hover link |
| `src/thumbnails`, `src/imaging` | previews | Font sheets, animation frames, text and table loading, spreadsheet reading, mesh loading |
| `src/media` (optional) | new module | Video and audio playback |
| `src/ui` | views and controls | Shape batch, pane shell, divider, pane header, strips, column and jump adapters, three instruments, three ribbons, text tile view, editor, table view, animated image view, video view, mesh view |
| `src/app/page` | controller | `WorkbenchController` behind `PageContext`, unsaved-changes guard, preview routing |

## The measured tree

Every layout consumes one immutable snapshot, built by `src/browser` from listings and the measure cache.

```cpp
namespace luciole::workbench {
  using NodeId = std::uint32_t;
  inline constexpr NodeId kNoNode = 0xFFFFFFFFU;

  struct Node {
    std::string name;
    NodeId parent = kNoNode;
    std::uint32_t firstChild = 0; // index into Tree::children
    std::uint32_t childCount = 0;
    double bytes = 0;   // own size, or everything below for a folder
    double files = 0;   // files below
    double items = 0;   // direct entries
    double modified = 0; // newest change below, seconds
    double visits = 0;
    std::uint8_t category = 0;
    bool folder = false;
    bool hidden = false;
    bool partial = false; // measure still running
    bool lowerBound = false; // walk cut short and not running: the totals are a lower bound
  };

  struct Tree {
    std::vector<Node> nodes;
    std::vector<NodeId> children; // child ids, grouped by parent
    NodeId root = kNoNode;        // the current folder
    std::vector<NodeId> ancestors; // nearest first
    double now = 0;
  };
}
```

`partial` means only that the measure is still running. A walk that was cut short and will not be continued (a folder below the walk's depth limit, a walk stopped by its entry budget or deadline, an ancestor measured under its budget, a folder on another filesystem that the walk does not enter, a folder or entry that cannot be read) sets `lowerBound` instead: its totals count what the walk saw and may be higher, and every folder above one that was not counted is a lower bound too. A folder the walk did not enter counts nothing, so it reads "not measured"; it reads exact when it is the folder you enter, since that walk starts at it. A lower-bound folder is hatched like a partial one, and its size and counts read "not measured" when the value is zero and "at least <value>" above zero (the Iris hub writes "≥ <value>" where the longer wording does not fit); a partial folder reads "measuring…" at zero and the value as it stands above zero. A sum over several folders, such as a "+N more" tile, reads measuring if any part is measuring, otherwise as a lower bound if any part is. The same rule decides the root when a refreshed child is grafted into a snapshot: it keeps its own record's state and takes in the child's, and a lower bound is never cleared by a graft, since the root's record may be cut short for a reason other than the child it lost. The "+N more" folder remainder takes its state from the record it was folded out of, because the folded folders' own states are not kept. A share beside such a value reads "about 12%" unless the node and its siblings are all finished.

Layout math uses `double`. Views convert to `float` at the scene boundary.

### Tooltips

Every view that draws entries gives a piece the same tooltip, written once in `workbench/entry_text`: `entryFacts` collects the name, size, item and file counts or type, age, opens, share, reasons and hidden flag of a node, each amount through `measureLabel`, and `entryText` joins them as the name, a line of facts and, for a lower bound, the sentence of `notCountedText` on a line of its own. The Nest, Flame, Flow, Ladder and Atlas use that text through `EntryTooltips::entry`; the Iris keeps its table of name, size and share while every amount is exact and falls back to the same text when one is a lower bound, since a table row holds one line. Shapes that stand for an entry are pieces, card heads, path bars, far cells, ancestor pills and ribbons; the current folder's own shapes have a tooltip only while its total is a lower bound, and rings of merged entries and drawing details have none. A "+N more" tile that prints a size, in the Nest, a Nest card and the Flow, has a tooltip only while that total reads "at least": `mergedText` sums its entries. The Iris hub's tooltip adds the sentence to where a click goes. A row of Jump ahead carries the sentence in `JumpRow::notCounted`, and the status bar's selection size has its own, `selectionSizeNote`.

`notCountedText` finds the folders that were not counted from the snapshot alone. A folder in `Tree::ancestors` is measured as an ancestor, under an entry and time limit, and its flags do not say what cut it, and the walk's record of the open folder below it was put in place of the one it made: the sentence for such a folder is only "Not counted: measuring this folder stopped at a limit, at another filesystem or at a folder that cannot be read.", with no folder named, and its share among its siblings, which are only listed, is left out. For any other node the walk starts at it and follows only children that are lower bounds, since an exact folder holds nothing uncounted. A visited node that is on another filesystem or unreadable is named, and its lower-bound children are followed too; a lower bound with no flagged folder in the snapshot below it is unplaced. The reason lists the folders on another filesystem, then the unreadable ones with their count, each at most three names and "and N more", then, when something is unplaced, "some folders below were not counted in full". Only lower-bound children are charged to the cap: nodes read plus nodes waiting never pass `kNotCountedVisitCap`, and the walk stops queueing when they reach it, so one folder with a hundred thousand lower-bound children costs the cap in pushes. Every child of a visited node still costs one cheap check, so a call costs at most the cap in nodes plus the children of those nodes, which the snapshot's node limit bounds. When the cap is reached the sentence is only the general one, since the names found so far are a part of the whole. The sum over a "+N more" piece walks each of its entries under the one cap. Wording states only what the flags say: it never names a cause the snapshot does not hold.

The presenter asks a shape view for its tooltip on every layout pass while the pointer rests, so each part keeps its last answer in an `EntryTooltips`, keyed on the snapshot, the shape, the settings and, for a merged piece, the full list of its entries; only another key reads the snapshot. The Atlas gives no entry tooltip while its camera moves or a drag pans it, because the circles move under a still pointer. The presenter sees the content change, hides the tooltip and shows it again after its delay once the answer comes back. Every tooltip follows the pointer rules of the presenter: after a delay, hidden on leaving, on a press and on the wheel.

## Measures

`src/fs/subtree_measure` walks with directory file descriptors, stays on one filesystem (a folder on another one is recorded and not entered), counts each directory once by device and inode, and emits one record per directory: bytes, files, direct items and newest change. The service keeps a priority queue (children of the current folder, then two and three levels ahead, then Jump ahead candidates, then ancestor levels), runs at most two walks, and merges results into immutable snapshots every 100 ms, so the picture refines while it loads. Ancestor levels are measured under a budget and fall back to direct item counts until known. `src/browser/measure_cache` is shared by tabs and windows, keyed by location with directory validators, and serves stale values while it revalidates.

A location that is not a local folder (the Trash, Recent, a share, an archive) is not measured: the model starts no walk, listing or watch for it and publishes at once a snapshot of one folder named as the location is ("Trash", "Network", else its name), marked `notMeasured`, which the instrument, the ribbon and Jump ahead show as "Not measured here" with the reason "This location is not a local folder.". The Trash's own files folder is not measured either: it holds one volume's trash only, under stored names that differ from the entries the list shows. The list, selection, preview and actions are unaffected.

## Rendering

Each instrument and ribbon is one batched shape node plus a bounded pool of text nodes. A part holds two piece lists during a transition and interpolates between them from a single tween value. Pieces carry shape kind, geometry, fill and state. Labels are created only for pieces large enough to carry one. Nothing in a tween changes node sizes, so the window is not laid out again while it runs.

## Page integration

`ViewMode::Workbench` is a fourth mode with its own action and shortcut. `WorkbenchController` owns the pane shell and the parts, adopts the page's existing file pane as the center list, and keeps `browser()` pointing at the list's controller. Columns ahead are read-only controllers that follow the cursor or a resting pointer and never become the active browser. The docked preview pane is not shown in this mode: previews live in the columns ahead.

Opening a file from a column ahead, a piece, the preview's Open button or the menu of an entry outside the list goes through `openWorkbenchFile`: it records the visit, then shows the archive browser for one browsable archive, by the rule `ActionRegistry::browsableFile` that `Open` applies to a list row (and with its refusal while a file job runs), and otherwise launches the file. The facts come from the row of the page's listing or of a column ahead, else from the file itself. A click on a folder piece in an instrument or a ribbon enters that folder, and a click on a file piece selects it, whatever the activation setting says. The list, the columns ahead and Jump ahead follow the activation setting.

### Ancestor strips

The strips are a pane of their own beside the list, with a divider on the list's side, and their width is saved like the other pane sizes. Each band is one ancestor: the nearest sits beside the list and the bands thin with distance. The pane shows as many named bands as its width holds, and the ancestors beyond them are folded into one "+N levels" band at the far side. There are no unnamed bands. The narrowest pane holds one named band and the "+N levels" band. The default holds four named bands and the "+N levels" band. Bands keep their own widths: a pane wider than its bands need stays empty beyond them, and it does not change width when the folder's depth does.

## Previews and editing

Kinds are resolved in one place from the MIME type, with a content check where the type alone is not enough. Files that hold private key material are never shown as text. Parsing of untrusted files runs in the existing resource-limited helper process with limits on bytes, rows, pages, vertices and time.

| Family | Shown as | How |
| --- | --- | --- |
| Raster and vector images | The picture, with its facts | Glycin and librsvg, as today |
| Animated images | The animation | Glycin frames, swapped on a timer while visible |
| Video and audio | Playback with play, pause and a position bar | Optional `src/media` on GStreamer; stills when it is absent |
| Text, code, JSON, YAML, TOML, INI, XML, diffs | Colored lines with color swatches | `src/formats` tokens, drawn as RGBA tiles |
| Markdown | Rendered blocks | `src/formats` |
| CSV and TSV | A table | `src/formats` reader |
| XLSX and ODS | The first sheet as a table, with a sheet list | libarchive for the members, the XML and sheet readers in `src/formats` |
| Documents and presentations | First page, page count, embedded thumbnail, text outline | Existing PDF helper; libarchive and XML for office files |
| Fonts | Names, a sample at several sizes, coverage, variable axes | A private fontconfig configuration in a helper |
| Archives | A listing with sizes | libarchive, as the archive browser does |
| 3D models | An orbit view | Small readers for OBJ, STL, PLY, glTF and 3MF, drawn into an offscreen buffer with depth |
| Everything else | Facts, and a hex and text view for unknown binaries | |

### Where the preview is shown

A previewed file has a home, decided by `workbench::previewHome()` from the column slot the file starts in, the number of columns the center's last layout drew, whether Jump ahead is shown (`PaneShell::jumpOpen()`) and the `workbench_preview_place` choice. With `jump_ahead_when_narrow` it is the preview's own column when that column is drawn, else the Jump ahead pane; with `hidden_when_narrow`, the column when drawn, else nowhere; with `jump_ahead_always`, the Jump ahead pane whatever the room. The column plan depends on the room alone under every choice, so with `always` the slot a column would have used stays an empty column and the list and the planned columns keep their places as the cursor moves between files and folders. The Jump ahead pane (`JumpPane`) holds the list and the preview node and shows one of them, under the header "Preview" while it shows the preview. Whenever the home is the Jump ahead pane and the pane is not shown (collapsed, closed for lack of room, under the expanded instrument), the home is nowhere; there is no fallback to a column. With no target, and for a folder, Jump ahead stays. The single `ColumnPreviewView` is the preview in all three homes; `WorkbenchColumns` moves it between a column slot, the Jump pane and a parked place outside the scene, so media, editing, the size controls and Open need nothing of their own. A hidden home is the hidden slot the page already handles: the file stays, its content goes, and the editor keeps an edited file. The page lays its own still preview over the slot below the header, and the left column's divider keeps its presses on the slot's edge (`jumpPreviewBounds`).

The drawn column count and the Jump ahead state come out of the layout, so `WorkbenchView::doLayout` places the preview after arranging its panes and arranges them once more when it moved: a resize that changes the home moves the preview within the same frame, and no frame draws it hidden or in two places. The preview keeps its load, its player and its editor across the move.

### Preview size

The preview's header has a smaller and a larger control, with the current step between them; pressing the step returns to the default. The step applies to whatever the preview shows: the text size of text, Markdown, tables, listings, the hex view and the editor, and the scale of pictures, animations and 3D models. Video stays fitted to the pane. The step is one saved preference for every preview. At the default step, preview text is one size below the list's; larger steps are the user's own choice and may pass it. Markdown headings are set apart by weight and color, not by size. The rows of the columns ahead are not affected: they always stay below the list's size.

### Audio visualizer

While a sound file plays, its preview shows one of nine visuals, chosen by the user:

| Visual | Picture |
| --- | --- |
| Cosmic Peanut | Waveform rings drift pole to pole around an implied, slowly turning sphere |
| Pulsar | Ridge lines stacked into the distance, each new line a moment of the sound |
| Bars | Spectrum bars with falling peak caps and a waveform line |
| Orbit | Sixty-four glowing pillars on a ring, with a camera that circles on the beat |
| Halo | A ring of light shaped by the spectrum |
| Radar | A scope whose sweep draws a spectrogram: tones trace arcs, hits leave spokes |
| Eclipse | A black sun in a bright ring, with the spectrum as its corona |
| Dark Sun | A sun that swells with the bass above an inverted mountain range made of the spectrum |
| Stargate | Two planes of slit-scan light seeded by the waveform |

Every visual is a small object behind one interface: it advances from an audio frame (a mono window of recent samples, sixty-four spectrum bands, the bass and overall level, and whether a hit just started) and draws into the preview's rectangle in the application's own GL context. A visual uses one program, at most two draw calls and at most one small history texture updated by one row per frame; none uses an offscreen pass or a simulation. Colors come from the theme's roles. The audio frame comes from a tap on the player's audio path and a small analysis done once per drawn frame.

Nothing is computed or drawn while playback is paused, while the preview is hidden, or while the visualizer is off, and the tap exists only while it is on. A preference turns the visualizer off and another picks the visual; the control bar has a button for each. With the visualizer off the preview shows the audio glyph or the embedded cover. Video is not affected.

A loop button in the control bar of every video and sound file makes a file that reaches its end start again from the beginning. It is one saved preference for every media file, off by default, and has no row in Settings. The player restarts with a seek to the start when the end arrives, and ends the file instead for an unknown length, a refused seek, or an end that comes again less than 100 ms after the restart.

Editing covers text of every kind and CSV and TSV cells, rows and columns. It is off until the user turns it on, writes only on Save, and asks before unsaved changes are left. A save goes through a user-file path in `src/fs` that keeps the encoding, byte order mark, line endings, final newline, permissions, owner and extended attributes, writes through a symlink to its target, leaves no lock file, and refuses when the file changed on disk since it was read. Hard-linked files and files in read-only folders are refused. Only valid UTF-8 text is editable. Images, video, animations, 3D models, archives, binaries and key files are never editable. Writing XLSX and ODS back is not part of this design.

### The editor

The editor shows the file's own lines, never a reformatted copy: Markdown and JSON are colored as source, and line N of the file is line N on screen. Its model is a pure text buffer in `src/editing` (one string per line, an operation log for undo, a saved mark). Lines are styled one at a time with the scan state carried between them, so an edit restyles only the lines it can affect. The view draws each visible line as its own strip and redraws only the strips an edit touched.

A file is editable when all of it was read, it is valid UTF-8, it is at most 1 MiB and no line is longer than 16 KiB. A table is editable when all of it was read. Otherwise "Enable editing" is not offered and the preview says why.

### Leaving unsaved changes

One question, "Save changes to <name>?", with Save and No. Escape and the close button mean No. A save that fails keeps the question open with the reason.

- The question is asked when the file is actually left: another entry becomes the preview's target, the folder or the view mode changes, or "Stop editing" is pressed. A move inside Workbench is not held back; the preview keeps the editor until the question is answered and then shows its new target.
- A change of view mode would take the editor off the screen, so it waits for the answer: the Workbench view stays until Save has written the file or the answer is No, then the view changes with the file still selected.
- While a file is being edited, a resting pointer does not retarget the preview. Only a deliberate action does.
- Closing the tab, the pane or the window, and quitting, ask first and continue after the answer. A close request from the compositor is deferred the same way.
- Removing a drive (the eject button or the drive's menu) asks first, before any pane leaves the drive and while a Save can still reach it. Every page of every window whose editor holds unsaved changes to a file of that drive is asked, one after the other, through the same gate as a close; edits of other files are not asked about. Save writes the file and the removal goes on; the No button drops the changes and it goes on. Escape or the close button choose nothing: the removal is given up, the changes stay in the editor and the drive stays mounted and untouched. A save that fails keeps the question open and the drive mounted. A removal asked while another question waits is refused with a message. A page that closes while its question is open counts as answered; the others are asked next. For a window or tab close, Escape and the close button still mean No. The removal then waits for a running save, walk, listing, load or playback stop that still holds a file of the drive.
- Switching to another tab asks nothing: the tab keeps its editor and its changes.
- A termination signal does not ask.

## Persistence

Choices (instrument, ribbon, strips, measure, color, emphasis) are browser preferences in `settings.toml`. Pane sizes, collapsed flags and column widths are saved like `preview_width`. Visit counts are private state.

## Order of work

1. Pure modules with their tests: `src/workbench` and `src/formats`.
2. Measure walker, service and cache. Visit store. Text save.
3. Shape batch, pane shell, divider and header, proven in a real window.
4. The mode: `ViewMode::Workbench`, the controller, the center (strips, list, columns ahead), Jump ahead, settings.
5. Ribbons, then instruments.
6. Previews by kind, then editing.
7. Documentation, packaging, performance passes.

## System font size

The toolkit multiplies every text size by the system font's size over the 14 px body size, `systemTextScale()`. The panes read it once per layout and size what holds text by it, so scale 1 is the layout as designed. The instruments and ribbons are laid out as if the pane were its size divided by the scale, with label advances divided by it, and the scene is then scaled up by it (`zoomScene`); text sizes stay, because the toolkit scales them. Pointer positions go the other way. The pane limits, the pane header, the ancestor strip bands and their paddings grow with the scale, and the rows and header of the columns ahead grow with the measured line of text. Label advances and the label nodes' measures are taken again when the scale or the font changes.

## Row size

The list's icon size is List view's setting, one for both modes. Ctrl+wheel over the list, a column ahead or Jump ahead, and Ctrl+Plus, Ctrl+Minus and Ctrl+0 outside a text edit, change its step in Workbench as in List view. The columns ahead take the icon of their own default size in the proportion of the list's to its default (16, 14 and 12 px at the default, in whole pixels), and their rows move with the icon by the same amount; Jump ahead rows, which hold no icon, are 12 px taller than column A's rows and move by the same amount. Each row is at least as tall as its text needs at the system font size, so the two compose.

## Verification

- Headless tests for every pure function and for services against fixture trees.
- Pixel tests for self-drawn nodes: the shape batch and the mesh view draw into an offscreen framebuffer on a surfaceless EGL context and the test reads the pixels back.
- Headless input tests that drive the toolkit's input dispatcher over real node trees.
- Whole-scene captures: a test lays out a real node tree with the toolkit's text shaping and draws it offscreen, so a composite view can be checked as an image.
- Real-window checks for layout, input and drawing of the whole page, in a private headless Umbriel that is built under `.cache/` and started for each check. The application gets private home and XDG directories and a fixture folder, and never connects to the desktop session.
- Frame time measured on a folder with 5,000 entries and on a subtree of 100,000 files, at 1x and fractional scale.

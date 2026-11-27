# Views and selection

Choose how a folder is shown, sorted and selected.

## Choose a view

Use the toolbar or main menu to pick a view.

![Grid view with thumbnails](assets/views-grid.webp)

| View | Shortcut | Shows |
| --- | --- | --- |
| Grid | `Ctrl+1` | Icons or thumbnails in tiles |
| List | `Ctrl+2` | One row per item with detail columns |
| Columns | `Ctrl+3` | Successive folder levels side by side |
| Workbench | `Ctrl+4` | The folder with what lies below it, see [Workbench](workbench.md) |

![List view with detail columns](assets/views-list.webp)

`Ctrl+1` to `Ctrl+3` also work while typing in the search field or location bar. File picker windows offer Grid and List only.

Your choices become defaults for new pages; existing tabs and panes keep their own state. **Settings → Appearance → Reduce Animations** turns off fade-ins and other transitions.

## Zoom

`Ctrl`+wheel, `Ctrl+Plus` and `Ctrl+Minus` change the icon size in every view. `Ctrl+0` resets it. Each view keeps its own size, except Workbench, which resizes its rows with List view's size: the wheel works over the list, a column ahead or Jump Ahead.

## Browse in columns

Selecting a folder reveals its child column; selecting another folder replaces that branch. Drag the dividers to resize columns.

![Columns view showing three folder levels](assets/views-columns.webp)

`Right` or `Enter` moves into the selected folder with its first item selected. `Left`, `Backspace` or `Alt+Up` moves back to the parent column. **Settings → Views → Show Parent Folders** is on by default; turn it off to start at the current folder alone.

## Sort items

List headers sort by their column. Grid uses the toolbar's **Sort** menu, and the background right-click menu also offers **Sort**. Changing the order shows its start, and selected items stay selected.

Shown hidden items sort among visible ones by default. **Hidden Last**, in the **Sort** menu and Settings, puts them after the visible ones; **Folders First** then applies within each group.

## Choose list columns

**Settings → List Columns** controls which columns show and their order. Extra columns include accessed and created time, owner, group, permissions and location. Missing metadata shows as unavailable.

In List view, folders show their recursive size in the **Size** column, computed in the background for visible rows. The cell stays blank until the calculation finishes, and `≥` marks a partial result. Network folders stay blank.

In Workbench the folder rows' Size cell shows the size Workbench measures for that folder, with no second walk: it stays blank until the first count, “≥” marks a folder still being measured or cut short after it was counted, and a folder on another filesystem or in a non-local location stays blank. Workbench counts up to 2,000,000 entries in a folder and List 1,000,000, and a subfolder that is itself a mount point is not scanned in Workbench.

When the measure of the open folder runs out of its entry limit before it reaches some subfolders, those are measured one by one afterwards, each with its own limit, and their rows fill in a little later; a subfolder that runs out of its own limit keeps “≥”. This second pass takes the first 1,000 such subfolders by name, and starts no further subfolder once it has counted 2,000,000 entries; the two subfolders being measured at that moment finish within their own 2,000,000-entry limits, so the pass counts at most about 6,000,000 entries. It stops when you leave the folder or refresh, and a subfolder it does not reach stays blank.

A file with several hard links counts once in Workbench, in the folder total and in the rows together, whichever subfolder is measured first, and once per name in List. The same holds across the first measure and the second pass for up to 100,000 hard-linked files; past that, a later name counts again.

Every subfolder row is filled, however many there are, up to the 100,000 largest subfolders beyond the 512 the picture names; in a folder with more than that, the smallest rows stay blank.

Workbench measures a folder when it opens it and again when its own entries change; a change deeper down, in a subfolder, is not noticed until Refresh (F5, the menu or the toolbar), which measures the open folder again, and its second pass measures the subfolders the first walk did not reach again, while every size shown stays until its new one arrives; a subfolder the second pass does not reach goes blank once it ends.

> [!NOTE]
> In large folders, names can appear before details. **Reading…** means a value is still loading; **Unknown** means it could not be read.

## Select items

Click an entry to select it, `Ctrl`+click to toggle it and `Shift`+click to select a range. Drag an area to select several.

| Action | Shortcut |
| --- | --- |
| Select all | `Ctrl+A` |
| Invert selection | `Ctrl+Shift+I` |
| Select by pattern | `Ctrl+S` |
| Clear selection | `Escape` |

Only an unselected entry's icon and name count as the entry; a drag can start from the rest of its row or tile. A selected entry can be dragged from anywhere in its highlight.

Opening uses double-click by default. **Settings → General** offers single-click opening. Supported local archives, and the archive-like files switched on in Settings, open in the archive browser; see [archives](archives.md). **Open With** and default applications are in [opening files](opening-files.md).

## Remember a view per folder

Turn on **Settings → Views → Remember View and Sort per Folder** so the sort menu, list headers and the Grid, List and Workbench choice change only the current folder. The folder keeps them in every tab and window and across restarts, so Downloads can sort by **Created** while everything else sorts by name.

Folders you never changed follow the defaults. A folder left in Workbench opens in Workbench; a folder left in Grid or List stays there even when Workbench is the default. Choose **Use Default View for This Folder**, at the end of the **Sort** menu, to remove its own settings.

A renamed or moved folder starts from the defaults. Columns is a mode for the whole window: choosing it, or leaving it for Grid, List or Workbench, changes the default view, and folders' own view choices do not apply while it is on. Once it is off, a folder with a view of its own shows that view, Workbench included. File picker windows ignore per-folder settings.

## Shorten long filenames

Overflowing names are shortened in the middle by default, keeping the beginning and ending visible. **Settings → Views → Long Filenames** also offers **Shorten at the End**, applied to open panes at once. Grid wraps names over two lines first.

## Show custom folder icons

Luciole shows the icon set by a folder's `metadata::custom-icon-name` or `metadata::custom-icon` attribute (an image's `file://` URI), or by the `Icon=` key in its `.directory` file (a themed icon name or an absolute image path). Changes appear after the next listing, for example `F5`. Network filesystems keep plain icons.

Links show their target's icon with a link emblem.

To choose an icon yourself, click **Change Icon…** in a local folder's Properties header. The choice is stored in the folder's `metadata::custom-icon-name` and its `.directory` file. Choose **Reset to Default** to restore the standard icon.

## Read the status bar

The status bar at the bottom of the file pane reports actions, search and selection. **Settings → Appearance → Always Show Status Bar** keeps it visible when idle, with the item count and free space when available.

For undo and redo of file changes, see [undo and redo](undo-redo.md).

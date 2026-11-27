# Archives

Create, browse and extract archives without leaving Luciole.

## Create an archive

Select native files or folders and choose **Compress**. Luciole creates ZIP, TAR and 7z archives. Compressed TAR variants (gzip, bzip2, XZ, Zstandard) depend on the installed archive library. When creating compressed archives, you can choose between Fast, Balanced and Maximum compression presets; uncompressed TAR skips this choice.

Symbolic links are stored as links with their original target text, not their contents. Absolute, outside-folder, missing and cyclic targets can be archived this way.

> [!NOTE]
> Luciole refuses to browse or extract an archive that contains escaping or cyclic links, even one it created itself.

## Browse an archive

Double-click, press `Enter` or choose **Open** on a supported local archive, or choose **Browse Archive**. The read-only archive page shows:

- the name, format, compression, size and a contents summary
- breadcrumbs for folders inside the archive
- members with sortable **Name**, **Type**, **Size** and **Modified** columns

**Open** on a member opens that file in its associated application. Members opened this way are limited to 128 MiB; use extraction for larger files. If another file operation is still finishing, Luciole says so instead of opening.

## Extract

Use **Extract Here** or **Extract To**, or the **Extract…** and **Extract Here** buttons on the archive page. They act on the selected members, or on the whole archive when nothing is selected. Extraction uses the normal conflict dialog, see [file operations](file-operations.md#resolve-name-conflicts).

You can also drop one archive or archive-like file on a folder and choose **Extract Here** from the menu.

Before extracting, Luciole checks that temporary storage and the destination have enough space. This is an estimate, so write errors are still reported. Archives that are unsafe or unsupported are refused with a message.

## Supported inputs

| Input | Notes |
|---|---|
| ZIP, TAR, 7z, RAR | Depends on the archive capabilities available on your system |
| Encrypted 7z | Needs a system 7-Zip executable |
| Encrypted RAR | Not supported |
| Split ZIP `name.zip.001`, `.002`, ... | Select `.001`, with all contiguous parts beside it |
| Spanned `.z01` sets | Not supported; split-archive creation is not supported either |
| `dump.sql.gz` (`.gz`, `.bz2`, `.xz`, `.zst` without `.tar`) | Browses and extracts as one file without the suffix; size is unknown until extracted |

Luciole asks for a password when one is needed and never stores it.

A gzip file, a `.tar.gz` or a single `.gz`, is read to its end before anything is listed or extracted, and is refused when its checksum or length does not match, when it is cut, or when anything but zero padding follows its last member.

Luciole also browses and extracts, but never creates, TARs compressed with lz4 (`.tar.lz4`, `.tlz4`), lzma (`.tar.lzma`, `.tlz`), lzip (`.tar.lz`) or compress (`.tar.Z`, `.taz`), and `ar` archives (`.a`, `.ar`, such as static libraries) and `cpio` archives (`.cpio`). The single-file row above covers only `.gz`, `.bz2`, `.xz` and `.zst`: a lone `notes.lz4`, `.lzma`, `.lz` or `.Z` file is not an archive. Extract Here drops these suffixes, so `logs.tar.lz4` extracts into `logs` and `libfoo.a` into `libfoo`. The symbol tables of `ar` libraries are not listed. A `cpio` file in the newc format that stores a hard link's data under another link is refused, because extracting it would produce an empty file. A `.tar.lz4` file needs the lz4 library in the libarchive build and is not recognized without it; Luciole never starts an external lz4 program.

## Open archive-like files

Many file types are real archives under another name and are normally opened by an application: a `.jar` is a ZIP file, an `.epub` is a ZIP file, a `.cbr` is a RAR file and a `.crate` is a gzip-compressed TAR file. Luciole calls them archive containers. Its context menu always offers Browse Archive, Extract Here and Extract To for them, exactly as for a `.zip`, and the same validation applies. The drop menu recognizes these files by suffix. Extract Here and the default folder of Extract To drop a recognized suffix, so `app.jar` extracts into `app`; a file recognized only by its content type, with no such suffix, extracts into a folder with the file's own name and so meets the usual already-exists choice, as a real archive without a suffix does. Browsing a container never runs it or hands it to another program.

Open (double-click, Enter or Open) uses the file's default application, except for the types the system has no content type of its own for and reports as a plain ZIP or TAR archive: `.ear`, `.aar`, `.apks`, `.xapk`, `.vsix`, `.nupkg`, `.whl`, `.egg`, `.ipa`, `.crate`, `.gem`, `.usdz`, `.mcpack`, `.mcworld`, `.mcaddon`, `.mctemplate`, `.pk3`, `.pk4`, `.love` and `.sb3`. These open in the archive browser, as a `.zip` does, until switched off. The default is fixed by these suffixes, not looked up per file, and a list of your own in `open_as_archive` replaces it, so the types above stop browsing unless the list includes them. Settings → Archive-Like Files has one switch per type, shown on for those types; a type that is switched on opens in the archive browser, and its entries are listed in the Workbench preview. The context menu and the drop menu offer Browse Archive and Extract Here for every type, switched on or not. The setting is the `open_as_archive` list in [configuration](configuration.md#browser-preferences). A type is recognized by its suffix, in any letter case, and, where the system names a type of its own for it, by that type. A file of a type whose reader this build lacks, such as a `.crate` without gzip support, is not an archive container.

The types, by the families the Settings page groups them in:

- Java: Java Archive (.jar), Web Application Archive (.war), Enterprise Archive (.ear), Android Library (.aar)
- Android: Android Package (.apk), Android App Bundle (.aab), Android Package Set (.apks), Extended Android Package (.xapk)
- Office Open XML: Word Document (.docx), Word Macro Document (.docm), Word Template (.dotx), Word Macro Template (.dotm), Excel Workbook (.xlsx), Excel Macro Workbook (.xlsm), Excel Template (.xltx), Excel Macro Template (.xltm), PowerPoint Presentation (.pptx), PowerPoint Macro Presentation (.pptm), PowerPoint Template (.potx), PowerPoint Macro Template (.potm), PowerPoint Slide Show (.ppsx)
- OpenDocument: OpenDocument Text (.odt), OpenDocument Text Template (.ott), OpenDocument Spreadsheet (.ods), OpenDocument Spreadsheet Template (.ots), OpenDocument Presentation (.odp), OpenDocument Presentation Template (.otp), OpenDocument Drawing (.odg), OpenDocument Drawing Template (.otg), OpenDocument Formula (.odf), OpenDocument Database (.odb)
- Books and Comics: EPUB Book (.epub), Comic Book, ZIP (.cbz), Comic Book, RAR (.cbr), Comic Book, 7z (.cb7), Comic Book, TAR (.cbt)
- Extensions and Packages: Firefox Extension (.xpi), Visual Studio Code Extension (.vsix), NuGet Package (.nupkg), Python Wheel (.whl), Python Egg (.egg), OpenOffice Extension (.oxt), iOS App (.ipa), Windows App Package (.appx), Windows MSIX Package (.msix), Rust Crate (.crate), Ruby Gem (.gem)
- Graphics, Maps, and 3D: Google Earth Placemarks (.kmz), 3D Manufacturing Model (.3mf), Universal Scene Description (.usdz), OpenRaster Image (.ora), Krita Document (.kra)
- Games and Projects: Minecraft Pack (.mcpack), Minecraft World (.mcworld), Minecraft Add-On (.mcaddon), Minecraft World Template (.mctemplate), Modrinth Modpack (.mrpack), Quake III Package (.pk3), Doom 3 Package (.pk4), LÖVE Game (.love), Scratch Project (.sb3)

A Workbench preview shows a spreadsheet as a table, not as a listing, whatever the setting.

> [!TIP]
> Right-click an archive and choose **Open With** to open it in another installed application. Luciole has no preferred external archiver.

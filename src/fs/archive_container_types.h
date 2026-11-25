#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace luciole {
  // The families the Settings page groups archive containers under.
  enum class ArchiveFamily : std::uint8_t {
    Java,
    Android,
    OfficeOpenXml,
    OpenDocument,
    Books,
    Packages,
    Graphics,
    Games
  };
  inline constexpr std::size_t kArchiveFamilyCount = 8;

  // The archive format a container really is.
  enum class ArchiveContainerFormat : std::uint8_t { Zip, Tar, TarGzip, SevenZip, Rar };

  // A file type that is an archive under another name and is normally opened by an application.
  struct ArchiveContainerType {
    std::string_view name;  // the suffix without the dot, in lower case; also its spelling in settings.toml
    std::string_view label; // as the Settings page shows it
    ArchiveFamily family;
    ArchiveContainerFormat format;
    std::string_view mime; // the type of its own the system reports for it; empty when the table carries none
    // The system reports it as a plain archive, so it opens in the archive browser until switched off.
    bool browsesByDefault = false;
  };

  namespace archive_container_table {
    using enum ArchiveFamily;
    using enum ArchiveContainerFormat;
    inline constexpr auto kArchiveContainerTypes = std::to_array<ArchiveContainerType>({
        {"jar", "Java Archive (.jar)", Java, Zip, "application/java-archive"},
        {"war", "Web Application Archive (.war)", Java, Zip, ""},
        {"ear", "Enterprise Archive (.ear)", Java, Zip, "", true},
        {"aar", "Android Library (.aar)", Java, Zip, "", true},
        {"apk", "Android Package (.apk)", Android, Zip, "application/vnd.android.package-archive"},
        {"aab", "Android App Bundle (.aab)", Android, Zip, "application/vnd.android.app-bundle"},
        {"apks", "Android Package Set (.apks)", Android, Zip, "", true},
        {"xapk", "Extended Android Package (.xapk)", Android, Zip, "", true},
        {"docx", "Word Document (.docx)", OfficeOpenXml, Zip,
         "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"docm", "Word Macro Document (.docm)", OfficeOpenXml, Zip, "application/vnd.ms-word.document.macroEnabled.12"},
        {"dotx", "Word Template (.dotx)", OfficeOpenXml, Zip,
         "application/vnd.openxmlformats-officedocument.wordprocessingml.template"},
        {"dotm", "Word Macro Template (.dotm)", OfficeOpenXml, Zip, "application/vnd.ms-word.template.macroEnabled.12"},
        {"xlsx", "Excel Workbook (.xlsx)", OfficeOpenXml, Zip,
         "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"xlsm", "Excel Macro Workbook (.xlsm)", OfficeOpenXml, Zip, "application/vnd.ms-excel.sheet.macroEnabled.12"},
        {"xltx", "Excel Template (.xltx)", OfficeOpenXml, Zip,
         "application/vnd.openxmlformats-officedocument.spreadsheetml.template"},
        {"xltm", "Excel Macro Template (.xltm)", OfficeOpenXml, Zip,
         "application/vnd.ms-excel.template.macroEnabled.12"},
        {"pptx", "PowerPoint Presentation (.pptx)", OfficeOpenXml, Zip,
         "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"pptm", "PowerPoint Macro Presentation (.pptm)", OfficeOpenXml, Zip,
         "application/vnd.ms-powerpoint.presentation.macroEnabled.12"},
        {"potx", "PowerPoint Template (.potx)", OfficeOpenXml, Zip,
         "application/vnd.openxmlformats-officedocument.presentationml.template"},
        {"potm", "PowerPoint Macro Template (.potm)", OfficeOpenXml, Zip,
         "application/vnd.ms-powerpoint.template.macroEnabled.12"},
        {"ppsx", "PowerPoint Slide Show (.ppsx)", OfficeOpenXml, Zip,
         "application/vnd.openxmlformats-officedocument.presentationml.slideshow"},
        {"odt", "OpenDocument Text (.odt)", OpenDocument, Zip, "application/vnd.oasis.opendocument.text"},
        {"ott", "OpenDocument Text Template (.ott)", OpenDocument, Zip,
         "application/vnd.oasis.opendocument.text-template"},
        {"ods", "OpenDocument Spreadsheet (.ods)", OpenDocument, Zip, "application/vnd.oasis.opendocument.spreadsheet"},
        {"ots", "OpenDocument Spreadsheet Template (.ots)", OpenDocument, Zip,
         "application/vnd.oasis.opendocument.spreadsheet-template"},
        {"odp", "OpenDocument Presentation (.odp)", OpenDocument, Zip,
         "application/vnd.oasis.opendocument.presentation"},
        {"otp", "OpenDocument Presentation Template (.otp)", OpenDocument, Zip,
         "application/vnd.oasis.opendocument.presentation-template"},
        {"odg", "OpenDocument Drawing (.odg)", OpenDocument, Zip, "application/vnd.oasis.opendocument.graphics"},
        {"otg", "OpenDocument Drawing Template (.otg)", OpenDocument, Zip,
         "application/vnd.oasis.opendocument.graphics-template"},
        {"odf", "OpenDocument Formula (.odf)", OpenDocument, Zip, "application/vnd.oasis.opendocument.formula"},
        {"odb", "OpenDocument Database (.odb)", OpenDocument, Zip, "application/vnd.oasis.opendocument.base"},
        {"epub", "EPUB Book (.epub)", Books, Zip, "application/epub+zip"},
        {"cbz", "Comic Book, ZIP (.cbz)", Books, Zip, "application/vnd.comicbook+zip"},
        {"cbr", "Comic Book, RAR (.cbr)", Books, Rar, "application/vnd.comicbook-rar"},
        {"cb7", "Comic Book, 7z (.cb7)", Books, SevenZip, "application/x-cb7"},
        {"cbt", "Comic Book, TAR (.cbt)", Books, Tar, "application/x-cbt"},
        {"xpi", "Firefox Extension (.xpi)", Packages, Zip, "application/x-xpinstall"},
        {"vsix", "Visual Studio Code Extension (.vsix)", Packages, Zip, "", true},
        {"nupkg", "NuGet Package (.nupkg)", Packages, Zip, "", true},
        {"whl", "Python Wheel (.whl)", Packages, Zip, "", true},
        {"egg", "Python Egg (.egg)", Packages, Zip, "", true},
        {"oxt", "OpenOffice Extension (.oxt)", Packages, Zip, "application/vnd.openofficeorg.extension"},
        {"ipa", "iOS App (.ipa)", Packages, Zip, "", true},
        {"appx", "Windows App Package (.appx)", Packages, Zip, "application/appx"},
        {"msix", "Windows MSIX Package (.msix)", Packages, Zip, "application/msix"},
        {"crate", "Rust Crate (.crate)", Packages, TarGzip, "", true},
        {"gem", "Ruby Gem (.gem)", Packages, Tar, "", true},
        {"kmz", "Google Earth Placemarks (.kmz)", Graphics, Zip, "application/vnd.google-earth.kmz"},
        {"3mf", "3D Manufacturing Model (.3mf)", Graphics, Zip, "model/3mf"},
        {"usdz", "Universal Scene Description (.usdz)", Graphics, Zip, "", true},
        {"ora", "OpenRaster Image (.ora)", Graphics, Zip, "image/openraster"},
        {"kra", "Krita Document (.kra)", Graphics, Zip, "application/x-krita"},
        {"mcpack", "Minecraft Pack (.mcpack)", Games, Zip, "", true},
        {"mcworld", "Minecraft World (.mcworld)", Games, Zip, "", true},
        {"mcaddon", "Minecraft Add-On (.mcaddon)", Games, Zip, "", true},
        {"mctemplate", "Minecraft World Template (.mctemplate)", Games, Zip, "", true},
        {"mrpack", "Modrinth Modpack (.mrpack)", Games, Zip, "application/x-modrinth-modpack+zip"},
        {"pk3", "Quake III Package (.pk3)", Games, Zip, "", true},
        {"pk4", "Doom 3 Package (.pk4)", Games, Zip, "", true},
        {"love", "LÖVE Game (.love)", Games, Zip, "", true},
        {"sb3", "Scratch Project (.sb3)", Games, Zip, "", true},
    });
  } // namespace archive_container_table
  // The one list of archive containers: recognition, the setting, the Settings page and the tests read it.
  using archive_container_table::kArchiveContainerTypes;
  inline constexpr std::size_t kArchiveContainerCount = kArchiveContainerTypes.size();
  static_assert(
      std::ranges::none_of(
          kArchiveContainerTypes,
          [](const ArchiveContainerType& type) { return type.browsesByDefault && !type.mime.empty(); }
      ),
      "a type that browses by default has no content type of its own"
  );

  // Which types open in the archive browser: bit i is kArchiveContainerTypes[i].
  using ArchiveContainerSet = std::bitset<kArchiveContainerCount>;

  // The types marked browsesByDefault: the default of the open_as_archive setting.
  constexpr ArchiveContainerSet archiveContainersBrowsingByDefault() {
    ArchiveContainerSet set;
    for (std::size_t index = 0; index < kArchiveContainerCount; ++index) {
      set.set(index, kArchiveContainerTypes[index].browsesByDefault);
    }
    return set;
  }

  constexpr std::string_view archiveFamilyLabel(ArchiveFamily family) {
    switch (family) {
    case ArchiveFamily::Java:
      return "Java";
    case ArchiveFamily::Android:
      return "Android";
    case ArchiveFamily::OfficeOpenXml:
      return "Office Open XML";
    case ArchiveFamily::OpenDocument:
      return "OpenDocument";
    case ArchiveFamily::Books:
      return "Books and Comics";
    case ArchiveFamily::Packages:
      return "Extensions and Packages";
    case ArchiveFamily::Graphics:
      return "Graphics, Maps, and 3D";
    case ArchiveFamily::Games:
      return "Games and Projects";
    }
    return {};
  }

  // The index of the type spelled `name` in settings.toml, exactly as the table spells it.
  constexpr std::optional<std::size_t> archiveContainerByName(std::string_view name) {
    for (std::size_t index = 0; index < kArchiveContainerCount; ++index) {
      if (kArchiveContainerTypes[index].name == name) {
        return index;
      }
    }
    return std::nullopt;
  }
} // namespace luciole

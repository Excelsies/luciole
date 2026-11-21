#include "app/page/workbench_open.h"

#include "app/page/workbench_session.h"
#include "fs/gio_ptr.h"

#include <glib.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace luciole {
  namespace {
    // A file no listing holds, such as a piece of the instrument: what the file system says of it, and the type its
    // name gives.
    ArchiveFacts factsOfFile(const Location& location) {
      ArchiveFacts facts{.location = location, .type = FileType::Unknown, .symlink = false, .readable = {}, .mime = {}};
      const auto path = location.nativePath();
      struct stat info{};
      if (!path || ::lstat(path->c_str(), &info) != 0) {
        return facts;
      }
      facts.symlink = S_ISLNK(info.st_mode);
      facts.type = S_ISREG(info.st_mode) ? FileType::Regular : FileType::Unknown;
      facts.readable = ::access(path->c_str(), R_OK) == 0;
      const auto name = location.basename();
      const GString type(g_content_type_guess(name ? name->c_str() : nullptr, nullptr, 0, nullptr));
      const GString mime(type ? g_content_type_get_mime_type(type.get()) : nullptr);
      facts.mime = mime ? mime.get() : "";
      return facts;
    }
  } // namespace

  ArchiveFacts archiveFactsFor(const Location& location, std::span<const DirectoryModel* const> models) {
    for (const auto* model : models) {
      if (const auto id = rowOf(*model, location)) {
        if (const auto* entry = model->entry(*id)) {
          auto facts = ActionRegistry::archiveFacts(*entry);
          if (facts.mime.empty()) {
            facts.mime = factsOfFile(location).mime; // its type is not read yet
          }
          return facts;
        }
      }
    }
    return factsOfFile(location);
  }

  void openWorkbenchFile(
      const Location& location, std::span<const DirectoryModel* const> models, bool chooser,
      const WorkbenchOpenHooks& hooks
  ) {
    if (!hooks.visited || !hooks.launch || !hooks.browseArchive) {
      throw std::invalid_argument("Opening a Workbench file needs every hook.");
    }
    hooks.visited(location);
    const auto facts = archiveFactsFor(location, models);
    if (!chooser && ActionRegistry::browsableFile(facts)) {
      hooks.browseArchive(facts);
    } else {
      hooks.launch(location);
    }
  }
} // namespace luciole

#include "app/cli.h"

#include "app/application.h"
#include "config/settings.h"
#include "luciole_build_info.h"
#include "luciole_completions.h"
#include "media/bink2_poster.h"
#include "thumbnails/font_preview.h"
#include "thumbnails/mesh_preview.h"
#include "thumbnails/preview_metadata.h"
#include "thumbnails/sheet_preview.h"
#include "thumbnails/thumbnail_worker.h"

#include <cstdio>
#include <cstdlib>
#include <print>

namespace {

  constexpr std::string_view kHelp =
      R"(Usage: luciole [--help | --version | completions <shell> | config validate <file>]

Open Luciole in Home, or pass one directory path/URI to browse.
Usage: luciole [directory]

Options:
  --help                 Show this help
  --version              Show version and explicit build revision
  completions <shell>    Print bash, zsh, or fish shell completions
  config validate <file> Validate one TOML settings file without opening a window
)";

  int printCompletions(std::string_view shell) {
    if (shell == "bash") {
      std::print("{}", luciole::completions::kBash);
    } else if (shell == "zsh") {
      std::print("{}", luciole::completions::kZsh);
    } else if (shell == "fish") {
      std::print("{}", luciole::completions::kFish);
    } else {
      std::println(stderr, "luciole: unsupported completion shell '{}'; expected bash, zsh, or fish", shell);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

} // namespace

int luciole::runCli(std::span<const std::string_view> args) {
  if (!args.empty() && args.front() == "--preview-metadata-worker") {
    return runPreviewMetadataWorker(args.subspan(1));
  }
  if (!args.empty() && args.front() == "--thumbnail-worker") {
    return runThumbnailWorker(args.subspan(1));
  }
  if (!args.empty() && args.front() == "--font-preview-worker") {
    return runFontPreviewWorker(args.subspan(1));
  }
  if (!args.empty() && args.front() == "--bink2-facts-worker") {
    return media::runBink2FactsWorker(args.subspan(1));
  }
  if (!args.empty() && args.front() == "--bink2-poster-worker") {
    return media::runBink2PosterWorker(args.subspan(1));
  }
  if (!args.empty() && args.front() == "--mesh-preview-worker") {
    return runMeshPreviewWorker(args.subspan(1));
  }
  if (!args.empty() && args.front() == "--sheet-preview-worker") {
    return runSheetPreviewWorker(args.subspan(1));
  }
  if (args.size() == 1 && args.front() == "--file-manager-service") {
    return runApplication(std::nullopt, StartupMode::FileManagerActivation);
  }
  if (args.size() == 1 && args.front() == "--file-chooser-service") {
    return runApplication(std::nullopt, StartupMode::FileChooserService);
  }
  if (args.size() == 1 && args.front() == "--help") {
    std::print("{}", kHelp);
    return EXIT_SUCCESS;
  }
  if (args.size() == 1 && args.front() == "--version") {
    std::println("luciole {} ({})", LUCIOLE_VERSION, LUCIOLE_BUILD_REVISION);
    return EXIT_SUCCESS;
  }
  if (args.size() == 2 && args.front() == "completions") {
    return printCompletions(args[1]);
  }
  if (args.size() == 3 && args[0] == "config" && args[1] == "validate") {
    const auto issues = validateConfigFile(std::filesystem::path(args[2]));
    if (!issues.empty()) {
      for (const auto& issue : issues) {
        std::println(stderr, "luciole: {}", issue.format());
      }
      return EXIT_FAILURE;
    }
    std::println("Configuration is valid.");
    return EXIT_SUCCESS;
  }
  if (args.empty()) {
    return runApplication();
  }
  if (args.size() == 1 && !args.front().starts_with('-') && args.front() != "config" && args.front() != "completions") {
    return runApplication(std::string(args.front()));
  }
  if (args.size() == 2 && args.front() == "--") {
    return runApplication(std::string(args[1]));
  }
  std::println(stderr, "luciole: invalid arguments; use --help for available commands");
  return EXIT_FAILURE;
}

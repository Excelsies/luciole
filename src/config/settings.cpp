#include "config/settings.h"

#include "config/preference_schema.h"
#include "config/toml_decoder.h"
#include "fs/xdg_dirs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <concepts>
#include <cstdio>
#include <fcntl.h>
#include <format>
#include <iterator>
#include <optional>
#include <print>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>

using namespace luciole::preference_schema;

namespace {
  constexpr std::size_t kMaxConfigBytes = 256 * 1024;

  // One optional override per descriptor; nullopt inherits.
  template <class... T> std::tuple<std::optional<T>...> patchFor(const std::tuple<Descriptor<T>...>& descriptors);
  using Patch = decltype(patchFor(kPreferences));

  struct FileDescriptor {
    int fd;
    ~FileDescriptor() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    explicit FileDescriptor(int value) : fd(value) {}
  };

  luciole::ConfigIssue ioIssue(const std::filesystem::path& file, std::string message) {
    return {.file = file, .key = {}, .message = std::move(message)};
  }

  std::string ioMessage(int error) { return std::error_code(error, std::generic_category()).message(); }

  std::expected<std::optional<std::string>, luciole::ConfigIssue>
  readFile(const std::filesystem::path& file, bool optional) {
    if (optional) {
      struct stat linkInfo{};
      if (::lstat(file.c_str(), &linkInfo) < 0) {
        const int error = errno;
        if (error == ENOENT) {
          return std::nullopt;
        }
        return std::unexpected(ioIssue(file, ioMessage(error)));
      }
    }
    // Declarative config symlinks are allowed. Nonblocking open plus fstat
    // rejects FIFOs/devices without waiting for a writer or reading them.
    const FileDescriptor descriptor(::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (descriptor.fd < 0) {
      return std::unexpected(ioIssue(file, ioMessage(errno)));
    }
    struct stat info{};
    if (::fstat(descriptor.fd, &info) < 0) {
      return std::unexpected(ioIssue(file, ioMessage(errno)));
    }
    if (!S_ISREG(info.st_mode)) {
      return std::unexpected(ioIssue(file, "expected a regular TOML file"));
    }
    std::string text;
    std::array<char, 8192> buffer{};
    while (true) {
      const auto count = ::read(descriptor.fd, buffer.data(), buffer.size());
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        return std::unexpected(ioIssue(file, ioMessage(errno)));
      }
      if (count == 0) {
        return text;
      }
      const auto size = static_cast<std::size_t>(count);
      if (text.size() + size > kMaxConfigBytes) {
        return std::unexpected(ioIssue(file, "configuration exceeds the 256 KiB limit"));
      }
      text.append(buffer.data(), size);
    }
  }

  enum class UnknownSettings { Reject, Warn };

  std::expected<Patch, luciole::ConfigIssues> parse(
      const std::filesystem::path& file, std::string_view text, UnknownSettings unknown = UnknownSettings::Reject,
      toml::table* document = nullptr, const luciole::KeybindConfig* keybindBase = nullptr
  ) {
    auto table = luciole::parseToml(text, file);
    if (!table) {
      return std::unexpected(std::move(table.error()));
    }
    luciole::TomlDecoder decoder(file);
    luciole::TomlDecoder warnings(file);
    const auto unknownSetting = [&](const toml::node& node, std::string key) {
      if (unknown == UnknownSettings::Reject) {
        decoder.issue(node, std::move(key), "unknown setting");
      } else {
        warnings.issue(node, std::move(key), "unknown setting; ignored");
      }
    };
    Patch patch;
    for (const auto& [key, node] : *table) {
      if (key == "schema_version") {
        if (!luciole::isSchemaVersion(node)) {
          decoder.issue(
              node, "schema_version", std::format("expected integer schema version {}", luciole::kSchemaVersion)
          );
        }
      } else if (key == "browser") {
        const auto* browser = node.as_table();
        if (browser == nullptr) {
          decoder.issue(node, "browser", "expected a table");
          continue;
        }
        for (const auto& [name, value] : *browser) {
          bool known = false;
          forEachPreference([&](auto index) {
            const auto& descriptor = std::get<index>(kPreferences);
            if (descriptor.key != name.str()) {
              return;
            }
            known = true;
            const auto decoded = [&] {
              if constexpr (
                  std::same_as<typename std::remove_cvref_t<decltype(descriptor)>::Value, luciole::KeybindConfig>
              ) {
                return decode(descriptor.rule, value, keybindBase != nullptr ? *keybindBase : luciole::KeybindConfig{});
              } else {
                return decode(descriptor.rule, value);
              }
            }();
            if (decoded && accepts(descriptor.rule, *decoded)) {
              std::get<index>(patch) = decoded;
            } else {
              decoder.issue(value, preferenceKey(descriptor), expected(descriptor.rule));
            }
          });
          if (!known) {
            unknownSetting(value, std::format("browser.{}", name.str()));
          }
        }
      } else {
        unknownSetting(node, std::string(key.str()));
      }
    }
    if (!decoder.issues.empty()) {
      return std::unexpected(std::move(decoder.issues));
    }
    for (const auto& warning : warnings.issues) {
      std::println(stderr, "luciole: warning: {}", warning.format());
    }
    if (document != nullptr) {
      *document = std::move(*table);
    }
    return patch;
  }

  void applyPatch(luciole::Settings& settings, const Patch& patch) {
    forEachPreference([&](auto index) {
      if (const auto& value = std::get<index>(patch)) {
        std::get<index>(kPreferences).field(settings.browser) = *value;
      }
    });
  }

  void editPatch(Patch& patch, const luciole::OverrideEdit& edit) {
    visitPreference(edit.preference(), [&](auto index) {
      std::get<index>(patch) = edit.value(std::get<index>(kPreferences).preference);
    });
  }

  std::string formatPatch(const Patch& patch, toml::table document, const luciole::BrowserSettings& inherited) {
    toml::table browser;
    if (const auto* existing = document["browser"].as_table()) {
      browser = *existing;
    }
    forEachPreference([&](auto index) {
      const auto& descriptor = std::get<index>(kPreferences);
      browser.erase(descriptor.key);
      if (const auto& value = std::get<index>(patch)) {
        if constexpr (std::same_as<typename std::remove_cvref_t<decltype(descriptor)>::Value, luciole::KeybindConfig>) {
          encode(browser, descriptor.key, *value, inherited.keybinds);
        } else {
          encode(browser, descriptor.key, descriptor.rule, *value);
        }
      }
    });
    document.insert_or_assign("schema_version", luciole::kSchemaVersion);
    if (browser.empty()) {
      document.erase("browser");
    } else {
      document.insert_or_assign("browser", std::move(browser));
    }
    std::ostringstream output;
    output << toml::toml_formatter{
        document,
        (toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings)
            | toml::format_flags::relaxed_float_precision
    };
    auto text = output.str();
    if (!text.ends_with('\n')) {
      text.push_back('\n');
    }
    return text;
  }
} // namespace

template <class T> T& luciole::preferenceValue(BrowserSettings& settings, PreferenceKey<T> key) {
  T* field = nullptr;
  visitPreference(key.id(), [&](auto index) {
    const auto& descriptor = std::get<index>(kPreferences);
    // A key's type always matches its descriptor; other instantiations never run.
    if constexpr (std::same_as<typename std::remove_cvref_t<decltype(descriptor)>::Value, T>) {
      field = &descriptor.field(settings);
    }
  });
  return *field;
}

template <class T> const T& luciole::preferenceValue(const BrowserSettings& settings, PreferenceKey<T> key) {
  return preferenceValue(const_cast<BrowserSettings&>(settings), key);
}

template std::string& luciole::preferenceValue(BrowserSettings&, PreferenceKey<std::string>);
template luciole::KeybindConfig& luciole::preferenceValue(BrowserSettings&, PreferenceKey<KeybindConfig>);
template const luciole::KeybindConfig& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<KeybindConfig>);
template const std::string& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<std::string>);
template bool& luciole::preferenceValue(BrowserSettings&, PreferenceKey<bool>);
template int& luciole::preferenceValue(BrowserSettings&, PreferenceKey<int>);
template double& luciole::preferenceValue(BrowserSettings&, PreferenceKey<double>);
template luciole::ActivationMode& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ActivationMode>);
template luciole::TypingMode& luciole::preferenceValue(BrowserSettings&, PreferenceKey<TypingMode>);
template luciole::DropAction& luciole::preferenceValue(BrowserSettings&, PreferenceKey<DropAction>);
template luciole::SortField& luciole::preferenceValue(BrowserSettings&, PreferenceKey<SortField>);
template luciole::ViewMode& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ViewMode>);
template luciole::ViewDensity& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ViewDensity>);
template luciole::FilenameTruncation& luciole::preferenceValue(BrowserSettings&, PreferenceKey<FilenameTruncation>);
template luciole::DetailsColumnOrder& luciole::preferenceValue(BrowserSettings&, PreferenceKey<DetailsColumnOrder>);
template luciole::ThumbnailPolicy& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ThumbnailPolicy>);
template luciole::ChooserStartFolder& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ChooserStartFolder>);
template luciole::WorkbenchInstrument& luciole::preferenceValue(BrowserSettings&, PreferenceKey<WorkbenchInstrument>);
template luciole::WorkbenchRibbon& luciole::preferenceValue(BrowserSettings&, PreferenceKey<WorkbenchRibbon>);
template luciole::WorkbenchMeasure& luciole::preferenceValue(BrowserSettings&, PreferenceKey<WorkbenchMeasure>);
template luciole::WorkbenchColor& luciole::preferenceValue(BrowserSettings&, PreferenceKey<WorkbenchColor>);
template luciole::AudioVisualKind& luciole::preferenceValue(BrowserSettings&, PreferenceKey<AudioVisualKind>);
template luciole::WorkbenchPreviewPlace&
luciole::preferenceValue(BrowserSettings&, PreferenceKey<WorkbenchPreviewPlace>);
template luciole::ColorScheme& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ColorScheme>);
template luciole::ArchiveContainerSet& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ArchiveContainerSet>);
template const bool& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<bool>);
template const int& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<int>);
template const double& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<double>);
template const luciole::ActivationMode& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ActivationMode>);
template const luciole::TypingMode& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<TypingMode>);
template const luciole::DropAction& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<DropAction>);
template const luciole::ChooserStartFolder&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ChooserStartFolder>);
template const luciole::SortField& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<SortField>);
template const luciole::ViewMode& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ViewMode>);
template const luciole::ViewDensity& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ViewDensity>);
template const luciole::FilenameTruncation&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<FilenameTruncation>);
template const luciole::DetailsColumnOrder&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<DetailsColumnOrder>);
template const luciole::ThumbnailPolicy&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ThumbnailPolicy>);
template const luciole::WorkbenchInstrument&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<WorkbenchInstrument>);
template const luciole::WorkbenchRibbon&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<WorkbenchRibbon>);
template const luciole::WorkbenchMeasure&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<WorkbenchMeasure>);
template const luciole::WorkbenchColor& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<WorkbenchColor>);
template const luciole::AudioVisualKind&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<AudioVisualKind>);
template const luciole::WorkbenchPreviewPlace&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<WorkbenchPreviewPlace>);
template const luciole::ColorScheme& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ColorScheme>);
template const luciole::ArchiveContainerSet&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ArchiveContainerSet>);

std::string_view luciole::sortFieldName(SortField field) {
  const auto* found = std::ranges::find(kSortFields, field, &Named<SortField>::value);
  return found != kSortFields.end() ? found->name : std::string_view{};
}
std::optional<luciole::SortField> luciole::sortFieldFromName(std::string_view name) {
  const auto* found = std::ranges::find(kSortFields, name, &Named<SortField>::name);
  return found != kSortFields.end() ? std::optional(found->value) : std::nullopt;
}
std::string_view luciole::viewModeName(ViewMode mode) {
  const auto* found = std::ranges::find(kViewModes, mode, &Named<ViewMode>::value);
  return found != kViewModes.end() ? found->name : std::string_view{};
}
std::optional<luciole::ViewMode> luciole::viewModeFromName(std::string_view name) {
  const auto* found = std::ranges::find(kViewModes, name, &Named<ViewMode>::name);
  return found != kViewModes.end() ? std::optional(found->value) : std::nullopt;
}

std::vector<luciole::OverrideEdit> luciole::inheritAllPreferences() {
  std::vector<OverrideEdit> edits;
  edits.reserve(kPreferenceCount);
  forEachPreference([&](auto index) { edits.emplace_back(std::get<index>(kPreferences).preference, std::nullopt); });
  return edits;
}

luciole::ConfigPaths luciole::ConfigPaths::fromEnvironment() {
  const auto directory = configHome() / "luciole";
  return {.base = directory / "config.toml", .overrides = directory / "settings.toml"};
}

luciole::ConfigIssues luciole::validateConfigFile(const std::filesystem::path& file) {
  auto text = readFile(file, false);
  if (!text) {
    return {std::move(text.error())};
  }
  auto parsed = parse(file, **text);
  return parsed ? ConfigIssues{} : std::move(parsed.error());
}

static luciole::SettingsResult loadLayers(const luciole::ConfigPaths& paths, bool overrides) {
  luciole::Settings settings;
  luciole::ConfigIssues issues;
  for (const auto* file : {&paths.base, &paths.overrides}) {
    if (file == &paths.overrides && !overrides) {
      continue;
    }
    auto text = readFile(*file, true);
    if (!text) {
      issues.push_back(std::move(text.error()));
      continue;
    }
    if (!*text) {
      continue;
    }
    auto parsed = parse(
        *file, **text, UnknownSettings::Warn, nullptr, file == &paths.overrides ? &settings.browser.keybinds : nullptr
    );
    if (!parsed) {
      auto& errors = parsed.error();
      issues.insert(issues.end(), std::make_move_iterator(errors.begin()), std::make_move_iterator(errors.end()));
    } else {
      applyPatch(settings, *parsed);
    }
  }
  if (!issues.empty()) {
    return std::unexpected(std::move(issues));
  }
  return settings;
}

luciole::SettingsResult luciole::loadSettings(const ConfigPaths& paths) { return loadLayers(paths, true); }
luciole::SettingsResult luciole::loadInheritedSettings(const ConfigPaths& paths) { return loadLayers(paths, false); }

luciole::ConfigService::ConfigService(ConfigPaths paths) : m_paths(std::move(paths)) {}

bool luciole::ConfigService::reload() {
  auto loaded = loadSettings(m_paths);
  if (!loaded) {
    m_issues = std::move(loaded.error());
    return false;
  }
  m_settings = *loaded;
  m_issues.clear();
  return true;
}

luciole::AtomicWriteState luciole::ConfigService::saveOverrides(std::span<const OverrideEdit> edits) {
  if (edits.empty()) {
    return AtomicWriteState::Unchanged;
  }
  m_issues.clear();
  if (!m_paths.base.is_absolute()
      || !m_paths.overrides.is_absolute()
      || m_paths.base.filename() != "config.toml"
      || m_paths.overrides.filename() != "settings.toml"
      || m_paths.base.parent_path().lexically_normal() != m_paths.overrides.parent_path().lexically_normal()) {
    m_issues.push_back(
        ioIssue(m_paths.overrides, "saves require distinct config.toml and settings.toml in one absolute directory")
    );
    return AtomicWriteState::NotCommitted;
  }
  for (const auto& edit : edits) {
    visitPreference(edit.preference(), [&](auto index) {
      const auto& descriptor = std::get<index>(kPreferences);
      if (const auto value = edit.value(descriptor.preference); value && !accepts(descriptor.rule, *value)) {
        m_issues.push_back(
            {.file = m_paths.overrides, .key = preferenceKey(descriptor), .message = "invalid preference value"}
        );
      }
    });
  }
  if (!m_issues.empty()) {
    return AtomicWriteState::NotCommitted;
  }
  Settings candidate;
  Settings inherited;
  const auto result = updateTextFileAtomic(
      m_paths.overrides,
      [&](const std::optional<std::string>& current) -> std::expected<std::optional<std::string>, std::string> {
        std::error_code aliasError;
        if (std::filesystem::equivalent(m_paths.base, m_paths.overrides, aliasError)) {
          return std::unexpected("base configuration and GUI overrides refer to the same file");
        }
        if (aliasError && aliasError != std::errc::no_such_file_or_directory) {
          return std::unexpected(std::format("cannot inspect base configuration: {}", aliasError.message()));
        }
        const auto base = readFile(m_paths.base, true);
        if (!base) {
          m_issues.push_back(base.error());
          return std::unexpected("cannot load base configuration");
        }
        if (*base) {
          const auto parsed = parse(m_paths.base, **base, UnknownSettings::Warn);
          if (!parsed) {
            m_issues = parsed.error();
            return std::unexpected("invalid base configuration");
          }
          applyPatch(candidate, *parsed);
          applyPatch(inherited, *parsed);
        }
        toml::table document;
        auto patch = parse(
            m_paths.overrides, current.value_or(""), UnknownSettings::Warn, &document, &inherited.browser.keybinds
        );
        if (!patch) {
          m_issues = std::move(patch.error());
          return std::unexpected("invalid existing settings");
        }
        const auto previous = *patch;
        for (const auto& edit : edits) {
          editPatch(*patch, edit);
        }
        applyPatch(candidate, *patch);
        if (*patch == previous) {
          return std::nullopt;
        }
        return formatPatch(*patch, std::move(document), inherited.browser);
      }
  );
  if (result.state != AtomicWriteState::NotCommitted) {
    m_settings = std::move(candidate);
  }
  if (!result.error.empty() && m_issues.empty()) {
    m_issues.push_back(ioIssue(m_paths.overrides, result.error));
  }
  return result.state;
}

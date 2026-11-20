#include "config/settings.h"
#include "temp_folder.h"
#include "test_check.h"
#include "ui/workbench/workbench_preferences.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <string>

// The preference for a preview that has no column to show in: read from settings.toml, checked like every other
// value, saved as an override, and handed to the view with the other Workbench choices.
namespace {
  using luciole::WorkbenchPreviewPlace;
  using P = luciole::BrowserPreference;
  using State = luciole::AtomicWriteState;

  struct Fixture {
    luciole::test::TempFolder folder{"luciole-preview-place-preferences"};

    void write(const std::string& text) const {
      std::ofstream file(folder.path() / "config.toml", std::ios::binary);
      file << text;
      TEST_CHECK(file.good());
    }
    luciole::ConfigPaths paths() const {
      return {.base = folder.path() / "config.toml", .overrides = folder.path() / "settings.toml"};
    }
    std::string overrides() const {
      std::ifstream file(paths().overrides, std::ios::binary);
      return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
  };

  void defaultsToJumpAhead() {
    Fixture fixture;
    fixture.write("");
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload());
    const auto& browser = service.settings().browser;
    TEST_CHECK(browser.workbench.previewPlace == WorkbenchPreviewPlace::JumpAheadWhenNarrow);
    TEST_CHECK(luciole::workbenchChoices(browser, false).previewPlace == WorkbenchPreviewPlace::JumpAheadWhenNarrow);
  }

  void readsBothWords() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    fixture.write("[browser]\nworkbench_preview_place = 'hidden_when_narrow'\n");
    TEST_CHECK(luciole::validateConfigFile(fixture.paths().base).empty());
    TEST_CHECK(service.reload());
    TEST_CHECK(service.settings().browser.workbench.previewPlace == WorkbenchPreviewPlace::HiddenWhenNarrow);
    TEST_CHECK(
        luciole::workbenchChoices(service.settings().browser, false).previewPlace
        == WorkbenchPreviewPlace::HiddenWhenNarrow
    );
    fixture.write("[browser]\nworkbench_preview_place = 'jump_ahead_always'\n");
    TEST_CHECK(luciole::validateConfigFile(fixture.paths().base).empty());
    TEST_CHECK(service.reload());
    TEST_CHECK(service.settings().browser.workbench.previewPlace == WorkbenchPreviewPlace::JumpAheadAlways);
    fixture.write("[browser]\nworkbench_preview_place = 'jump_ahead_when_narrow'\n");
    TEST_CHECK(luciole::validateConfigFile(fixture.paths().base).empty());
    TEST_CHECK(service.reload());
    TEST_CHECK(service.settings().browser.workbench.previewPlace == WorkbenchPreviewPlace::JumpAheadWhenNarrow);
  }

  void reportsUnknownWordsAndKeepsTheLastGoodValue() {
    Fixture fixture;
    for (const char* word : {"hidden", "jump_ahead", "Jump_Ahead_Always", "", "column"}) {
      fixture.write(std::format("[browser]\nworkbench_preview_place = '{}'\n", word));
      const auto issues = luciole::validateConfigFile(fixture.paths().base);
      TEST_CHECK(issues.size() == 1);
      TEST_CHECK(issues.front().key == "browser.workbench_preview_place");
      TEST_CHECK(
          issues.front().message == "expected 'jump_ahead_when_narrow', 'jump_ahead_always', or 'hidden_when_narrow'"
      );
    }
    fixture.write("[browser]\nworkbench_preview_place = true\n");
    TEST_CHECK(luciole::validateConfigFile(fixture.paths().base).size() == 1);
    luciole::ConfigService service(fixture.paths());
    fixture.write("[browser]\nworkbench_preview_place = 'hidden_when_narrow'\n");
    TEST_CHECK(service.reload());
    fixture.write("[browser]\nworkbench_preview_place = 'beside'\n");
    TEST_CHECK(!service.reload());
    TEST_CHECK(service.settings().browser.workbench.previewPlace == WorkbenchPreviewPlace::HiddenWhenNarrow);
  }

  void savesAndRestores() {
    Fixture fixture;
    fixture.write("");
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload());
    const std::vector<luciole::OverrideEdit> edits{{P::WorkbenchPreviewPlace, WorkbenchPreviewPlace::JumpAheadAlways}};
    TEST_CHECK(edits.front().value(P::WorkbenchPreviewPlace) == std::optional(WorkbenchPreviewPlace::JumpAheadAlways));
    TEST_CHECK(service.saveOverrides(edits) == State::Durable);
    TEST_CHECK(fixture.overrides().contains("workbench_preview_place = \"jump_ahead_always\""));
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload());
    TEST_CHECK(restored.settings().browser.workbench.previewPlace == WorkbenchPreviewPlace::JumpAheadAlways);
    // Resetting every preference removes it.
    TEST_CHECK(restored.saveOverrides(luciole::inheritAllPreferences()) == State::Durable);
    TEST_CHECK(!fixture.overrides().contains("workbench_preview_place"));
    TEST_CHECK(restored.reload());
    TEST_CHECK(restored.settings().browser.workbench.previewPlace == WorkbenchPreviewPlace::JumpAheadWhenNarrow);
  }
} // namespace

int main() {
  defaultsToJumpAhead();
  readsBothWords();
  reportsUnknownWordsAndKeepsTheLastGoodValue();
  savesAndRestores();
  std::println("preview-place-preferences: all checks passed");
  return 0;
}

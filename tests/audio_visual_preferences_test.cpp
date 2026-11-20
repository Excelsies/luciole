#include "config/settings.h"
#include "temp_folder.h"
#include "test_check.h"
#include "ui/workbench/workbench_preferences.h"

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <string>

// The sound visualizer's two preferences: saved and loaded through settings.toml, checked like every other value,
// and turned into the choice the sound preview applies and back into edits.
namespace {
  using luciole::AudioVisualKind;
  using P = luciole::BrowserPreference;
  using State = luciole::AtomicWriteState;

  struct Fixture {
    luciole::test::TempFolder folder{"luciole-audio-visual-preferences"};

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

  void defaultsAreOnWithCosmicPeanut() {
    Fixture fixture;
    fixture.write("");
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload());
    const auto& workbench = service.settings().browser.workbench;
    TEST_CHECK(workbench.audioVisualizer);
    TEST_CHECK(workbench.audioVisual == AudioVisualKind::CosmicPeanut);
    const AudioVisualChoice choice = luciole::audioVisualChoice(workbench);
    TEST_CHECK(choice.on && choice.kind == AudioVisualKind::CosmicPeanut);
  }

  void readsEveryWord() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    const std::array<std::string, luciole::kAudioVisualCount> words{"cosmic-peanut", "pulsar",   "bars",
                                                                    "orbit",         "halo",     "radar",
                                                                    "eclipse",       "dark-sun", "stargate"};
    for (std::size_t index = 0; index < words.size(); ++index) {
      fixture.write(std::format("[browser]\nworkbench_audio_visual = '{}'\n", words.at(index)));
      TEST_CHECK(luciole::validateConfigFile(fixture.paths().base).empty());
      TEST_CHECK(service.reload());
      TEST_CHECK(service.settings().browser.workbench.audioVisual == luciole::kAudioVisualKinds.at(index));
    }
    fixture.write("[browser]\nworkbench_audio_visualizer = false\nworkbench_audio_visual = 'radar'\n");
    TEST_CHECK(service.reload());
    const AudioVisualChoice choice = luciole::audioVisualChoice(service.settings().browser.workbench);
    TEST_CHECK(!choice.on && choice.kind == AudioVisualKind::Radar);
  }

  void reportsBadValues() {
    Fixture fixture;
    for (const char* word : {"dark_sun", "Dark Sun", "Halo", "", "cosmic peanut"}) {
      fixture.write(std::format("[browser]\nworkbench_audio_visual = '{}'\n", word));
      const auto issues = luciole::validateConfigFile(fixture.paths().base);
      TEST_CHECK(issues.size() == 1);
      TEST_CHECK(issues.front().key == "browser.workbench_audio_visual");
      TEST_CHECK(
          issues.front().message
          == "expected 'cosmic-peanut', 'pulsar', 'bars', 'orbit', 'halo', 'radar', 'eclipse', 'dark-sun', or "
             "'stargate'"
      );
    }
    fixture.write("[browser]\nworkbench_audio_visual = 3\n");
    TEST_CHECK(luciole::validateConfigFile(fixture.paths().base).size() == 1);
    fixture.write("[browser]\nworkbench_audio_visualizer = 'yes'\n");
    const auto issues = luciole::validateConfigFile(fixture.paths().base);
    TEST_CHECK(issues.size() == 1);
    TEST_CHECK(issues.front().key == "browser.workbench_audio_visualizer");
    TEST_CHECK(issues.front().message == "expected a boolean");
    // A bad value fails the load and keeps what was loaded before.
    luciole::ConfigService service(fixture.paths());
    fixture.write("[browser]\nworkbench_audio_visual = 'orbit'\n");
    TEST_CHECK(service.reload());
    fixture.write("[browser]\nworkbench_audio_visual = 'spiral'\n");
    TEST_CHECK(!service.reload());
    TEST_CHECK(service.settings().browser.workbench.audioVisual == AudioVisualKind::Orbit);
  }

  void savesAndRestores() {
    Fixture fixture;
    fixture.write("");
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload());
    auto chosen = service.settings().browser.workbench;
    const auto edits = luciole::chooseAudioVisual(chosen, {.on = false, .kind = AudioVisualKind::DarkSun});
    TEST_CHECK(!chosen.audioVisualizer && chosen.audioVisual == AudioVisualKind::DarkSun);
    TEST_CHECK(edits.size() == 2);
    TEST_CHECK(edits.at(0).value(P::WorkbenchAudioVisualizer) == std::optional(false));
    TEST_CHECK(edits.at(1).value(P::WorkbenchAudioVisual) == std::optional(AudioVisualKind::DarkSun));
    TEST_CHECK(service.saveOverrides(edits) == State::Durable);
    const auto saved = fixture.overrides();
    std::println("settings.toml:\n{}", saved);
    TEST_CHECK(saved.contains("workbench_audio_visualizer = false"));
    TEST_CHECK(saved.contains("workbench_audio_visual = \"dark-sun\""));
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload());
    const AudioVisualChoice choice = luciole::audioVisualChoice(restored.settings().browser.workbench);
    TEST_CHECK(!choice.on && choice.kind == AudioVisualKind::DarkSun);
    // Only what changed is written; nothing changed writes nothing.
    auto again = restored.settings().browser.workbench;
    TEST_CHECK(luciole::chooseAudioVisual(again, choice).empty());
    const auto one = luciole::chooseAudioVisual(again, {.on = true, .kind = choice.kind});
    TEST_CHECK(one.size() == 1 && one.front().value(P::WorkbenchAudioVisualizer) == std::optional(true));
    TEST_CHECK(again.audioVisualizer && again.audioVisual == AudioVisualKind::DarkSun);
    // Resetting every preference removes both.
    TEST_CHECK(restored.saveOverrides(luciole::inheritAllPreferences()) == State::Durable);
    TEST_CHECK(!fixture.overrides().contains("workbench_audio"));
    TEST_CHECK(restored.reload() && restored.settings().browser.workbench.audioVisualizer);
  }

  // The loop switch of every media file: off at first, read from the file, checked, saved when the button changes it.
  void loopDefaultsOffAndRoundTrips() {
    Fixture fixture;
    fixture.write("");
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload());
    TEST_CHECK(!service.settings().browser.workbench.mediaLoop);

    fixture.write("[browser]\nworkbench_media_loop = true\n");
    TEST_CHECK(luciole::validateConfigFile(fixture.paths().base).empty());
    TEST_CHECK(service.reload());
    TEST_CHECK(service.settings().browser.workbench.mediaLoop);

    fixture.write("[browser]\nworkbench_media_loop = 'on'\n");
    const auto issues = luciole::validateConfigFile(fixture.paths().base);
    TEST_CHECK(issues.size() == 1);
    TEST_CHECK(issues.front().key == "browser.workbench_media_loop");
    TEST_CHECK(issues.front().message == "expected a boolean");

    fixture.write("");
    TEST_CHECK(service.reload());
    auto chosen = service.settings().browser.workbench;
    const auto edits = luciole::chooseMediaLoop(chosen, true);
    TEST_CHECK(chosen.mediaLoop);
    TEST_CHECK(edits.size() == 1 && edits.front().value(P::WorkbenchMediaLoop) == std::optional(true));
    TEST_CHECK(service.saveOverrides(edits) == State::Durable);
    TEST_CHECK(fixture.overrides().contains("workbench_media_loop = true"));
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload() && restored.settings().browser.workbench.mediaLoop);
    // The same value again edits nothing; the audio visualizer's choice is not touched.
    auto again = restored.settings().browser.workbench;
    TEST_CHECK(luciole::chooseMediaLoop(again, true).empty());
    TEST_CHECK(again.audioVisualizer && again.audioVisual == AudioVisualKind::CosmicPeanut);
    TEST_CHECK(restored.saveOverrides(luciole::inheritAllPreferences()) == State::Durable);
    TEST_CHECK(!fixture.overrides().contains("workbench_media_loop"));
    TEST_CHECK(restored.reload() && !restored.settings().browser.workbench.mediaLoop);
  }
} // namespace

int main() {
  defaultsAreOnWithCosmicPeanut();
  readsEveryWord();
  reportsBadValues();
  savesAndRestores();
  loopDefaultsOffAndRoundTrips();
  std::println("audio-visual-preferences: all checks passed");
  return 0;
}

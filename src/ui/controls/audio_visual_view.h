#pragma once

#include "model/audio_visual_kind.h"
#include "render/scene/custom_draw_node.h"
#include "ui/controls/audio_visual.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace luciole {
  class MediaPlayer;
} // namespace luciole
class Button;
class Flex;
class MediaBarKeys;
class Glyph;

// The sound visualizer's two preferences: whether it shows, and which visual.
struct AudioVisualChoice {
  bool on = true;
  luciole::AudioVisualKind kind = luciole::AudioVisualKind::CosmicPeanut;
  bool operator==(const AudioVisualChoice&) const = default;
};

// The visual of `kind`; null for a value outside the enum.
[[nodiscard]] std::unique_ptr<AudioVisual> makeAudioVisual(luciole::AudioVisualKind kind);

// The kind `steps` places after `kind` in kAudioVisualKinds, wrapping at both ends; a kind outside the enum counts
// as the first.
luciole::AudioVisualKind steppedAudioVisual(luciole::AudioVisualKind kind, int steps);

// The stage of a sound file in a media preview: a large sound glyph, or its visual. While the file plays, the
// visualizer is on and the view is drawn, each drawn frame copies the player's newest window of sound, analyzes it,
// advances the visual, draws it over the background into the view's box and asks for the next frame. Paused after
// playing, it draws the same picture again and asks for nothing. Before the first Play, after the end, with the
// visualizer off or without a player, it is inactive: no tap, no visual, no GL objects and no timer. The two
// control-bar buttons it adds turn the visualizer on and off and step through the visuals; both report the new choice
// to the owner, which saves it.
class AudioVisualView final : public CustomDrawNode {
public:
  // The tap's window: the analysis takes kAudioWaveformSamples at kAudioWaveformRate.
  static constexpr int kTapRate = luciole::workbench::kAudioWaveformRate;
  static constexpr int kTapLength = static_cast<int>(luciole::workbench::kAudioWaveformSamples);

  AudioVisualView();
  ~AudioVisualView() override;
  AudioVisualView(const AudioVisualView&) = delete;
  AudioVisualView& operator=(const AudioVisualView&) = delete;
  AudioVisualView(AudioVisualView&&) = delete;
  AudioVisualView& operator=(AudioVisualView&&) = delete;

  // Adds the visualizer and visual buttons to the end of `bar`, under `keys`; once, before the view is used.
  void addButtons(Flex& bar, MediaBarKeys& keys);
  // The owner's player, or null once it is gone, and whether the file has sound only. The owner calls it after every
  // change of either and of the player's state. A different player counts as another file.
  void follow(luciole::MediaPlayer* player, bool soundOnly);
  // Shows the sound glyph in the middle of the box while the visual is not active; the owner shows it for a sound
  // file with no cover and no error.
  void showGlyph(bool shown);
  // Applies `choice` at once: a playing file switches its visual or turns the visualizer on or off.
  void setChoice(const AudioVisualChoice& choice);
  // Reads the keyboard modifiers held when the visual button is clicked: with Shift it steps back.
  void setModifiers(std::function<std::uint32_t()> modifiers);

  // Runs after a button changed the choice, which is already applied. It must not destroy the view.
  std::function<void(const AudioVisualChoice&)> onChosen;
  // Runs when active() changed, so the owner shows or hides what lies under the stage.
  std::function<void()> onActiveChanged;

  const AudioVisualChoice& choice() const noexcept { return m_choice; }
  // Whether the visual takes the stage: a sound file playing, or paused after playing, with the visualizer on.
  bool active() const noexcept { return m_active; }
  bool showsGlyph() const noexcept;
  // Whether the view holds a visual, its analysis and the tap.
  bool running() const noexcept { return m_run != nullptr; }
  Button& visualizerButton() const noexcept { return *m_toggle; }
  Button& visualButton() const noexcept { return *m_next; }

  void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

protected:
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;

private:
  struct Run;

  void refresh();
  void refreshButtons();
  void refreshGlyph();
  void choose(const AudioVisualChoice& choice);
  void startRun();
  void stopRun();
  void releaseRun();
  void stopTap();
  void step() const;

  AudioVisualChoice m_choice;
  luciole::MediaPlayer* m_player = nullptr;
  luciole::MediaPlayer* m_tapped = nullptr; // the player whose tap this view started
  bool m_soundOnly = false;
  bool m_heardPlaying = false; // the player played since it was made or last ended
  bool m_active = false;
  bool m_glyphWanted = false;
  std::unique_ptr<Run> m_run;
  std::function<std::uint32_t()> m_modifiers;
  Glyph* m_glyph = nullptr;
  Button* m_toggle = nullptr;
  Button* m_next = nullptr;
  Renderer* m_renderer = nullptr;
};

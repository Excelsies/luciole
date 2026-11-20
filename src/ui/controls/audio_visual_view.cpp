#include "ui/controls/audio_visual_view.h"

#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "media/media_player.h"
#include "render/backend/render_backend.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/media_bar_keys.h"
#include "ui/palette.h"
#include "workbench/audio_analysis.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
  using luciole::MediaState;
  using Clock = std::chrono::steady_clock;

  const Logger kLog("audio-visual");

  // The fill of the column preview the view sits in, which the box shows under the visual.
  constexpr ColorRole kBackground = ColorRole::SurfaceContainerLow;
  // The sound glyph's share of the box's shorter side, and its smallest and largest size.
  constexpr float kGlyphShare = 0.4F;
  constexpr float kGlyphMin = 24;
  constexpr float kGlyphMax = 160;
  // A position this far behind the last one, or this far ahead of the time that passed, is a seek.
  constexpr double kSeekSlack = 0.05;
  constexpr double kSeekJump = 0.5;

  void showIn(Node& node, bool shown) {
    node.setVisible(shown);
    node.setParticipatesInLayout(shown);
  }

  std::unique_ptr<Button> barButton(std::function<void()> onClick) {
    auto button = std::make_unique<Button>();
    button->setVariant(ButtonVariant::Ghost);
    button->setOnClick(std::move(onClick));
    return button;
  }

  AudioVisualPalette themePalette() {
    return {
        .accents =
            {colorForRole(ColorRole::Primary), colorForRole(ColorRole::Secondary), colorForRole(ColorRole::Tertiary)},
        .background = colorForRole(kBackground),
        .text = colorForRole(ColorRole::OnSurface),
    };
  }

  // The node's box in buffer pixels, in GL window coordinates; empty when anything in it is not finite.
  AudioVisualRect boxPixels(const CustomDrawContext& context) {
    const Vec2 a = context.transform.transformPoint(0, 0);
    const Vec2 b = context.transform.transformPoint(context.width, context.height);
    const std::array values{a.x, a.y, b.x, b.y, context.pixelScaleX, context.pixelScaleY, context.surfaceHeight};
    if (!std::ranges::all_of(values, [](float v) { return std::isfinite(v); })) {
      return {};
    }
    const auto pixel = [](float logical, float scale) { return static_cast<int>(std::lround(logical * scale)); };
    const int left = pixel(std::min(a.x, b.x), context.pixelScaleX);
    const int right = pixel(std::max(a.x, b.x), context.pixelScaleX);
    const int top = pixel(std::min(a.y, b.y), context.pixelScaleY);
    const int bottom = pixel(std::max(a.y, b.y), context.pixelScaleY);
    return {
        .x = left,
        .y = pixel(context.surfaceHeight, context.pixelScaleY) - bottom,
        .width = right - left,
        .height = bottom - top,
    };
  }
} // namespace

// What the view holds while it is active.
struct AudioVisualView::Run {
  std::unique_ptr<AudioVisual> visual;
  luciole::workbench::AudioAnalysis analysis;
  std::vector<float> samples = std::vector<float>(luciole::workbench::kAudioWaveformSamples);
  std::optional<Clock::time_point> advanced; // when the visual last advanced, while playing
  std::optional<double> position;            // the player's position at the last advance, to notice a seek
  bool primed = false;                       // the visual advanced at least once, so a pause shows a picture
  Timer nextFrame;
  std::function<void()> requestFrame; // marks the view dirty; run by nextFrame
  bool drawn = false;                 // the visual holds GL objects
  bool failed = false;                // the visual could not draw; it stays empty
};

AudioVisualView::AudioVisualView() : m_glyph(static_cast<Glyph*>(addChild(std::make_unique<Glyph>()))) {
  setHitTestVisible(false);
  m_glyph->setGlyph("music");
  m_glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_glyph->setVisible(false);
}

AudioVisualView::~AudioVisualView() { releaseRun(); }

void AudioVisualView::addButtons(Flex& bar, MediaBarKeys& keys) {
  const auto toggle = [this] { choose({.on = !m_choice.on, .kind = m_choice.kind}); };
  const auto next = [this] {
    const bool back = m_modifiers && (m_modifiers() & KeyMod::Shift) != 0;
    choose({.on = m_choice.on, .kind = steppedAudioVisual(m_choice.kind, back ? -1 : 1)});
  };
  m_toggle = static_cast<Button*>(bar.addChild(barButton(toggle)));
  m_next = static_cast<Button*>(bar.addChild(barButton(next)));
  keys.adopt(*m_toggle, toggle);
  keys.adopt(*m_next, next);
  m_next->setGlyph("sparkles");
  refreshButtons();
}

void AudioVisualView::follow(luciole::MediaPlayer* player, bool soundOnly) {
  if (player != m_player) {
    if (m_tapped != player) {
      m_tapped = nullptr; // the player that had the tap is gone
    }
    m_heardPlaying = false;
    stopRun();
  }
  m_player = player;
  if (soundOnly != m_soundOnly) {
    m_soundOnly = soundOnly;
    refreshButtons();
  }
  refresh();
}

void AudioVisualView::setChoice(const AudioVisualChoice& choice) {
  if (choice == m_choice) {
    return;
  }
  const bool swap = m_run && choice.on && choice.kind != m_choice.kind;
  m_choice = choice;
  refreshButtons();
  if (swap) {
    stopRun();
  }
  refresh();
}

void AudioVisualView::showGlyph(bool shown) {
  m_glyphWanted = shown;
  refreshGlyph();
}

bool AudioVisualView::showsGlyph() const noexcept { return m_glyph->visible(); }

void AudioVisualView::refreshGlyph() {
  const bool shown = m_glyphWanted && !m_active;
  if (shown != m_glyph->visible()) {
    m_glyph->setVisible(shown);
    markLayoutDirty();
  }
}

void AudioVisualView::setModifiers(std::function<std::uint32_t()> modifiers) { m_modifiers = std::move(modifiers); }

void AudioVisualView::choose(const AudioVisualChoice& choice) {
  setChoice(choice);
  if (onChosen) {
    onChosen(m_choice);
  }
}

// Brings the tap and the visual in line with the player's state and the choice.
void AudioVisualView::refresh() {
  const auto state = m_player != nullptr ? m_player->state() : MediaState::Idle;
  if (state == MediaState::Playing) {
    m_heardPlaying = true;
  } else if (state != MediaState::Paused) {
    m_heardPlaying = false;
  }
  const bool open = state == MediaState::Loading || state == MediaState::Paused || state == MediaState::Playing;
  const bool tap = m_choice.on && m_soundOnly && open;
  if (!tap) {
    stopTap();
  } else if (m_tapped != m_player) {
    m_tapped = m_player->startAudioTap(kTapRate, kTapLength) ? m_player : nullptr;
  }
  const bool was = std::exchange(m_active, tap && m_heardPlaying);
  if (m_active && !m_run) {
    startRun();
  } else if (!m_active) {
    stopRun();
  }
  if (m_run && state == MediaState::Playing) {
    markPaintDirty();
  } else if (m_run) {
    m_run->advanced.reset();
    m_run->nextFrame.stop();
  }
  if (m_active != was) {
    refreshGlyph();
    if (onActiveChanged) {
      onActiveChanged();
    }
  }
}

void AudioVisualView::refreshButtons() {
  if (m_toggle == nullptr) {
    return;
  }
  showIn(*m_toggle, m_soundOnly);
  showIn(*m_next, m_soundOnly);
  m_toggle->setGlyph(m_choice.on ? "ripple" : "ripple-off");
  m_toggle->setTooltip(m_choice.on ? "Turn off the audio visualizer" : "Turn on the audio visualizer");
  m_next->setEnabled(m_choice.on);
  m_next->setTooltip(
      std::format("Visual: {} (click for the next, Shift-click for the previous)", audioVisualLabel(m_choice.kind))
  );
}

void AudioVisualView::startRun() {
  m_run = std::make_unique<Run>();
  m_run->requestFrame = [this] { markPaintDirty(); };
  m_run->visual = makeAudioVisual(m_choice.kind);
  markPaintDirty();
}

void AudioVisualView::stopRun() {
  if (m_run) {
    releaseRun();
    markPaintDirty();
  }
}

// Frees the visual's GL objects with the context current, then everything the run holds.
void AudioVisualView::releaseRun() {
  if (m_run && m_run->drawn && m_renderer != nullptr) {
    (void)m_renderer->textureManager(); // makes the context current
    m_run->visual->release();
  }
  m_run.reset();
}

void AudioVisualView::stopTap() {
  if (m_tapped != nullptr && m_tapped == m_player) {
    m_tapped->stopAudioTap();
  }
  m_tapped = nullptr;
}

// The analysis of the newest window heard, and the visual's advance by it.
void AudioVisualView::step() const {
  auto& run = *m_run;
  const bool playing = m_player->state() == MediaState::Playing;
  const auto now = Clock::now();
  const double seconds = run.advanced ? std::chrono::duration<double>(now - *run.advanced).count() : 0.0;
  run.advanced = playing ? std::optional(now) : std::nullopt;
  const double position = m_player->position();
  // A position that went back, or ahead of the time that passed, is a seek: the analysis starts afresh.
  if (run.position && (position < *run.position - kSeekSlack || position > *run.position + seconds + kSeekJump)) {
    run.analysis.reset(position);
  }
  run.position = position;
  run.primed = true;
  const auto window = m_player->copyAudioWindow(run.samples);
  auto frame = run.analysis.analyze(std::span(run.samples).first(static_cast<std::size_t>(window.count)), seconds);
  frame.playedSeconds = position;
  run.visual->advance(frame);
}

void AudioVisualView::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  if (!m_run || !m_run->visual || m_run->failed || m_player == nullptr) {
    return;
  }
  const AudioVisualRect rect = boxPixels(context);
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }
  const AudioVisualPalette palette = themePalette();
  RoundedRectStyle background;
  background.fill = palette.background;
  background.fill.a *= context.opacity;
  backend.drawRect(
      context.surfaceWidth, context.surfaceHeight, context.width, context.height, background, context.transform
  );
  backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
  const bool playing = m_player->state() == MediaState::Playing;
  if (playing || !m_run->primed) {
    step(); // a visual started while paused advances once, on the window the tap holds
  }
  try {
    m_run->drawn = true;
    m_run->visual->draw({.node = context, .rect = rect, .scale = context.pixelScaleX, .palette = palette});
  } catch (const std::runtime_error& error) {
    kLog.error("the {} visual cannot draw: {}", audioVisualName(m_choice.kind), error.what());
    m_run->failed = true;
    return;
  }
  if (playing) {
    m_run->nextFrame.start(std::chrono::milliseconds{0}, m_run->requestFrame);
  }
}

void AudioVisualView::doLayout(Renderer& renderer) {
  m_renderer = &renderer;
  if (m_glyph->visible()) {
    m_glyph->setGlyphSize(std::clamp(std::min(width(), height()) * kGlyphShare, kGlyphMin, kGlyphMax));
    m_glyph->measure(renderer);
    m_glyph->setPosition(std::round((width() - m_glyph->width()) / 2), std::round((height() - m_glyph->height()) / 2));
  }
}

void AudioVisualView::doInvalidateGpuResources(Renderer& /*renderer*/) {
  if (m_run && m_run->visual) {
    m_run->visual->abandon();
    m_run->drawn = false;
    m_run->failed = false;
  }
}

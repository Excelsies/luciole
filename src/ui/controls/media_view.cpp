#include "ui/controls/media_view.h"

#include "core/input/key_symbols.h"
#include "render/core/renderer.h"
#include "render/scene/image_node.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/media_position_bar.h"
#include "ui/controls/preview_frame.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>

namespace {
  using luciole::MediaState;
  using luciole::SeekMode;

  // The least width the position bar keeps beside the time labels.
  constexpr float kMinBarWidth = 24;
  constexpr std::string_view kNoSoundDevice = "No sound device was found.";
  constexpr std::string_view kSeeking = "Seeking...";
  constexpr std::string_view kReading = "Still reading the file...";
  constexpr std::string_view kUnread = "Could not read the details of this file.";

  // 0:05, 12:34 or 1:02:03; "--:--" while unknown.
  std::string clockText(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) {
      return "--:--";
    }
    const auto whole = static_cast<long long>(seconds);
    if (whole >= 3600) {
      return std::format("{}:{:02}:{:02}", whole / 3600, whole / 60 % 60, whole % 60);
    }
    return std::format("{}:{:02}", whole / 60, whole % 60);
  }

  std::unique_ptr<Button> controlButton(std::string_view glyph, std::function<void()> onClick) {
    auto button = std::make_unique<Button>();
    button->setVariant(ButtonVariant::Ghost);
    button->setGlyph(glyph);
    button->setOnClick(std::move(onClick));
    return button;
  }

  std::unique_ptr<Label> timeLabel() {
    auto label = std::make_unique<Label>();
    label->setMaxLines(1);
    label->setFontFeatures("tnum");
    label->setText(clockText(0));
    return label;
  }

  void showIn(Node& node, bool shown) {
    node.setVisible(shown);
    node.setParticipatesInLayout(shown);
  }
} // namespace

MediaView::MediaView(luciole::AudioOutput audio)
    : m_audio(audio), m_barKeys([this](const InputArea::KeyData& key) { onKey(key); }),
      m_textSize(Style::fontSizeCaption) {
  m_frame = static_cast<luciole::PreviewFrame*>(addChild(std::make_unique<luciole::PreviewFrame>()));
  m_picture = &m_frame->image();
  m_visual = static_cast<AudioVisualView*>(addChild(std::make_unique<AudioVisualView>()));
  m_visual->onActiveChanged = [this] { refreshControls(); };
  m_notice = static_cast<Label*>(addChild(std::make_unique<Label>()));
  m_notice->setTextAlign(TextAlign::Center);
  m_notice->setColor(colorSpecFromRole(ColorRole::Error));
  m_stageArea = static_cast<InputArea*>(addChild(std::make_unique<InputArea>()));
  m_stageArea->setFocusable(true);
  m_stageArea->setRetainsFocusOnPointerRelease(true);
  m_stageArea->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      togglePlay();
    }
  });
  m_stageArea->setOnKeyDown([this](const InputArea::KeyData& key) { onKey(key); });

  auto controls = std::make_unique<Flex>();
  controls->setDirection(FlexDirection::Horizontal);
  controls->setAlign(FlexAlign::Center);
  controls->setGap(Style::spaceXs);
  controls->setPadding(0, Style::spaceXs);
  const auto addButton = [&](std::string_view glyph, std::function<void()> action) {
    auto* button = static_cast<Button*>(controls->addChild(controlButton(glyph, action)));
    m_barKeys.adopt(*button, std::move(action));
    return button;
  };
  m_play = addButton("media-play", [this] { togglePlay(); });
  m_elapsed = static_cast<Label*>(controls->addChild(timeLabel()));
  auto bar = std::make_unique<MediaPositionBar>();
  bar->setFlexGrow(1);
  bar->onSeek = [this](double fraction, SeekMode mode, bool done) { scrub(fraction, mode, done); };
  m_bar = static_cast<MediaPositionBar*>(controls->addChild(std::move(bar)));
  m_barKeys.adopt(m_bar->inputArea());
  m_total = static_cast<Label*>(controls->addChild(timeLabel()));
  m_message = static_cast<Label*>(controls->addChild(std::make_unique<Label>()));
  m_message->setMaxLines(1);
  m_visual->addButtons(*controls, m_barKeys);
  m_loopButton = addButton("repeat-off", [this] {
    setLoop(!m_looping);
    if (onLoopChosen) {
      onLoopChosen(m_looping);
    }
  });
  m_mute = addButton("volume-high", [this] {
    m_muted = !m_muted;
    if (m_player) {
      m_player->setMuted(m_muted);
    }
    refreshControls();
  });
  m_controls = static_cast<Flex*>(addChild(std::move(controls)));
  showIn(*m_controls, luciole::MediaPlayer::available());
  m_paletteConn = paletteChanged().connect([this] { markLayoutDirty(); });
  setTextSize(m_textSize);
  refreshControls();
}

// The timers stop with their members.
MediaView::~MediaView() {
  m_player.reset();
  releaseFrame();
  releasePoster();
}

void MediaView::setSource(const std::string& path) {
  if (path == m_path && !m_factsFailed) {
    return;
  }
  stop();
  m_probe.cancel();
  m_path = path;
  m_facts = {};
  m_factsRead = false;
  m_factsFailed = false;
  m_factsRetried = false;
  setPoster(nullptr);
  readFacts();
  refreshControls();
}

void MediaView::setShown(bool shown) {
  if (shown == m_shown) {
    return;
  }
  m_shown = shown;
  if (!shown) {
    stop();
    releasePoster();
    m_probe.cancel();
    m_catchUp.stop();
    return;
  }
  m_posterPending = m_poster != nullptr;
  readFacts();
  scheduleCatchUp();
}

void MediaView::setTextSize(float size) {
  if (!std::isfinite(size) || size <= 0) {
    return;
  }
  m_textSize = size;
  const float row = std::round(std::max(size * 1.9F, 20.0F));
  for (Label* label : {m_elapsed, m_total, m_message, m_notice}) {
    label->setFontSize(size);
  }
  for (Button* button : {m_play, m_loopButton, m_mute, &m_visual->visualizerButton(), &m_visual->visualButton()}) {
    button->setGlyphSize(size + 2);
    button->setControlHeight(row);
    button->setMinWidth(row);
    button->setPadding((row - size - 2) / 2);
  }
  m_controls->setMinHeight(row);
  m_controls->setMaxHeight(row);
  m_bar->setRowHeight(row);
  markLayoutDirty();
}

void MediaView::setLoop(bool loop) {
  m_looping = loop;
  if (m_player) {
    m_player->setLoop(loop);
  }
  refreshControls();
}

void MediaView::doLayout(Renderer& renderer) {
  m_renderer = &renderer;
  const float w = width();
  const float controls = m_controls->participatesInLayout() ? m_controls->maxHeight() : 0;
  const float stage = std::max(0.0F, height() - controls);
  const auto [boxWidth, boxHeight] = placePicture(renderer, w, stage);
  m_stageArea->setPosition(0, 0);
  m_stageArea->setFrameSize(w, stage);
  m_visual->arrange(renderer, {0, 0, w, stage});
  if (m_notice->visible()) {
    m_notice->setMaxWidth(std::max(0.0F, w - 2 * Style::spaceLg));
    m_notice->measure(renderer);
    m_notice->setPosition(std::round((w - m_notice->width()) / 2), std::round((stage - m_notice->height()) / 2));
  }
  if (controls > 0) {
    fitTimes(renderer, w);
    m_controls->arrange(renderer, {0, stage, w, controls});
    m_barKeys.fit();
  }
  const float scale = renderer.renderScale();
  m_stagePixels = {static_cast<int>(std::lround(boxWidth * scale)), static_cast<int>(std::lround(boxHeight * scale))};
  if (m_posterPending || (m_player && m_stagePixels != m_askedPixels)) {
    scheduleCatchUp();
  }
}

void MediaView::togglePlay() {
  if (!luciole::MediaPlayer::available() || m_path.empty() || !m_shown) {
    return;
  }
  if (!m_player || m_player->state() == MediaState::Error) {
    start();
  } else {
    m_playRequested = !m_playRequested;
    m_playRequested ? m_player->play() : m_player->pause();
  }
  refreshControls();
  followClock();
}

void MediaView::onKey(const InputArea::KeyData& key) {
  if (!key.pressed || key.modifiers != 0) {
    return;
  }
  if (KeySymbol::isSpace(key.sym)) {
    togglePlay();
  } else if (KeySymbol::isLeft(key.sym)) {
    seekBy(-kSeekStep);
  } else if (KeySymbol::isRight(key.sym)) {
    seekBy(kSeekStep);
  }
}

void MediaView::stop() {
  m_player.reset();
  m_parked.reset();
  m_awaiting.reset();
  m_idle.stop();
  m_clock.stop();
  clearSeek();
  m_playRequested = false;
  m_dragging = false;
  releaseFrame();
  showPoster();
  refreshControls();
}

void MediaView::seekBy(double seconds) {
  if (!wake()) {
    return;
  }
  const auto state = m_player->state();
  if (state == MediaState::Error || (state == MediaState::Loading && !m_parked)) {
    return;
  }
  // From where the player stands, which while a parked file opens is the place it was asked for last.
  double target = m_player->position() + seconds;
  if (const double length = knownDuration(); length > 0) {
    target = std::min(target, length);
  }
  target = std::max(target, 0.0);
  m_player->seek(target);
  if (m_awaiting) {
    m_awaiting = target; // the place a reopened file goes on from is where the latest seek asked
  }
  watchSeek();
  refreshPosition();
  watchIdle(); // the seek is use: it drops the old deadline, and its settling starts a new one
}

void MediaView::scrub(double fraction, SeekMode mode, bool done) {
  if (m_parked) {
    (void)wake();
  }
  const double duration = knownDuration();
  if (!m_player || duration <= 0) {
    return;
  }
  m_dragging = !done;
  m_dragSeconds = std::clamp(fraction, 0.0, 1.0) * duration;
  if (m_player->state() != MediaState::Loading) {
    m_awaiting.reset(); // a seek on a file that has loaded flushes what came before it
  } else if (m_awaiting) {
    m_awaiting = m_dragSeconds;
  }
  m_player->seek(m_dragSeconds, mode);
  watchSeek();
  refreshPosition();
  watchIdle();
}

// The player's state, duration or error changed; the player is never destroyed from here.
void MediaView::playerChanged() {
  const auto state = m_player->state();
  if (m_parked && state != MediaState::Loading && !m_player->seeking()) {
    m_parked.reset();
  }
  if (m_awaiting) {
    scheduleCatchUp(); // the frame at the place may be waiting, and the wait ends once the player has settled
  }
  if (state == MediaState::Ended || state == MediaState::Error) {
    m_playRequested = false;
  }
  if (state == MediaState::Error) {
    releaseFrame();
    showPoster();
  }
  if (const double duration = m_player->duration(); duration > 0 && m_facts.seconds <= 0) {
    m_facts.seconds = duration;
    if (onFactsChanged) {
      onFactsChanged();
    }
  }
  refreshControls();
  followClock();
}

void MediaView::refreshControls() {
  const bool failed = m_player && m_player->state() == MediaState::Error;
  const bool soundFailed = failed && soundOnly();
  m_timesWanted = !failed || soundFailed;
  showIn(*m_bar, m_timesWanted);
  refreshMessage(failed && !soundFailed);
  refreshNotice(soundFailed);
  m_play->setGlyph(m_playRequested ? "media-pause" : "media-play");
  m_play->setTooltip(m_playRequested ? "Pause" : "Play");
  m_loopButton->setGlyph(m_looping ? "repeat" : "repeat-off");
  m_loopButton->setTooltip(m_looping ? "Turn off looping" : "Turn on looping");
  m_mute->setGlyph(m_muted ? "volume-mute" : "volume-high");
  m_mute->setTooltip(m_muted ? "Unmute" : "Mute");
  const double duration = m_player && m_player->duration() > 0 ? m_player->duration() : m_facts.seconds;
  const auto total = duration > 0 ? clockText(duration) : clockText(-1);
  if (m_total->text() != total) {
    m_total->setText(total);
  }
  m_bar->setEnabled(knownDuration() > 0);
  m_choiceWanted = soundOnly() && !failed;
  m_visual->follow(m_player.get(), m_choiceWanted);
  showTimes();
  m_visual->showGlyph(soundOnly() && !failed && !m_showingFrame && !m_posterTexture.valid());
  m_frame->setVisible(!m_visual->active() && !soundFailed); // the visual and the notice cover the stage
  refreshPosition();
}

void MediaView::showTimes() {
  showIn(*m_elapsed, m_timesWanted && m_timesFit);
  showIn(*m_total, m_timesWanted && m_timesFit);
  showIn(m_visual->visualButton(), m_choiceWanted && m_choiceFit);
}

// A row too narrow for its parts gives up the elapsed and total time, both together, and then the visual button, each
// only while the parts left plus kMinBarWidth do not fit. When they still do not fit with both gone, as at large text
// sizes, the position bar gets what is left, less than kMinBarWidth. Each choice is measured with the others set,
// never from the last result.
void MediaView::fitTimes(Renderer& renderer, float width) {
  const auto fits = [&](bool times, bool choice) {
    m_timesFit = times;
    m_choiceFit = choice;
    showTimes();
    return m_controls->measure(renderer, LayoutConstraints::unconstrained()).width + kMinBarWidth <= width;
  };
  if (!fits(true, true) && !fits(false, true)) {
    m_choiceFit = false;
  }
  showTimes();
}

// The reason a sound file failed sits where its glyph was.
void MediaView::refreshNotice(bool shown) {
  if (m_notice->setText(shown ? m_player->error() : std::string{}) || m_notice->visible() != shown) {
    m_notice->setVisible(shown);
    markLayoutDirty();
  }
}

// An error replaces the time; the note that no sound device was found sits beside it, said once per playback.
void MediaView::refreshMessage(bool failed) {
  const bool silent = m_player && m_player->state() != MediaState::Error && m_player->soundDeviceMissing();
  const bool seeking = m_seekShown && !failed;
  const bool reading = m_factsRetried && !m_factsRead && !failed;
  const bool unread = m_factsFailed && !failed;
  const auto text = failed ? m_player->error()
      : unread             ? std::string(kUnread)
      : reading            ? std::string(kReading)
      : seeking            ? std::string(kSeeking)
      : silent             ? std::string(kNoSoundDevice)
                           : std::string{};
  if (m_message->text() != text) {
    m_message->setText(text);
    m_message->setColor(colorSpecFromRole(failed ? ColorRole::Error : ColorRole::OnSurfaceVariant));
    m_message->setFlexGrow(failed ? 1 : 0);
  }
  showIn(*m_message, failed || silent || seeking || reading || unread);
  showIn(*m_mute, !silent);
}

void MediaView::refreshPosition() {
  const double duration = knownDuration();
  const double at = !m_player ? (m_parked ? m_parked->at : 0) : (m_dragging ? m_dragSeconds : m_player->position());
  m_bar->setFraction(duration > 0 ? at / duration : 0);
  const auto second = static_cast<long long>(std::max(at, 0.0));
  if (second != m_shownSecond) {
    m_shownSecond = second;
    m_elapsed->setText(clockText(static_cast<double>(second)));
  }
}

// The clock runs only while the player plays.
void MediaView::followClock() {
  if (!m_player || m_player->state() != MediaState::Playing) {
    m_clock.stop();
  } else if (!m_clock.active()) {
    m_clock.startRepeating(kClockInterval, [this] { onScreen() ? refreshPosition() : stop(); });
  }
  watchIdle();
}

void MediaView::scheduleCatchUp() {
  if (m_renderer != nullptr && m_shown && !m_catchUp.active()) {
    m_catchUp.start(std::chrono::milliseconds{0}, [this] { catchUp(); });
  }
}

// Uploads a waiting poster, passes a new stage size to the player and shows its newest frame. A view whose
// ancestor was hidden stops instead.
void MediaView::catchUp() {
  if (m_player && !onScreen()) {
    stop();
    return;
  }
  if (m_posterPending) {
    uploadPoster();
  }
  if (!m_player) {
    return;
  }
  if (m_stagePixels != m_askedPixels) {
    m_askedPixels = m_stagePixels;
    m_player->setFrameSize(m_stagePixels.first, m_stagePixels.second);
  }
  if (const auto frame = m_player->takeFrame()) {
    // A parked file opening again keeps its frame until one from the place it goes on from arrives; a frame from
    // before that place is dropped. A frame with no stream time is shown.
    if (!m_awaiting || frame->time < 0 || frame->time >= *m_awaiting - kFrameSlack) {
      m_awaiting.reset();
      const bool first = !m_showingFrame;
      showFrame(*frame);
      if (first) {
        refreshControls();
      }
    }
  }
  if (m_awaiting && m_player->state() != MediaState::Loading && !m_player->seeking()) {
    m_awaiting.reset(); // settled with no frame from the place: the kept one stands
  }
}

bool MediaView::onScreen() const {
  for (const Node* node = this; node != nullptr; node = node->parent()) {
    if (!node->visible()) {
      return false;
    }
  }
  return true;
}

bool MediaView::soundOnly() const {
  if (m_player && m_player->state() != MediaState::Loading && m_player->state() != MediaState::Error) {
    return !m_player->hasVideo();
  }
  return m_facts.audio && !m_facts.video;
}

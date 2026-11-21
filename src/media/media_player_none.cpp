#include "media/media_player.h"

#include <algorithm>
#include <cmath>

// The player of a build without GStreamer: every file fails to open and the view shows stills.
struct luciole::MediaPlayer::State {
  MediaState state = MediaState::Idle;
  std::string error;
  double volume = 1;
  bool muted = false;
  bool loop = false;
};

luciole::MediaPlayer::MediaPlayer(AudioOutput /*audio*/) : m_state(std::make_unique<State>()) {}

luciole::MediaPlayer::~MediaPlayer() = default;

bool luciole::MediaPlayer::available() noexcept { return false; }

bool luciole::MediaPlayer::stopsPending() noexcept { return false; }

bool luciole::MediaPlayer::open(const std::string& /*path*/) {
  m_state->state = MediaState::Error;
  m_state->error = "Playback is not available in this build.";
  return false;
}

void luciole::MediaPlayer::close() {
  m_state->state = MediaState::Idle;
  m_state->error.clear();
}

void luciole::MediaPlayer::play() {}

void luciole::MediaPlayer::pause() {}

void luciole::MediaPlayer::seek(double /*seconds*/, SeekMode /*mode*/) {}

void luciole::MediaPlayer::setVolume(double volume) {
  if (!std::isnan(volume)) {
    m_state->volume = std::clamp(volume, 0.0, 1.0);
  }
}

void luciole::MediaPlayer::setMuted(bool muted) { m_state->muted = muted; }

void luciole::MediaPlayer::setLoop(bool loop) { m_state->loop = loop; }

void luciole::MediaPlayer::setFrameSize(int /*width*/, int /*height*/) {}

std::optional<luciole::MediaFrame> luciole::MediaPlayer::takeFrame() { return std::nullopt; }

bool luciole::MediaPlayer::startAudioTap(int /*rate*/, int /*length*/) { return false; }

void luciole::MediaPlayer::stopAudioTap() {}

luciole::AudioWindow luciole::MediaPlayer::copyAudioWindow(std::span<float> /*samples*/) { return {}; }

luciole::MediaState luciole::MediaPlayer::state() const noexcept { return m_state->state; }

const std::string& luciole::MediaPlayer::error() const noexcept { return m_state->error; }

double luciole::MediaPlayer::duration() const noexcept { return 0; }

double luciole::MediaPlayer::position() const { return 0; }

double luciole::MediaPlayer::volume() const noexcept { return m_state->volume; }

bool luciole::MediaPlayer::muted() const noexcept { return m_state->muted; }

bool luciole::MediaPlayer::loop() const noexcept { return m_state->loop; }

bool luciole::MediaPlayer::hasVideo() const noexcept { return false; }

bool luciole::MediaPlayer::soundDeviceMissing() const noexcept { return false; }

bool luciole::MediaPlayer::reopensInPlace() const { return true; }

bool luciole::MediaPlayer::seeking() const noexcept { return false; }

// NOLINTBEGIN(performance-unnecessary-value-param): the header's signatures; no callback runs in this build.
void luciole::MediaPlayer::setChangedCallback(std::function<void()> /*callback*/) {}

void luciole::MediaPlayer::setFrameCallback(std::function<void()> /*callback*/) {}
// NOLINTEND(performance-unnecessary-value-param)

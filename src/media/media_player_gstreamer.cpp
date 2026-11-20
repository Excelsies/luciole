#include "media/audio_tap.h"
#include "media/gstreamer_pipeline.h"
#include "media/gstreamer_runtime.h"
#include "media/media_player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>
#include <utility>

namespace {
  using luciole::media::kCannotPlay;

  constexpr std::string_view kNoSoundDevice = "This file cannot be played: no sound device was found.";
  // A position this close to the length of the file counts as its end: about one audio buffer.
  constexpr double kEndTolerance = 0.02;
} // namespace

// The player's state on the main context; the pipeline does the work elsewhere.
struct luciole::MediaPlayer::State {
  using Pipeline = media::Pipeline;

  AudioOutput audio;
  std::shared_ptr<Pipeline> pipeline;
  MediaState state = MediaState::Idle;
  std::string error;
  double duration = 0;
  mutable double lastPosition = 0;
  double volume = 1;
  bool muted = false;
  bool loop = false;
  bool wantPlay = false;
  bool restarting = false; // a restart's seek is in flight; a refusal of it ends the file
  // When the last restart was asked; cleared by a seek or a Play.
  std::optional<std::chrono::steady_clock::time_point> lastRestart;
  bool seeking = false;   // one seek is in flight; a later one waits in pendingSeek, the latest replacing the earlier
  bool refitting = false; // the seek in flight redraws the shown frame and moves nothing
  bool endMissed = false; // an end of stream came during a seek and was held until it settles or is refused
  bool video = false;
  bool soundMissing = false;
  int boxWidth = 0;
  int boxHeight = 0;
  // The audio tap's rate and length, 0 while it is off, and its samples for the open pipeline.
  int tapRate = 0;
  int tapLength = 0;
  std::shared_ptr<media::AudioTap> tap;
  std::optional<std::pair<double, SeekMode>> pendingSeek;
  std::function<void()> changed;
  std::function<void()> frame;

  explicit State(AudioOutput output) : audio(output) {}

  void request(const std::function<void(Pipeline&)>& change) const {
    if (pipeline) {
      media::request(pipeline, change);
    }
  }

  // Every seek sent ends in a done message or a refusal; one that ended in neither would block all later seeks.
  void requestSeek(double seconds, SeekMode mode) {
    lastPosition = seconds;
    if (seeking) {
      pendingSeek = std::pair{seconds, mode};
      return;
    }
    seeking = true;
    request([seconds, mode](Pipeline& p) { p.seek = std::pair{seconds, mode}; });
  }

  void listen() {
    auto& listener = pipeline->listener;
    listener.message = [this](GstMessage* message) { handle(message); };
    listener.frame = [this] {
      if (frame) {
        frame();
      }
    };
    listener.failure = [this](std::string text) { failWith(std::move(text)); };
    listener.seekFailed = [this] { seekRefused(); };
    listener.refit = [this] { refitDue(); };
  }

  void closePipeline() {
    if (pipeline) {
      media::stop(std::exchange(pipeline, nullptr));
    }
    duration = 0;
    lastPosition = 0;
    wantPlay = false;
    seeking = false;
    restarting = false;
    endMissed = false;
    lastRestart.reset();
    refitting = false;
    video = false;
    soundMissing = false;
    pendingSeek.reset();
    tap.reset();
  }

  // Where the pipeline stands, in seconds; empty while it does not know.
  std::optional<double> queryPosition() const {
    GstElement* element = media::elementRef(*pipeline);
    if (element == nullptr) {
      return std::nullopt;
    }
    gint64 at = 0;
    const bool known = gst_element_query_position(element, GST_FORMAT_TIME, &at) != FALSE && at >= 0;
    gst_object_unref(element);
    return known ? std::optional(static_cast<double>(at) / static_cast<double>(GST_SECOND)) : std::nullopt;
  }

  // The pipeline stands at the end of a file whose length is known.
  bool atEnd() const {
    const auto at = queryPosition();
    return duration > 0 && at && duration - *at < kEndTolerance;
  }

  void notify() const {
    if (changed) {
      changed();
    }
  }

  void failWith(std::string text) {
    closePipeline();
    state = MediaState::Error;
    error = std::move(text);
    notify();
  }

  void refreshDuration() {
    GstElement* element = media::elementRef(*pipeline);
    if (element == nullptr) {
      return;
    }
    gint64 length = 0;
    if (gst_element_query_duration(element, GST_FORMAT_TIME, &length) != FALSE && length > 0) {
      duration = static_cast<double>(length) / static_cast<double>(GST_SECOND);
    }
    gst_object_unref(element);
  }

  // The file reached its end for good: paused at the end until Play.
  void end() {
    state = MediaState::Ended;
    wantPlay = false;
    restarting = false;
    request([](Pipeline& p) { p.target = GST_STATE_PAUSED; });
    notify();
  }

  // With loop on, a file that is playing starts again from the beginning. False when it must end instead: a length
  // that is not known, or an end that came again less than kMinLoopPass after the last restart, with no seek or Play
  // between, so a file that cannot be played forward does not restart without end.
  bool restart() {
    const auto now = std::chrono::steady_clock::now();
    if (!loop || !wantPlay || duration <= 0 || (lastRestart && now - *lastRestart < kMinLoopPass)) {
      return false;
    }
    lastRestart = now;
    restarting = true;
    requestSeek(0, SeekMode::Exact);
    return true;
  }

  // The file reached its end: it starts again when loop allows, otherwise it ends.
  void reachEnd() {
    if (!restart()) {
      end();
    }
  }

  // The stream time of the newest frame, in seconds; empty before the first.
  std::optional<double> frameTime() const {
    const auto at = pipeline->frameTime.load();
    return at >= 0 ? std::optional(static_cast<double>(at) / static_cast<double>(GST_SECOND)) : std::nullopt;
  }

  // Decodes the shown frame again at the new size, as a seek behind any in flight; asked again when that one settles.
  // An ended file is at its duration, where an exact seek yields no frame, so it seeks to its last frame instead.
  void refitDue() {
    if (!pipeline->refitWanted || seeking || (state != MediaState::Paused && state != MediaState::Ended)) {
      return;
    }
    const auto at = state == MediaState::Ended ? frameTime() : queryPosition();
    if (!at) {
      pipeline->refitWanted = false; // no position to decode at: the frame keeps its old size
      return;
    }
    refitting = true;
    requestSeek(*at, SeekMode::Exact);
  }

  // The pipeline took no seek: a restart ends the file; any other seek just settles. The end that came during it is
  // handled, since no new end follows a refused seek; when a seek that was waiting goes out now, the end is kept and
  // judged again when that seek settles or is refused.
  void seekRefused() {
    const bool missed = std::exchange(endMissed, false);
    if (std::exchange(refitting, false) && state == MediaState::Ended) {
      seeking = false; // a refused refit: the frame keeps its old size
      return;
    }
    if (restarting) {
      seeking = false;
      end();
    } else {
      settle(); // a refused refit leaves the frame at its old size
      if (missed) {
        if (seeking) {
          endMissed = true;
        } else {
          reachEnd();
        }
      }
    }
  }

  // The pipeline settled after loading or a seek. An end that came meanwhile is handled when the pipeline stands at
  // the end of the file, and dropped as stale otherwise; the end that follows a handled one is dropped the same way.
  void settle() {
    seeking = false;
    restarting = false;
    const bool refit = std::exchange(refitting, false);
    const bool missed = std::exchange(endMissed, false);
    if (pipeline->refitWanted) {
      request([](Pipeline& /*pipeline*/) {});
    }
    if (refit && state == MediaState::Ended) {
      return; // the last frame is redrawn; the file is still ended and nothing changed for listeners
    }
    if (state == MediaState::Loading) {
      refreshDuration();
      video = pipeline->video;
      soundMissing = pipeline->soundMissing;
      if (soundMissing && !video) {
        closePipeline();
        soundMissing = true;
        state = MediaState::Error;
        error = kNoSoundDevice;
        notify();
        return;
      }
      state = wantPlay ? MediaState::Playing : MediaState::Paused;
    }
    if (const auto seek = std::exchange(pendingSeek, std::nullopt)) {
      requestSeek(seek->first, seek->second); // the seek that was waiting; an end that came is from before it
    } else if (missed && atEnd()) {
      reachEnd();
      if (state != MediaState::Ended) {
        notify();
      }
      return;
    }
    notify();
  }

  void handle(GstMessage* message) {
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
      GError* problem = nullptr;
      gst_message_parse_error(message, &problem, nullptr);
      std::string text(
          problem != nullptr && problem->message != nullptr ? std::string_view(problem->message) : kCannotPlay
      );
      g_clear_error(&problem);
      failWith(std::move(text));
      break;
    }
    case GST_MESSAGE_EOS:
      // An end that was already queued when a seek was asked is from before the seek; the seek's own end follows.
      if (seeking) {
        endMissed = true;
      } else if (state != MediaState::Ended) {
        reachEnd();
      }
      break;
    case GST_MESSAGE_ASYNC_DONE:
      settle();
      break;
    case GST_MESSAGE_DURATION_CHANGED:
      refreshDuration();
      notify();
      break;
    default:
      break;
    }
  }
};

luciole::MediaPlayer::MediaPlayer(AudioOutput audio) : m_state(std::make_unique<State>(audio)) {}

luciole::MediaPlayer::~MediaPlayer() { m_state->closePipeline(); }

bool luciole::MediaPlayer::available() noexcept { return true; }

bool luciole::MediaPlayer::open(const std::string& path) {
  close();
  auto& s = *m_state;
  gchar* uri =
      path.starts_with('/') && !path.contains('\0') ? g_filename_to_uri(path.c_str(), nullptr, nullptr) : nullptr;
  if (uri == nullptr) {
    s.state = MediaState::Error;
    s.error = "Only absolute paths of local files are played.";
    return false;
  }
  s.pipeline = std::make_shared<media::Pipeline>();
  auto& p = *s.pipeline;
  p.path = path;
  p.uri = uri;
  g_free(uri);
  p.audio = s.audio;
  p.volume = s.volume;
  p.muted = s.muted;
  p.boxWidth = s.boxWidth;
  p.boxHeight = s.boxHeight;
  if (s.tapRate > 0) {
    s.tap = media::makeAudioTap(s.tapRate, s.tapLength);
    p.tap = s.tap;
  }
  s.listen();
  s.state = MediaState::Loading;
  media::start(s.pipeline);
  return true;
}

void luciole::MediaPlayer::close() {
  m_state->closePipeline();
  m_state->state = MediaState::Idle;
  m_state->error.clear();
}

void luciole::MediaPlayer::play() {
  auto& s = *m_state;
  if (!s.pipeline) {
    return;
  }
  if (s.state == MediaState::Ended) {
    s.requestSeek(0, SeekMode::Exact);
  }
  s.lastRestart.reset();
  s.wantPlay = true;
  s.request([](media::Pipeline& p) { p.target = GST_STATE_PLAYING; });
  if (s.state == MediaState::Paused || s.state == MediaState::Ended) {
    s.state = MediaState::Playing;
  }
}

void luciole::MediaPlayer::pause() {
  auto& s = *m_state;
  if (!s.pipeline) {
    return;
  }
  s.wantPlay = false;
  s.request([](media::Pipeline& p) { p.target = GST_STATE_PAUSED; });
  if (s.state == MediaState::Playing) {
    s.state = MediaState::Paused;
  }
}

void luciole::MediaPlayer::seek(double seconds, SeekMode mode) {
  auto& s = *m_state;
  if (!s.pipeline || std::isnan(seconds)) {
    return;
  }
  seconds = std::max(seconds, 0.0);
  if (s.duration > 0) {
    seconds = std::min(seconds, s.duration);
  }
  if (s.state == MediaState::Loading) {
    s.pendingSeek = std::pair{seconds, mode};
    s.lastPosition = seconds;
    return;
  }
  if (s.state == MediaState::Ended) {
    s.state = MediaState::Paused;
  }
  s.lastRestart.reset();
  s.restarting = false;
  s.requestSeek(seconds, mode);
}

void luciole::MediaPlayer::setVolume(double volume) {
  if (std::isnan(volume)) {
    return;
  }
  m_state->volume = std::clamp(volume, 0.0, 1.0);
  m_state->request([volume = m_state->volume](media::Pipeline& p) { p.volume = volume; });
}

void luciole::MediaPlayer::setMuted(bool muted) {
  m_state->muted = muted;
  m_state->request([muted](media::Pipeline& p) { p.muted = muted; });
}

void luciole::MediaPlayer::setLoop(bool loop) { m_state->loop = loop; }

void luciole::MediaPlayer::setFrameSize(int width, int height) {
  m_state->boxWidth = std::clamp(width, 0, kMaxMediaFrameSide);
  m_state->boxHeight = std::clamp(height, 0, kMaxMediaFrameSide);
  m_state->request([w = m_state->boxWidth, h = m_state->boxHeight](media::Pipeline& p) {
    p.boxWidth = w;
    p.boxHeight = h;
  });
}

std::optional<luciole::MediaFrame> luciole::MediaPlayer::takeFrame() {
  return m_state->pipeline ? media::takeFrame(*m_state->pipeline) : std::nullopt;
}

bool luciole::MediaPlayer::startAudioTap(int rate, int length) {
  auto& s = *m_state;
  if (rate < kMinAudioTapRate || rate > kMaxAudioTapRate || length < 1 || length > rate) {
    stopAudioTap();
    return false;
  }
  if (rate == s.tapRate && length == s.tapLength) {
    return true;
  }
  s.tapRate = rate;
  s.tapLength = length;
  s.tap = s.pipeline ? media::makeAudioTap(rate, length) : nullptr;
  s.request([tap = s.tap](media::Pipeline& p) { p.tap = tap; });
  return true;
}

void luciole::MediaPlayer::stopAudioTap() {
  auto& s = *m_state;
  s.tapRate = 0;
  s.tapLength = 0;
  if (s.tap) {
    s.tap.reset();
    s.request([](media::Pipeline& p) { p.tap.reset(); });
  }
}

luciole::AudioWindow luciole::MediaPlayer::copyAudioWindow(std::span<float> samples) {
  auto& s = *m_state;
  if (!s.tap || !s.pipeline || samples.empty() || s.state == MediaState::Loading) {
    return {};
  }
  std::optional<std::int64_t> heard;
  if (GstElement* element = media::elementRef(*s.pipeline)) {
    gint64 at = 0;
    if (gst_element_query_position(element, GST_FORMAT_TIME, &at) != FALSE && at >= 0) {
      heard = at;
    }
    gst_object_unref(element);
  }
  return media::copyAudioWindow(*s.tap, heard, samples);
}

luciole::MediaState luciole::MediaPlayer::state() const noexcept { return m_state->state; }

const std::string& luciole::MediaPlayer::error() const noexcept { return m_state->error; }

double luciole::MediaPlayer::duration() const noexcept { return m_state->duration; }

double luciole::MediaPlayer::position() const {
  const auto& s = *m_state;
  if (!s.pipeline) {
    return 0;
  }
  if (s.state == MediaState::Ended) {
    return s.duration;
  }
  if (s.seeking || s.state == MediaState::Loading) {
    return s.lastPosition;
  }
  if (const auto at = s.queryPosition()) {
    s.lastPosition = *at;
  }
  return s.lastPosition;
}

double luciole::MediaPlayer::volume() const noexcept { return m_state->volume; }

bool luciole::MediaPlayer::muted() const noexcept { return m_state->muted; }

bool luciole::MediaPlayer::loop() const noexcept { return m_state->loop; }

bool luciole::MediaPlayer::hasVideo() const noexcept { return m_state->video; }

bool luciole::MediaPlayer::soundDeviceMissing() const noexcept { return m_state->soundMissing; }

bool luciole::MediaPlayer::seeking() const noexcept { return m_state->seeking; }

void luciole::MediaPlayer::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}

void luciole::MediaPlayer::setFrameCallback(std::function<void()> callback) { m_state->frame = std::move(callback); }

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace luciole {
  enum class MediaState : std::uint8_t {
    Idle,    // no file
    Loading, // opening the file and decoding its first frame
    Paused,
    Playing,
    Ended,
    Error, // error() says why; the pipeline is already freed
  };

  // Where the sound goes: the system's default output, or a sink that keeps the clock and plays nothing.
  enum class AudioOutput : std::uint8_t { System, Silent };

  // Exact lands on the asked time; KeyFrame lands on the nearest key frame, for seeks that follow a drag.
  enum class SeekMode : std::uint8_t { Exact, KeyFrame };

  // The largest frame side the pipeline produces.
  inline constexpr int kMaxMediaFrameSide = 4096;

  // The shortest time a looped file takes from its restart to its end again; a pass shorter than this is not looped.
  inline constexpr std::chrono::milliseconds kMinLoopPass{100};

  // The rates the audio tap delivers, in samples a second.
  inline constexpr int kMinAudioTapRate = 1000;
  inline constexpr int kMaxAudioTapRate = 192000;

  // A copied window of the sound: `count` samples at the start of the caller's buffer, oldest first, and whether
  // they moved since the last copy.
  struct [[nodiscard]] AudioWindow {
    int count = 0;
    bool fresh = false;
  };

  // One decoded picture in RGBA, rows packed at width * 4 bytes. `hold` keeps the pixels alive.
  struct MediaFrame {
    int width = 0;
    int height = 0;
    std::span<const std::uint8_t> rgba;
    std::shared_ptr<const void> hold;
  };

  // Plays one local video or audio file. Decoding and pipeline changes run on other threads; every call here
  // returns at once and the callbacks run on the default GLib main context. Main context only.
  class MediaPlayer {
  public:
    explicit MediaPlayer(AudioOutput audio = AudioOutput::System);
    ~MediaPlayer();

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    // Whether this build can play media at all.
    static bool available() noexcept;
    // A stop that closes a file is still queued or running on the pipeline thread. A file the stop is closing stays
    // open until it ends; the answer is for every player, so it can be true for a file on another drive.
    static bool stopsPending() noexcept;

    // Loads an absolute local path paused at its start, closing the file before. False, in state Error, for a
    // path that is not absolute or when playback is not available; later failures arrive as state Error.
    [[nodiscard]] bool open(const std::string& path);
    // Stops and frees the pipeline; the state returns to Idle.
    void close();
    // Play from Ended starts again from the beginning.
    void play();
    void pause();
    // Seconds from the start, clamped to the duration; NaN is ignored.
    void seek(double seconds, SeekMode mode = SeekMode::Exact);
    // 0 to 1, clamped; NaN is ignored.
    void setVolume(double volume);
    void setMuted(bool muted);
    // With loop on, a file that reaches its end starts again from the beginning and keeps playing, so the state never
    // becomes Ended. It applies at the next end, stays across open() and is off at first. A file of unknown length,
    // a restart that fails, and a file that ends again less than kMinLoopPass after its restart without a seek or a
    // Play in between end as with loop off.
    void setLoop(bool loop);
    // The box in pixels frames must fit in: they keep their aspect and are never larger than the source.
    void setFrameSize(int width, int height);
    // The newest decoded frame, once; empty when none arrived since the last call or the file has no video.
    std::optional<MediaFrame> takeFrame();
    // Taps the sound as it plays, mixed to mono at `rate` samples a second, keeping three seconds of it, so
    // copyAudioWindow can hand out the newest `length` samples heard. False, with the tap off, for a rate outside
    // kMinAudioTapRate to kMaxAudioTapRate or a length outside 1 to `rate`. The tap stays on across files.
    [[nodiscard]] bool startAudioTap(int rate, int length);
    // Removes the tap and frees its samples.
    void stopAudioTap();
    // Copies the samples heard now, at most the tap's length and the size of `samples`, to the start of `samples`.
    // Paused, the window stays where it is and is not fresh. Nothing while the tap is off, the file loads or no
    // sound arrived since the tap started or the last seek.
    AudioWindow copyAudioWindow(std::span<float> samples);

    MediaState state() const noexcept;
    const std::string& error() const noexcept;
    // Seconds; 0 while unknown.
    double duration() const noexcept;
    double position() const;
    double volume() const noexcept;
    bool muted() const noexcept;
    bool loop() const noexcept;
    // Known once the state left Loading.
    bool hasVideo() const noexcept;
    // The system's sound output did not open, so the file plays silent; a file with sound only ends in Error. Known
    // once the state left Loading.
    bool soundDeviceMissing() const noexcept;
    // A seek is in flight: asked of the pipeline and not yet settled, refused or dropped by close().
    bool seeking() const noexcept;

    // State, duration or error changed.
    void setChangedCallback(std::function<void()> callback);
    // A frame is waiting for takeFrame.
    void setFrameCallback(std::function<void()> callback);

  private:
    struct State;
    std::unique_ptr<State> m_state;
  };
} // namespace luciole

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace luciole {
  // What a media file's container says of it, read without decoding.
  struct MediaFacts {
    double seconds = 0; // playing time; 0 while the container does not state it
    int width = 0;      // the first video stream's picture size; 0 without video
    int height = 0;
    bool video = false;
    bool audio = false;
    bool operator==(const MediaFacts&) const = default;
  };

  // Reads one file's facts off the main context from its container headers, with a time limit. The file is
  // treated as hostile: only a regular local file is opened, nothing reaches the network, and nothing is decoded.
  // Main context only.
  class MediaProbe {
  public:
    static constexpr std::chrono::milliseconds kTimeLimit{3000};

    MediaProbe();
    // Cancels the read in progress.
    ~MediaProbe();
    MediaProbe(const MediaProbe&) = delete;
    MediaProbe& operator=(const MediaProbe&) = delete;
    MediaProbe(MediaProbe&&) = delete;
    MediaProbe& operator=(MediaProbe&&) = delete;

    // Whether this build reads media at all; without it start() does nothing.
    static bool available() noexcept;

    // Reads the absolute path `path`, replacing a read in progress. `done` runs once on the default main context,
    // with the facts, or with none when the file is not media, cannot be read or takes longer than `limit`. It never
    // runs after cancel(), another start() or destruction.
    void start(
        const std::string& path, std::function<void(std::optional<MediaFacts>)> done,
        std::chrono::milliseconds limit = kTimeLimit
    );
    // Drops the read in progress; its file is closed on the probe thread.
    void cancel();
    // Whether a read runs whose result has not arrived.
    bool busy() const noexcept;

  private:
    struct Read;
    std::shared_ptr<Read> m_read;
  };
} // namespace luciole

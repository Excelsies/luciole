#pragma once

#include "core/timer_manager.h"
#include "event_loop/glib_poll_source.h"
#include "gl_test_context.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <gst/gst.h>
#include <initializer_list>
#include <netinet/in.h>
#include <numbers>
#include <poll.h>
#include <span>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

// Short clips made at run time for the media tests, in a private directory that also holds GStreamer's plugin
// registry, so no test reads or writes anything under the home folder.
namespace test::media {
  namespace fs = std::filesystem;

  class Scratch {
  public:
    Scratch() {
      TEST_CHECK(::setenv("GST_REGISTRY", (m_folder.path() / "registry.bin").c_str(), 1) == 0);
      gst_init(nullptr, nullptr);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    fs::path path(const char* name) const { return m_folder.path() / name; }

  private:
    luciole::test::TempFolder m_folder{"luciole-media"};
  };

  // The names in `names` that have no element factory, joined by spaces; empty when all exist.
  inline std::string missingElements(std::span<const char* const> names) {
    std::string missing;
    for (const char* name : names) {
      if (GstElementFactory* factory = gst_element_factory_find(name)) {
        gst_object_unref(factory);
      } else {
        missing += missing.empty() ? name : std::string(" ") + name;
      }
    }
    return missing;
  }
  inline std::string missingElements(std::initializer_list<const char*> names) {
    return missingElements(std::span(names.begin(), names.size()));
  }

  struct ClipSpec {
    int width = 160;
    int height = 120;
    int fps = 10;
    int seconds = 3;
    const char* pattern = "smpte";
    bool audio = true;
  };

  // Elements writeClip needs.
  inline constexpr std::array kClipElements{"videotestsrc", "audiotestsrc", "avimux", "filesink", "capsfilter"};

  // Runs a launch description that writes a file until it ends; whether it ended without an error.
  inline bool writeLaunched(const std::string& description) {
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
    if (pipeline == nullptr || error != nullptr) {
      g_clear_error(&error);
      return false;
    }
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* done = gst_bus_timed_pop_filtered(
        bus, 30 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)
    );
    const bool ok = done != nullptr && GST_MESSAGE_TYPE(done) == GST_MESSAGE_EOS;
    if (done != nullptr) {
      gst_message_unref(done);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok;
  }

  // Uncompressed video, with a tone when asked, muxed into AVI by GStreamer itself.
  inline bool writeClip(const fs::path& path, const ClipSpec& spec) {
    std::string description = std::format(
        "videotestsrc num-buffers={} pattern={} ! video/x-raw,format=I420,width={},height={},framerate={}/1 ! "
        "avimux name=mux ! filesink location=\"{}\"",
        spec.fps * spec.seconds, spec.pattern, spec.width, spec.height, spec.fps, path.string()
    );
    if (spec.audio) {
      description += std::format(
          " audiotestsrc num-buffers={} samplesperbuffer=800 ! audio/x-raw,format=S16LE,rate=8000,channels=1 ! mux.",
          spec.seconds * 10
      );
    }
    return writeLaunched(description);
  }

  // Elements the compressed clips below need; a test that lacks them skips them.
  inline constexpr std::array kParsedClipElements{"avenc_mpeg4", "mpeg4videoparse", "lamemp3enc", "mpegaudioparse",
                                                  "mp4mux",      "qtdemux",         "flacenc",    "flacparse"};

  // Two seconds of 96 by 64 MPEG-4 video with MP3 sound in MP4: both streams reach the reader through a parser.
  inline bool writeMp4(const fs::path& path) {
    return writeLaunched(
        std::format(
            "videotestsrc num-buffers=20 ! video/x-raw,width=96,height=64,framerate=10/1 ! avenc_mpeg4 ! "
            "mpeg4videoparse ! "
            "mp4mux name=mux ! filesink location=\"{}\" "
            "audiotestsrc num-buffers=20 samplesperbuffer=4410 ! audio/x-raw,rate=44100,channels=1 ! lamemp3enc ! "
            "mpegaudioparse ! mux.",
            path.string()
        )
    );
  }

  // Two seconds of sound encoded by `encoder`, written as is: FLAC with "flacenc", MP3 with "lamemp3enc".
  inline bool writeSound(const fs::path& path, const char* encoder) {
    return writeLaunched(
        std::format(
            "audiotestsrc num-buffers=20 samplesperbuffer=4410 ! audio/x-raw,rate=44100,channels=1 ! {} ! "
            "filesink location=\"{}\"",
            encoder, path.string()
        )
    );
  }

  // A mono 16-bit 8 kHz sine as a WAV file written byte by byte.
  inline void writeTone(const fs::path& path, int seconds) {
    constexpr std::uint32_t kRate = 8000;
    const std::uint32_t samples = kRate * static_cast<std::uint32_t>(seconds);
    std::vector<std::uint8_t> bytes;
    const auto put = [&bytes](std::uint32_t value, int size) {
      for (int i = 0; i < size; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
      }
    };
    const auto tag = [&bytes](const char* text) { bytes.insert(bytes.end(), text, text + 4); };
    tag("RIFF");
    put(36 + samples * 2, 4);
    tag("WAVE");
    tag("fmt ");
    put(16, 4);
    put(1, 2); // PCM
    put(1, 2); // mono
    put(kRate, 4);
    put(kRate * 2, 4);
    put(2, 2);
    put(16, 2);
    tag("data");
    put(samples * 2, 4);
    for (std::uint32_t i = 0; i < samples; ++i) {
      const double wave = std::sin(2.0 * std::numbers::pi * 440.0 * i / kRate);
      put(static_cast<std::uint16_t>(static_cast<std::int16_t>(wave * 8000)), 2);
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::ssize(bytes));
  }

  inline void writeBytes(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }

  // Whether a descriptor of this process refers to `path`.
  inline bool holdsOpen(const fs::path& path) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator("/proc/self/fd", error)) {
      std::error_code ignored;
      if (fs::read_symlink(entry.path(), ignored) == path) {
        return true;
      }
    }
    return false;
  }

  // How long a test waits for a condition before it fails; a machine under load still meets it.
  inline constexpr std::chrono::milliseconds kWait{20'000};

  // Runs the default main context and the application timers for at most 5 ms.
  class Pump {
  public:
    void once() {
      std::vector<pollfd> fds;
      const auto start = m_source.addPollFds(fds);
      int wait = 5;
      for (const int due : {m_source.pollTimeoutMs(), TimerManager::instance().pollTimeoutMs()}) {
        if (due >= 0) {
          wait = std::min(wait, due);
        }
      }
      TEST_CHECK(::poll(fds.data(), fds.size(), wait) >= 0);
      m_source.dispatch(fds, start);
      TimerManager::instance().tick();
    }
    // Pumps until `done` holds or `limit` passed on the steady clock; whether it held.
    bool until(const std::function<bool()>& done, std::chrono::milliseconds limit = kWait) {
      const auto deadline = std::chrono::steady_clock::now() + limit;
      // Each round takes at least a poll, so the rounds end with the deadline; the count bounds the loop as well.
      const auto rounds = limit.count() * 100;
      for (std::int64_t i = 0; i < rounds && std::chrono::steady_clock::now() < deadline; ++i) {
        if (done()) {
          return true;
        }
        once();
      }
      return done();
    }
    // Pumps for `span` on the steady clock.
    void run(std::chrono::milliseconds span) {
      (void)until([] { return false; }, span);
    }

  private:
    GlibPollSource m_source;
  };

  // A loopback port that accepts connections without ever answering; closed with the object.
  class Listener {
  public:
    Listener() : m_fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {
      TEST_CHECK(m_fd >= 0);
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      TEST_CHECK(::bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
      TEST_CHECK(::listen(m_fd, 4) == 0);
      socklen_t length = sizeof(address);
      TEST_CHECK(::getsockname(m_fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
      m_port = ntohs(address.sin_port);
    }
    ~Listener() { ::close(m_fd); }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    int port() const { return m_port; }
    bool connected() const {
      const int peer = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
      if (peer >= 0) {
        ::close(peer);
      }
      return peer >= 0;
    }

  private:
    int m_fd = -1;
    int m_port = 0;
  };

  // An element that refuses to open, standing in for a sound output with no device behind it.
  inline GType brokenSinkType() {
    static const GType type = [] {
      static GstStaticPadTemplate sinkTemplate =
          GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
      const auto classInit = [](gpointer klass, gpointer /*data*/) {
        auto* element = GST_ELEMENT_CLASS(klass);
        gst_element_class_set_static_metadata(element, "Broken sink", "Sink/Audio", "Never opens", "tests");
        gst_element_class_add_static_pad_template(element, &sinkTemplate);
        element->change_state = [](GstElement* self, GstStateChange change) {
          if (change == GST_STATE_CHANGE_NULL_TO_READY) {
            return GST_STATE_CHANGE_FAILURE;
          }
          auto* parent = GST_ELEMENT_CLASS(g_type_class_peek(GST_TYPE_ELEMENT));
          return parent->change_state(self, change);
        };
      };
      const auto instanceInit = [](GTypeInstance* instance, gpointer /*klass*/) {
        (void)gst_element_add_pad(GST_ELEMENT(instance), gst_pad_new_from_static_template(&sinkTemplate, "sink"));
      };
      return g_type_register_static_simple(
          GST_TYPE_ELEMENT, "LucioleBrokenAudioSink", sizeof(GstElementClass), classInit, sizeof(GstElement),
          instanceInit, GTypeFlags{}
      );
    }();
    return type;
  }

  // Makes the system's sound output the broken element for the rest of the process, so nothing can make a sound.
  inline void breakSystemSoundOutput() {
    TEST_CHECK(gst_element_register(nullptr, "autoaudiosink", GST_RANK_NONE, brokenSinkType()) == TRUE);
    GstElement* sink = gst_element_factory_make("autoaudiosink", nullptr);
    TEST_CHECK(sink != nullptr && G_TYPE_CHECK_INSTANCE_TYPE(sink, brokenSinkType()));
    gst_object_unref(gst_object_ref_sink(sink));
  }

  // GST_PLUGIN_FEATURE_RANK text that ranks every real audio sink out of automatic choice.
  inline std::string audioSinksRankedOut() {
    std::string ranks;
    GList* sinks = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_SINK | GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO, GST_RANK_NONE
    );
    for (const GList* item = sinks; item != nullptr; item = item->next) {
      const std::string_view name = GST_OBJECT_NAME(item->data);
      if (name != "autoaudiosink" && !name.starts_with("fake")) {
        ranks += ranks.empty() ? "" : ",";
        ranks += std::format("{}:NONE", name);
      }
    }
    gst_plugin_feature_list_free(sinks);
    return ranks;
  }

  // Runs this executable again with `argument` and every real audio sink ranked out, so the system output cannot
  // open as on a machine with no sound device; its exit status, or -1 when it did not end within `limit`.
  inline int runWithoutSoundDevices(const char* argument, std::chrono::seconds limit = std::chrono::seconds{120}) {
    std::error_code error;
    const auto self = fs::read_symlink("/proc/self/exe", error);
    const std::string ranks = "GST_PLUGIN_FEATURE_RANK=" + audioSinksRankedOut();
    std::vector<std::string> kept;
    for (char** entry = environ; *entry != nullptr; ++entry) {
      if (!std::string_view(*entry).starts_with("GST_PLUGIN_FEATURE_RANK=")) {
        kept.emplace_back(*entry);
      }
    }
    kept.push_back(ranks);
    std::vector<char*> env;
    env.reserve(kept.size() + 1);
    for (auto& entry : kept) {
      env.push_back(entry.data());
    }
    env.push_back(nullptr);
    std::string program = self.string();
    std::string flag = argument;
    char* args[] = {program.data(), flag.data(), nullptr};
    pid_t child = -1;
    if (error || ::posix_spawn(&child, program.c_str(), nullptr, nullptr, args, env.data()) != 0) {
      return -1;
    }
    const auto deadline = std::chrono::steady_clock::now() + limit;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      if (::waitpid(child, &status, WNOHANG) == child) {
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      }
      ::usleep(20'000);
    }
    ::kill(child, SIGKILL);
    (void)::waitpid(child, &status, 0);
    return -1;
  }

  // Processor time of this process, in milliseconds.
  inline double processorMs() {
    timespec now{};
    TEST_CHECK(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) == 0);
    return static_cast<double>(now.tv_sec) * 1000.0 + static_cast<double>(now.tv_nsec) / 1e6;
  }
} // namespace test::media

#include "bink2_sample.h"
#include "media/gstreamer_runtime.h"
#include "media/media_player.h"
#include "media_clips.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gst/app/gstappsink.h>
#include <print>
#include <thread>
#include <vector>

// How playback ends when the world changes under it: a file that shrinks after it was opened must end in an error, not
// in silence, at the element and through the player; and a consumer that keeps the pictures it was given must not
// make stopping wait for ever. It needs a real Bink 2 file: LUCIOLE_BINK2_SAMPLE names one, and without it the test
// exits with meson's skip code, 77. The files are private copies of its start.
namespace {
  namespace bink = test::bink2;
  using namespace test::media;
  using namespace std::chrono_literals;

  // A private copy of up to 16 MiB of the start of `sample`, and the size it is cut to later: its middle.
  fs::path copyOfTheStart(const fs::path& sample, const fs::path& folder, std::uintmax_t& cut) {
    std::ifstream in(sample, std::ios::binary);
    std::vector<char> bytes(16U << 20);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    const fs::path copy = folder / "shrinking.bik";
    std::ofstream(copy, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    cut = bytes.size() / 2;
    return copy;
  }

  // A sink that takes every picture, or an application sink that holds the one it is given until it is pulled.
  GstElement* pipelineOn(const fs::path& path, GstElement*& sink, bool pulled) {
    GstElement* pipeline = gst_pipeline_new(nullptr);
    GstElement* source = gst_element_factory_make("filesrc", nullptr);
    GstElement* demux = gst_element_factory_make("luciolebink2demux", nullptr);
    sink = gst_element_factory_make(pulled ? "appsink" : "fakesink", nullptr);
    TEST_CHECK(pipeline != nullptr && source != nullptr && demux != nullptr && sink != nullptr);
    g_object_set(source, "location", path.c_str(), nullptr);
    g_object_set(sink, "sync", FALSE, nullptr);
    if (pulled) {
      g_object_set(sink, "max-buffers", 1U, "drop", FALSE, nullptr);
    }
    gst_bin_add_many(GST_BIN(pipeline), source, demux, sink, nullptr);
    TEST_CHECK(gst_element_link(source, demux) != FALSE);
    g_signal_connect(
        demux, "pad-added", G_CALLBACK(+[](GstElement* /*element*/, GstPad* pad, gpointer data) {
          GstPad* entry = gst_element_get_static_pad(GST_ELEMENT(data), "sink");
          if (std::string_view(GST_PAD_NAME(pad)) == "video_0") {
            TEST_CHECK(gst_pad_link(pad, entry) == GST_PAD_LINK_OK);
          }
          gst_object_unref(entry);
        }),
        sink
    );
    return pipeline;
  }

  // The file is cut after the element prerolled: playing on reaches the cut, and the pipeline ends in an error that
  // says the file ended early or changed, within a bound, where it used to wait for ever.
  void aFileThatShrinksEndsInAnError(const fs::path& sample, const fs::path& folder) {
    std::uintmax_t cut = 0;
    const fs::path copy = copyOfTheStart(sample, folder, cut);
    GstElement* sink = nullptr;
    GstElement* pipeline = pipelineOn(copy, sink, false);
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    TEST_CHECK(gst_element_get_state(pipeline, nullptr, nullptr, 120 * GST_SECOND) == GST_STATE_CHANGE_SUCCESS);
    fs::resize_file(copy, cut);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 60 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)
    );
    TEST_CHECK(message != nullptr && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR);
    GError* error = nullptr;
    gst_message_parse_error(message, &error, nullptr);
    TEST_CHECK(error != nullptr && std::string_view(error->message).contains("ended early or changed"));
    std::println("a file cut under the element: {}", error->message);
    g_clear_error(&error);
    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
  }

  // The same through the player: it ends in the Error state with a reason.
  void aFileThatShrinksEndsThePlayerInAnError(const fs::path& sample, const fs::path& folder) {
    std::uintmax_t cut = 0;
    const fs::path copy = copyOfTheStart(sample, folder, cut);
    Pump pump;
    luciole::MediaPlayer player(luciole::AudioOutput::Silent);
    player.setFrameSize(320, 180);
    TEST_CHECK(player.open(copy.string()));
    TEST_CHECK(pump.until([&] { return player.state() == luciole::MediaState::Paused; }, 120s));
    fs::resize_file(copy, cut);
    player.play();
    TEST_CHECK(pump.until([&] { return player.state() == luciole::MediaState::Error; }, 60s));
    TEST_CHECK(player.error().contains("ended early or changed"));
    std::println("a file cut under the player: {}", player.error());
  }

  // A consumer that keeps the pictures it takes holds all of the element's buffers: the streaming task waits for one
  // and stopping the pipeline must still end, since going down makes the pool stop handing buffers out.
  void aConsumerThatKeepsPicturesCannotHangTheStop(const fs::path& sample) {
    GstElement* sink = nullptr;
    GstElement* pipeline = pipelineOn(sample, sink, true);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    std::vector<GstSample*> held;
    // The first picture takes as long as the file needs; then every pull keeps its picture until one finds nothing
    // waiting, which is when the pool is empty and the task waits.
    GstSample* first = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 60 * GST_SECOND);
    TEST_CHECK(first != nullptr);
    held.push_back(first);
    while (GstSample* picture = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND)) {
      held.push_back(picture);
      TEST_CHECK(held.size() < 32);
    }
    TEST_CHECK(held.size() >= 2);
    std::this_thread::sleep_for(500ms); // the task is now waiting for a buffer
    auto stopped = std::async(std::launch::async, [&] { return gst_element_set_state(pipeline, GST_STATE_NULL); });
    TEST_CHECK(stopped.wait_for(20s) == std::future_status::ready);
    TEST_CHECK(stopped.get() == GST_STATE_CHANGE_SUCCESS);
    for (GstSample* picture : held) {
      gst_sample_unref(picture);
    }
    gst_object_unref(pipeline);
    std::println("stopping with the buffers held downstream ended");
  }
} // namespace

int main() {
  Scratch scratch;
  TEST_CHECK(luciole::media::startProblem().empty());
  const auto sample = bink::sampleFrom(bink::kSampleVariable);
  const auto& path = bink::needSample(sample, bink::kSampleVariable);
  const fs::path folder = scratch.path("x").parent_path();
  aFileThatShrinksEndsInAnError(path, folder);
  aFileThatShrinksEndsThePlayerInAnError(path, folder);
  aConsumerThatKeepsPicturesCannotHangTheStop(path);
  std::println("bink2 failure checks passed");
  return 0;
}

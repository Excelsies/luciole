#include "media/gstreamer_pipeline.h"
#include "media/gstreamer_runtime.h"
#include "media_clips.h"
#include "test_check.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// The refusal of network sources: a parser or decoder bin is told to skip an element that fetches over the network
// and the bin ends with an error, a local demuxer is left alone, and every autoplugging bin of a playing pipeline
// has the signal the refusal listens to.
namespace {
  using namespace test::media;

  constexpr std::string_view kLocalOnly = "Only local files are played.";
  // The values of GstAutoplugSelectResult.
  constexpr int kTry = 0;
  constexpr int kSkip = 2;

  // The first element among `names` that exists, or an empty name.
  std::string firstElement(std::initializer_list<const char*> names) {
    for (const char* name : names) {
      if (GstElementFactory* factory = gst_element_factory_find(name)) {
        gst_object_unref(factory);
        return name;
      }
    }
    return {};
  }

  // A pipeline with `filesrc ! <bin>` made under the refusal, and its bus.
  struct Rig {
    GstElement* pipeline = gst_pipeline_new(nullptr);
    GstElement* bin = nullptr;
    GstBus* bus = nullptr;

    explicit Rig(const char* binName) {
      gst_object_ref_sink(pipeline);
      luciole::media::refuseNetwork(pipeline);
      GstElement* source = gst_element_factory_make("filesrc", nullptr);
      bin = gst_element_factory_make(binName, nullptr);
      TEST_CHECK(source != nullptr && bin != nullptr);
      gst_bin_add_many(GST_BIN(pipeline), source, bin, nullptr);
      TEST_CHECK(gst_element_link(source, bin) != FALSE);
      bus = gst_element_get_bus(pipeline);
    }
    ~Rig() {
      (void)gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(bus);
      gst_object_unref(pipeline);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // What the bin answers when asked whether it may build `factoryName`.
    int asks(const std::string& factoryName) const {
      GstElementFactory* factory = gst_element_factory_find(factoryName.c_str());
      TEST_CHECK(factory != nullptr);
      GstPad* pad = gst_pad_new("probe", GST_PAD_SRC);
      GstCaps* caps = gst_caps_new_empty_simple("application/octet-stream");
      int result = -1;
      g_signal_emit_by_name(bin, "autoplug-select", pad, caps, factory, &result);
      gst_caps_unref(caps);
      gst_object_unref(gst_object_ref_sink(pad));
      gst_object_unref(factory);
      return result;
    }
    // The text of the next error on the bus, empty when there is none.
    std::string error() const {
      GstMessage* message = gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ERROR);
      std::string text;
      if (message != nullptr) {
        GError* problem = nullptr;
        gst_message_parse_error(message, &problem, nullptr);
        text = problem != nullptr ? problem->message : "";
        g_clear_error(&problem);
        gst_message_unref(message);
      }
      return text;
    }
  };

  void refusesAdaptiveDemuxers(const char* binName, const std::string& adaptive, const std::string& local) {
    Rig rig(binName);
    TEST_CHECK(g_signal_lookup("autoplug-select", G_OBJECT_TYPE(rig.bin)) != 0);
    TEST_CHECK(rig.asks(local) == kTry);
    TEST_CHECK(rig.error().empty());
    TEST_CHECK(rig.asks(adaptive) == kSkip);
    TEST_CHECK(rig.error() == kLocalOnly);
    std::println("{}: {} skipped, {} tried", binName, adaptive, local);
  }

  // An element of the Network class added below the pipeline keeps its state and ends the pipeline with the error.
  void locksNetworkElements() {
    Rig rig("identity");
    GstElement* socket = gst_element_factory_make("tcpclientsrc", nullptr);
    TEST_CHECK(socket != nullptr);
    gst_bin_add(GST_BIN(rig.pipeline), socket);
    TEST_CHECK(gst_element_is_locked_state(socket) != FALSE);
    TEST_CHECK(rig.error() == kLocalOnly);
    // A local source is neither locked nor reported.
    GstElement* local = gst_element_factory_make("fakesrc", nullptr);
    TEST_CHECK(local != nullptr);
    gst_bin_add(GST_BIN(rig.pipeline), local);
    TEST_CHECK(gst_element_is_locked_state(local) == FALSE);
    TEST_CHECK(rig.error().empty());
  }

  bool isAutoplugging(GstElement* element) {
    GstElementFactory* factory = gst_element_get_factory(element);
    const std::string_view name = factory != nullptr ? GST_OBJECT_NAME(factory) : "";
    return name.contains("decodebin") || name.contains("parsebin");
  }

  // Every element of the playing pipeline that builds elements by itself, as (name, has the signal, guard connected).
  struct Bin {
    std::string name;
    bool hasSignal = false;
    bool guarded = false;
  };

  void record(std::vector<Bin>& bins, GstElement* element) {
    if (!isAutoplugging(element)) {
      return;
    }
    const guint id = g_signal_lookup("autoplug-select", G_OBJECT_TYPE(element));
    bins.push_back(
        {.name = GST_OBJECT_NAME(gst_element_get_factory(element)),
         .hasSignal = id != 0,
         .guarded = id != 0 && g_signal_handler_find(element, G_SIGNAL_MATCH_ID, id, 0, nullptr, nullptr, nullptr) != 0}
    );
  }

  // `root` and every element below it that builds elements by itself.
  std::vector<Bin> autopluggingBins(GstElement* root) {
    std::vector<Bin> bins;
    record(bins, root);
    GstIterator* all = gst_bin_iterate_recurse(GST_BIN(root));
    GValue item = G_VALUE_INIT;
    // The iterator ends with the pipeline's finite tree; a resync restarts it a few times at most.
    int resyncs = 0;
    for (bool more = true; more && resyncs < 8;) {
      switch (gst_iterator_next(all, &item)) {
      case GST_ITERATOR_OK: {
        auto* element = GST_ELEMENT(g_value_get_object(&item));
        record(bins, element);
        g_value_reset(&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        bins.clear();
        record(bins, root);
        gst_iterator_resync(all);
        ++resyncs;
        break;
      default:
        more = false;
        break;
      }
    }
    g_value_unset(&item);
    gst_iterator_free(all);
    return bins;
  }

  // The player's own pipeline, built by the player's code on a clip with sound and picture: each parser or decoder
  // bin in it must have the signal and carry the guard, so a later change of playback element cannot drop it.
  void everyAutopluggingBinOfThePlayerIsGuarded(const fs::path& clip) {
    Pump pump;
    auto pipeline = std::make_shared<luciole::media::Pipeline>();
    pipeline->path = clip.string();
    pipeline->uri = "file://" + clip.string();
    pipeline->audio = luciole::AudioOutput::Silent;
    luciole::media::start(pipeline);
    GstElement* element = nullptr;
    const auto paused = [&] {
      if (element == nullptr) {
        element = luciole::media::elementRef(*pipeline);
      }
      GstState state = GST_STATE_NULL;
      return element != nullptr
          && gst_element_get_state(element, &state, nullptr, 0) == GST_STATE_CHANGE_SUCCESS
          && state == GST_STATE_PAUSED;
    };
    TEST_CHECK(pump.until(paused));
    if (element != nullptr) {
      const auto bins = autopluggingBins(element);
      std::string seen;
      bool decoding = false;
      for (const auto& bin : bins) {
        seen += (seen.empty() ? "" : " ") + bin.name;
        decoding = decoding || bin.name == "decodebin";
        if (!bin.hasSignal || !bin.guarded) {
          std::println("{}: signal {}, guard {}", bin.name, bin.hasSignal, bin.guarded);
        }
        TEST_CHECK(bin.hasSignal);
        TEST_CHECK(bin.guarded);
      }
      std::println("autoplugging bins of the player's pipeline: {}", seen);
      TEST_CHECK(decoding);
      TEST_CHECK(bins.size() >= 2);
      gst_object_unref(element);
    }
    luciole::media::stop(pipeline);
    pipeline.reset();
    TEST_CHECK(pump.until([&] { return !holdsOpen(clip); }));
  }
} // namespace

int main() {
  Scratch scratch;
  if (const auto missing = missingElements(
          {"filesrc", "parsebin", "decodebin", "playbin", "identity", "tcpclientsrc", "fakesrc", "fakesink", "appsink",
           "videoconvertscale", "capsfilter"}
      );
      !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  const std::string adaptive = firstElement({"hlsdemux2", "hlsdemux", "dashdemux2", "dashdemux", "mssdemux2"});
  const std::string local = firstElement({"avidemux", "matroskademux", "qtdemux"});
  if (adaptive.empty() || local.empty()) {
    std::println(
        "skipped: GStreamer lacks {}",
        adaptive.empty() ? "an adaptive demuxer (hlsdemux2)" : "a local demuxer (avidemux)"
    );
    return ::test::kSkipExitCode;
  }
  refusesAdaptiveDemuxers("parsebin", adaptive, local);
  refusesAdaptiveDemuxers("decodebin", adaptive, local);
  locksNetworkElements();
  if (const auto lacking = missingElements(kClipElements); !lacking.empty()) {
    std::println("skipped: the player's pipeline is not checked, GStreamer lacks {}", lacking);
    return ::test::kSkipExitCode;
  }
  const auto clip = scratch.path("clip.avi");
  TEST_CHECK(writeClip(clip, {.width = 64, .height = 48, .seconds = 2}));
  everyAutopluggingBinOfThePlayerIsGuarded(clip);
  std::println("media-network-refusal: all checks passed");
  return 0;
}

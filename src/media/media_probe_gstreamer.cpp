#include "media/gstreamer_runtime.h"
#include "media/media_probe.h"
#include "media/pipeline_thread.h"

#include <atomic>
#include <cmath>
#include <mutex>

// A read in progress: the main context owns `done` and `finished`; the probe thread only reads `cancelled`.
struct luciole::MediaProbe::Read {
  std::atomic<bool> cancelled{false};
  std::function<void(std::optional<MediaFacts>)> done;
  bool finished = false;
  bool timedOut = false;
};

namespace {
  using luciole::MediaFacts;

  // How often the probe thread looks at the clock and cancellation while it waits.
  constexpr auto kSlice = std::chrono::milliseconds(20);
  // A picture side or playing time past these is not believed.
  constexpr int kLargestSide = 32768;
  constexpr double kLongestSeconds = 1e7;
  // Buffers a parser may see to estimate a playing time its container does not state, as in an MP3; the streams
  // stop there, so no more of the file is read.
  constexpr int kEstimateBuffers = 128;
  // The streaming threads wake the probe thread at most three times: all streams settled, the buffers ran out,
  // all streams ended.
  constexpr int kWakes = 3;

  // What the streaming threads found, guarded by `lock`.
  struct Found {
    GstBin* pipeline = nullptr; // where the sinks go
    std::mutex lock;
    MediaFacts facts;
    GstPad* first = nullptr; // the first exposed stream, owned, for the duration query
    // parsebin exposes every stream before any buffer flows, so `streams` is final once one settles.
    int streams = 0;
    int settled = 0; // streams whose first buffer, gap or end passed, with their caps then set
    int ended = 0;
    std::atomic<int> buffers{0}; // buffers that passed, up to kEstimateBuffers
  };

  // One exposed stream; its probe runs on that stream's thread only.
  struct Stream {
    Found* found = nullptr;
    bool settled = false;
    bool ended = false;
  };

  // Streaming thread: wakes the probe thread.
  void wake(const Found& found) {
    (void)gst_element_post_message(
        GST_ELEMENT(found.pipeline),
        gst_message_new_application(GST_OBJECT(found.pipeline), gst_structure_new_empty("luciole-probe"))
    );
  }

  // Records what the stream's caps say; whether it was the last stream to settle. A parser exposes its stream
  // before its caps are complete, so this runs when the first buffer, gap or end passes and the caps are set.
  bool settle(Found& found, GstPad* pad) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    const GstStructure* structure =
        caps != nullptr && gst_caps_get_size(caps) > 0 ? gst_caps_get_structure(caps, 0) : nullptr;
    const std::string_view name = structure != nullptr ? gst_structure_get_name(structure) : "";
    int width = 0;
    int height = 0;
    bool last = false;
    {
      const std::scoped_lock lock(found.lock);
      if (name.starts_with("video/")) {
        if (!found.facts.video
            && gst_structure_get_int(structure, "width", &width) != FALSE
            && gst_structure_get_int(structure, "height", &height) != FALSE
            && width > 0
            && height > 0
            && width <= kLargestSide
            && height <= kLargestSide) {
          found.facts.width = width;
          found.facts.height = height;
        }
        found.facts.video = true;
      } else if (name.starts_with("audio/")) {
        found.facts.audio = true;
      }
      last = ++found.settled == found.streams;
    }
    if (caps != nullptr) {
      gst_caps_unref(caps);
    }
    return last;
  }

  // Streaming thread: settles the stream at its first buffer, gap or end. Buffers are dropped, so no sink holds
  // the demuxer's thread before the other streams reach theirs, until kEstimateBuffers passed; the stream then
  // blocks until the pipeline stops.
  GstPadProbeReturn streamPassed(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
    auto& stream = *static_cast<Stream*>(data);
    Found& found = *stream.found;
    const bool buffer = (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0;
    const GstEventType event = buffer ? GST_EVENT_UNKNOWN : GST_EVENT_TYPE(gst_pad_probe_info_get_event(info));
    if (!stream.settled && (buffer || event == GST_EVENT_GAP || event == GST_EVENT_EOS)) {
      stream.settled = true;
      if (settle(found, pad)) {
        wake(found);
      }
    }
    if (event == GST_EVENT_EOS && !stream.ended) {
      stream.ended = true;
      bool all = false;
      {
        const std::scoped_lock lock(found.lock);
        all = ++found.ended == found.streams;
      }
      if (all) {
        wake(found);
      }
    }
    if (!buffer) {
      return GST_PAD_PROBE_PASS;
    }
    if (found.buffers >= kEstimateBuffers) {
      return GST_PAD_PROBE_OK;
    }
    if (++found.buffers == kEstimateBuffers) {
      wake(found);
    }
    return GST_PAD_PROBE_DROP;
  }

  // Streaming thread: watches the new stream and ends it in a sink, so it is linked.
  void streamAdded(GstElement* /*parsebin*/, GstPad* pad, gpointer data) {
    auto* found = static_cast<Found*>(data);
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    if (sink == nullptr) {
      return;
    }
    {
      const std::scoped_lock lock(found->lock);
      ++found->streams;
      if (found->first == nullptr) {
        found->first = GST_PAD(gst_object_ref(pad));
      }
    }
    g_object_set(sink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, nullptr);
    (void)gst_pad_add_probe(
        pad,
        static_cast<GstPadProbeType>(
            GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM
        ),
        streamPassed, new Stream{.found = found}, [](gpointer stream) { delete static_cast<Stream*>(stream); }
    );
    (void)gst_bin_add(found->pipeline, sink);
    GstPad* input = gst_element_get_static_pad(sink, "sink");
    (void)gst_pad_link(pad, input);
    gst_object_unref(input);
    (void)gst_element_sync_state_with_parent(sink);
  }

  double durationOf(GstPad* pad) {
    gint64 length = 0;
    if (pad == nullptr || gst_pad_query_duration(pad, GST_FORMAT_TIME, &length) == FALSE || length <= 0) {
      return 0;
    }
    const double seconds = static_cast<double>(length) / static_cast<double>(GST_SECOND);
    return std::isfinite(seconds) && seconds < kLongestSeconds ? seconds : 0;
  }

  // The facts once every stream settled, with the playing time once it is known, the buffers a parser may see
  // to estimate it passed, or every stream ended; none before.
  std::optional<MediaFacts> settledFacts(Found& found) {
    MediaFacts facts;
    GstPad* first = nullptr;
    bool ended = false;
    {
      const std::scoped_lock lock(found.lock);
      if (found.streams == 0 || found.settled < found.streams) {
        return std::nullopt;
      }
      facts = found.facts;
      first = found.first;
      ended = found.ended == found.streams;
    }
    facts.seconds = durationOf(first);
    if (facts.seconds > 0 || ended || found.buffers >= kEstimateBuffers) {
      return facts;
    }
    return std::nullopt;
  }

  // Waits for the facts; none on an error, cancellation or the deadline, which sets `timedOut`. Each pass ends with a
  // slice, a wake or an error, and there are at most kWakes wakes.
  std::optional<MediaFacts> awaitFacts(
      GstBus* bus, Found& found, const std::atomic<bool>& cancelled, std::chrono::milliseconds limit, bool& timedOut
  ) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    const auto passes = limit / kSlice + 1 + kWakes;
    const auto wait = static_cast<GstClockTime>(std::chrono::duration_cast<std::chrono::nanoseconds>(kSlice).count());
    const auto types = static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_APPLICATION);
    for (std::int64_t i = 0; i < passes && std::chrono::steady_clock::now() < deadline && !cancelled; ++i) {
      if (auto facts = settledFacts(found)) {
        return facts;
      }
      if (GstMessage* message = gst_bus_timed_pop_filtered(bus, wait, types)) {
        const bool error = GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
        gst_message_unref(message);
        if (error) {
          return std::nullopt;
        }
      }
    }
    timedOut = !cancelled;
    return std::nullopt;
  }

  // Probe thread: the facts of `path`, or none.
  std::optional<MediaFacts>
  probe(const std::string& path, const std::atomic<bool>& cancelled, std::chrono::milliseconds limit, bool& timedOut) {
    if (!path.starts_with('/')
        || !luciole::media::startProblem().empty()
        || !luciole::media::fileProblem(path).empty()) {
      return std::nullopt;
    }
    GstElement* pipeline = gst_pipeline_new(nullptr);
    GstElement* source = gst_element_factory_make("filesrc", nullptr);
    GstElement* parse = gst_element_factory_make("parsebin", nullptr);
    if (pipeline == nullptr || source == nullptr || parse == nullptr) {
      for (GstElement* element : {pipeline, source, parse}) {
        if (element != nullptr) {
          gst_object_unref(gst_object_ref_sink(element));
        }
      }
      return std::nullopt;
    }
    gst_object_ref_sink(pipeline);
    g_object_set(source, "location", path.c_str(), nullptr);
    luciole::media::refuseNetwork(pipeline);
    gst_bin_add_many(GST_BIN(pipeline), source, parse, nullptr);
    Found found;
    found.pipeline = GST_BIN(pipeline);
    g_signal_connect(parse, "pad-added", G_CALLBACK(streamAdded), &found);
    GstBus* bus = gst_element_get_bus(pipeline);
    const bool linked = gst_element_link(source, parse) != FALSE;
    const bool started = linked && gst_element_set_state(pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE;
    const auto result = started ? awaitFacts(bus, found, cancelled, limit, timedOut) : std::nullopt;
    (void)gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    if (found.first != nullptr) {
      gst_object_unref(found.first);
    }
    return result && (result->video || result->audio) ? result : std::nullopt;
  }
} // namespace

luciole::MediaProbe::MediaProbe() = default;

luciole::MediaProbe::~MediaProbe() { cancel(); }

bool luciole::MediaProbe::available() noexcept { return true; }

bool luciole::MediaProbe::readsPending() noexcept { return media::probeThreadBusy(); }

void luciole::MediaProbe::start(
    const std::string& path, std::function<void(std::optional<MediaFacts>)> done, std::chrono::milliseconds limit
) {
  cancel();
  m_read = std::make_shared<Read>();
  m_read->done = std::move(done);
  media::runOnProbeThread([read = m_read, path, limit] {
    // A read dropped while it waited in the queue opens nothing.
    if (read->cancelled) {
      return;
    }
    bool timedOut = false;
    auto facts = probe(path, read->cancelled, limit, timedOut);
    media::runOnMainContext([read, facts, timedOut] {
      if (read->cancelled || read->finished) {
        return;
      }
      read->finished = true;
      read->timedOut = timedOut;
      // A copy, so the owner may start another read from the callback.
      const auto done = std::move(read->done);
      done(facts);
    });
  });
}

void luciole::MediaProbe::cancel() {
  if (m_read) {
    m_read->cancelled = true;
    m_read.reset();
  }
}

bool luciole::MediaProbe::busy() const noexcept { return m_read && !m_read->finished; }

bool luciole::MediaProbe::timedOut() const noexcept { return m_read && m_read->finished && m_read->timedOut; }

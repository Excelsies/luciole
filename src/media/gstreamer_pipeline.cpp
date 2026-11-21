#include "media/gstreamer_pipeline.h"

#include "media/bink2_demux.h"
#include "media/gstreamer_runtime.h"
#include "media/pipeline_thread.h"

#include <cmath>
#include <malloc.h>
#include <string_view>

namespace {
  using luciole::SeekMode;
  using luciole::media::Pipeline;

  // playbin flags: video, audio and software volume. No subtitles, no visualizations.
  constexpr guint kPlayFlags = 0x1U | 0x2U | 0x10U;

  void fail(const std::shared_ptr<Pipeline>& pipeline, std::string_view text) {
    luciole::media::runOnMainContext([pipeline, text = std::string(text)] {
      if (const auto heard = pipeline->listener.failure) {
        heard(text);
      }
    });
  }

  gboolean busMessage(GstBus* /*bus*/, GstMessage* message, gpointer data) {
    const auto& pipeline = *static_cast<std::shared_ptr<Pipeline>*>(data);
    if (const auto heard = pipeline->listener.message) {
      heard(message);
    }
    return G_SOURCE_CONTINUE;
  }

  // A flushing seek that the main context hears of only through the settled pipeline, or through seekFailed.
  void seekTo(const std::shared_ptr<Pipeline>& pipeline, GstElement* element, double seconds, SeekMode mode) {
    const auto precision = mode == SeekMode::Exact
        ? GST_SEEK_FLAG_ACCURATE
        : static_cast<GstSeekFlags>(GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST);
    const auto flags = static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | precision);
    const auto at = static_cast<gint64>(std::llround(seconds * static_cast<double>(GST_SECOND)));
    if (gst_element_seek_simple(element, GST_FORMAT_TIME, flags, at) == FALSE) {
      luciole::media::runOnMainContext([pipeline] {
        if (const auto heard = pipeline->listener.seekFailed) {
          heard();
        }
      });
    }
  }

  // A paused pipeline whose frame size changed needs its shown frame decoded again at the new size. The player does
  // that, as a seek of its own, so it waits behind any seek in flight and its completion is one the player asked for.
  // True when nothing is left to ask; false while the pipeline is still settling, or until the player's seek goes out.
  bool refit(const std::shared_ptr<Pipeline>& pipeline, GstElement* element) {
    GstState current = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    if (gst_element_get_state(element, &current, &pending, 0) != GST_STATE_CHANGE_SUCCESS
        || current != GST_STATE_PAUSED
        || pending != GST_STATE_VOID_PENDING) {
      return false;
    }
    if (!pipeline->video) {
      return true;
    }
    luciole::media::runOnMainContext([pipeline] {
      if (const auto heard = pipeline->listener.refit) {
        heard();
      }
    });
    return false;
  }

  // Pipeline thread: removes the tap's probe from the sound pad.
  void dropTap(Pipeline& pipeline) {
    if (pipeline.tapProbe != 0) {
      gst_pad_remove_probe(pipeline.soundPad, std::exchange(pipeline.tapProbe, 0));
    }
    pipeline.tapped.reset();
  }

  // Pipeline thread: moves the probe on the sound pad to the tap the main context wants.
  void syncTap(Pipeline& pipeline, const std::shared_ptr<luciole::media::AudioTap>& wanted) {
    if (wanted == pipeline.tapped || pipeline.soundPad == nullptr) {
      return;
    }
    dropTap(pipeline);
    if (wanted) {
      pipeline.tapProbe = luciole::media::attachAudioTap(pipeline.soundPad, wanted);
      pipeline.tapped = wanted;
    }
  }

  // Pipeline thread: applies what the main context asked for last. The state is set only when it changes, so a
  // state change in progress is not asked for again.
  void sync(const std::shared_ptr<Pipeline>& pipeline) {
    GstElement* element = nullptr;
    GstState target = GST_STATE_PAUSED;
    std::optional<std::pair<double, SeekMode>> seek;
    double volume = 1;
    gboolean muted = FALSE;
    std::shared_ptr<luciole::media::AudioTap> tap;
    {
      const std::scoped_lock lock(pipeline->lock);
      pipeline->syncPosted = false;
      if (pipeline->element == nullptr) {
        return;
      }
      element = GST_ELEMENT(gst_object_ref(pipeline->element));
      target = pipeline->target;
      seek = std::exchange(pipeline->seek, std::nullopt);
      volume = pipeline->volume;
      muted = static_cast<gboolean>(pipeline->muted);
      tap = pipeline->tap;
    }
    // Outside the lock: the streaming threads take it while GStreamer holds its own locks.
    g_object_set(element, "volume", volume, "mute", muted, nullptr);
    syncTap(*pipeline, tap);
    if (luciole::media::fitFrames(*pipeline)) {
      pipeline->refitWanted = true;
    }
    if (target == GST_STATE_PLAYING) {
      // Playing frames follow the new size by themselves.
      pipeline->refitWanted = false;
    }
    if (seek) {
      // The seek decodes its frame at the new size.
      pipeline->refitWanted = false;
      seekTo(pipeline, element, seek->first, seek->second);
    }
    if (target != pipeline->asked) {
      pipeline->asked = target;
      if (gst_element_set_state(element, target) == GST_STATE_CHANGE_FAILURE) {
        fail(pipeline, luciole::media::kCannotPlay);
      }
    }
    // After the state change: a resize and a pause together reach here with the pause only just asked, and no message
    // follows it that would ask again.
    if (!seek && pipeline->refitWanted && target == GST_STATE_PAUSED) {
      pipeline->refitWanted = !refit(pipeline, element);
    }
    gst_object_unref(element);
  }

  // Pipeline thread: makes the playbin with its frame and sound sinks, or null, and the pad the tap watches: the
  // audio filter's output, which comes before the volume, so volume and mute do not reach the tap.
  GstElement* makePlaybin(const std::shared_ptr<Pipeline>& pipeline, GstElement*& filter, GstPad*& soundPad) {
    GstElement* frames = luciole::media::frameSink(pipeline, filter);
    GstElement* playbin = gst_element_factory_make("playbin", nullptr);
    GstElement* tapPoint = gst_element_factory_make("identity", "sound-tap");
    const auto sound = luciole::media::openAudioSink(pipeline->audio);
    if (frames == nullptr || playbin == nullptr || tapPoint == nullptr || sound.element == nullptr) {
      for (GstElement* element : {frames, playbin, tapPoint}) {
        if (element != nullptr) {
          gst_object_unref(gst_object_ref_sink(element));
        }
      }
      if (sound.element != nullptr) {
        (void)gst_element_set_state(sound.element, GST_STATE_NULL);
        gst_object_unref(sound.element);
      }
      return nullptr;
    }
    pipeline->soundMissing = sound.deviceMissing;
    g_object_set(tapPoint, "signal-handoffs", FALSE, nullptr);
    soundPad = gst_element_get_static_pad(tapPoint, "src");
    gst_object_ref_sink(playbin);
    g_object_set(
        playbin, "uri", pipeline->uri.c_str(), "flags", kPlayFlags, "video-sink", frames, "audio-sink", sound.element,
        "audio-filter", tapPoint, nullptr
    );
    gst_object_unref(sound.element);
    luciole::media::refuseNetwork(playbin);
    return playbin;
  }

  // Pipeline thread: makes the pipeline and starts it toward the requested state.
  void build(const std::shared_ptr<Pipeline>& pipeline) {
    if (const auto& problem = luciole::media::startProblem(); !problem.empty()) {
      fail(pipeline, problem);
      return;
    }
    if (const auto problem = luciole::media::fileProblem(pipeline->path); !problem.empty()) {
      fail(pipeline, problem);
      return;
    }
    GstElement* filter = nullptr;
    GstPad* soundPad = nullptr;
    GstElement* playbin = makePlaybin(pipeline, filter, soundPad);
    if (playbin == nullptr) {
      fail(pipeline, luciole::media::kCannotPlay);
      return;
    }
    GstBus* bus = gst_element_get_bus(playbin);
    (void)gst_bus_add_watch_full(
        bus, G_PRIORITY_DEFAULT, busMessage, new std::shared_ptr<Pipeline>(pipeline),
        [](gpointer data) { delete static_cast<std::shared_ptr<Pipeline>*>(data); }
    );
    {
      const std::scoped_lock lock(pipeline->lock);
      if (!pipeline->closed) {
        pipeline->element = std::exchange(playbin, nullptr);
        pipeline->filter = filter;
      }
    }
    if (playbin != nullptr) {
      gst_bus_remove_watch(bus);
      gst_object_unref(playbin);
    } else {
      pipeline->soundPad = std::exchange(soundPad, nullptr);
    }
    if (soundPad != nullptr) {
      gst_object_unref(soundPad);
    }
    gst_object_unref(bus);
    sync(pipeline);
  }

  // Pipeline thread: stops the pipeline, frees it with its frames and returns the freed heap to the system.
  void teardown(const std::shared_ptr<Pipeline>& pipeline) {
    GstElement* element = nullptr;
    GstSample* latest = nullptr;
    {
      const std::scoped_lock lock(pipeline->lock);
      element = std::exchange(pipeline->element, nullptr);
      pipeline->filter = nullptr;
      latest = std::exchange(pipeline->latest, nullptr);
      pipeline->tap.reset();
    }
    if (latest != nullptr) {
      gst_sample_unref(latest);
    }
    if (pipeline->soundPad != nullptr) {
      dropTap(*pipeline);
      gst_object_unref(std::exchange(pipeline->soundPad, nullptr));
    }
    if (element == nullptr) {
      return;
    }
    GstBus* bus = gst_element_get_bus(element);
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);
    (void)gst_element_set_state(element, GST_STATE_NULL);
    gst_object_unref(element);
    tracePlayback(pipeline->stats);
    // The streaming threads freed into heaps of their own, which only a trim gives back.
    (void)::malloc_trim(0);
  }
} // namespace

luciole::media::Pipeline::~Pipeline() {
  if (latest != nullptr) {
    gst_sample_unref(latest);
  }
  if (soundPad != nullptr) {
    gst_object_unref(soundPad);
  }
}

void luciole::media::start(const std::shared_ptr<Pipeline>& pipeline) {
  runOnPipelineThread([pipeline] { build(pipeline); });
}

void luciole::media::request(const std::shared_ptr<Pipeline>& pipeline, const std::function<void(Pipeline&)>& change) {
  bool post = false;
  {
    const std::scoped_lock lock(pipeline->lock);
    change(*pipeline);
    post = !std::exchange(pipeline->syncPosted, true);
  }
  if (post) {
    runOnPipelineThread([pipeline] { sync(pipeline); });
  }
}

void luciole::media::stop(const std::shared_ptr<Pipeline>& pipeline) {
  pipeline->listener = {};
  {
    const std::scoped_lock lock(pipeline->lock);
    pipeline->closed = true;
  }
  runOnPipelineThread([pipeline] { teardown(pipeline); });
}

GstElement* luciole::media::elementRef(Pipeline& pipeline) {
  const std::scoped_lock lock(pipeline.lock);
  return pipeline.element != nullptr ? GST_ELEMENT(gst_object_ref(pipeline.element)) : nullptr;
}

bool luciole::media::reopensInPlace(Pipeline& pipeline) {
  GstElement* element = elementRef(pipeline);
  if (element == nullptr || !GST_IS_BIN(element)) {
    if (element != nullptr) {
      gst_object_unref(element);
    }
    return true;
  }
  bool bink = false;
  GstIterator* items = gst_bin_iterate_recurse(GST_BIN(element));
  GValue value = G_VALUE_INIT;
  // A pipeline holds a few dozen elements; the count bounds the walk if the iterator keeps resyncing.
  for (int step = 0; step < 4096 && !bink; ++step) {
    const GstIteratorResult next = gst_iterator_next(items, &value);
    if (next == GST_ITERATOR_RESYNC) {
      gst_iterator_resync(items);
    } else if (next != GST_ITERATOR_OK) {
      break;
    } else {
      GstElementFactory* factory = gst_element_get_factory(GST_ELEMENT(g_value_get_object(&value)));
      bink = factory != nullptr && std::string_view(GST_OBJECT_NAME(factory)) == bink2::kDemuxName;
      g_value_reset(&value);
    }
  }
  g_value_unset(&value);
  gst_iterator_free(items);
  gst_object_unref(element);
  return !bink;
}

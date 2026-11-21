#pragma once

#include "media/audio_tap.h"
#include "media/media_player.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <gst/gst.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace luciole::media {
  // One playbin pipeline, shared by the main context, the pipeline thread and GStreamer's streaming threads. A
  // single pipeline thread builds, changes and frees every pipeline in order, so the main context never waits on
  // GStreamer or on the disk; it only reads and posts requests.
  struct Pipeline {
    // Set before start().
    std::string path;
    std::string uri;
    AudioOutput audio = AudioOutput::System;

    std::mutex lock;
    // Serializes fitting, so the caps filter ends at the latest size while no other lock is held.
    std::mutex fitLock;
    // Guarded by `lock`.
    GstElement* element = nullptr; // owned
    GstElement* filter = nullptr;  // the caps filter that sets the frame size
    GstSample* latest = nullptr;   // the newest frame, owned
    GstState target = GST_STATE_PAUSED;
    std::optional<std::pair<double, SeekMode>> seek;
    double volume = 1;
    bool muted = false;
    int boxWidth = 0;
    int boxHeight = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int parN = 1;
    int parD = 1;
    std::pair<int, int> applied{-1, -1};
    std::shared_ptr<AudioTap> tap; // the tap the main context wants, or null
    bool syncPosted = false;
    bool closed = false;

    // Pipeline thread only: the state last asked of the element, and the pad before the volume with the tap watching
    // it.
    GstState asked = GST_STATE_VOID_PENDING;
    GstPad* soundPad = nullptr; // owned
    std::shared_ptr<AudioTap> tapped;
    gulong tapProbe = 0;

    std::atomic<bool> framePosted{false};
    std::atomic<bool> video{false};          // a video stream reached the frame sink
    std::atomic<bool> soundMissing{false};   // the system's output did not open, so sound plays nowhere
    std::atomic<bool> refitWanted{false};    // the frame size changed while paused and its frame was not decoded again
    std::atomic<std::int64_t> frameTime{-1}; // stream time in nanoseconds of the newest frame; -1 before the first

    // Main context only: what the owner hears, each called through a copy so it may stop the pipeline. stop()
    // clears it, so nothing arrives after that.
    struct Listener {
      std::function<void(GstMessage*)> message;
      std::function<void()> frame;
      std::function<void(std::string)> failure;
      std::function<void()> seekFailed;
      std::function<void()> refit; // paused and settled with a frame at an old size
    } listener;

    Pipeline() = default;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    ~Pipeline();
  };

  // Builds the pipeline on the pipeline thread and starts it toward its target state.
  void start(const std::shared_ptr<Pipeline>& pipeline);
  // Changes requested fields under the lock; one sync applies the latest of them on the pipeline thread.
  void request(const std::shared_ptr<Pipeline>& pipeline, const std::function<void(Pipeline&)>& change);
  // Silences the listener, then stops and frees the pipeline on the pipeline thread.
  void stop(const std::shared_ptr<Pipeline>& pipeline);
  // The newest frame, once.
  std::optional<MediaFrame> takeFrame(Pipeline& pipeline);
  // A new reference to the pipeline element for queries, or null before it is built and after it is freed.
  GstElement* elementRef(Pipeline& pipeline);

  // Whether the file can be opened again at the same place without a visible wait: false while a Bink 2 demuxer is in
  // the pipeline, whose exact seek decodes forward from a key frame.
  bool reopensInPlace(Pipeline& pipeline);

  // The video branch: scale and convert to RGBA at the fitted size, into a sink that queues one frame and drops
  // older ones. Returns a floating bin and its caps filter, or null.
  GstElement* frameSink(const std::shared_ptr<Pipeline>& pipeline, GstElement*& filter);
  // Sets the caps filter to the fitted size; whether that size changed. Any thread.
  bool fitFrames(Pipeline& pipeline);
} // namespace luciole::media

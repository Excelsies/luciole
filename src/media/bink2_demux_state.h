#pragma once

#include "media/bink2_pull_reader.h"
#include "media/bink2_stream.h"

#include <atomic>
#include <cstdint>
#include <gst/base/gstflowcombiner.h>
#include <gst/gst.h>
#include <memory>
#include <mutex>
#include <vector>

// The demuxer element's instance. GObject allocates it without running constructors, so `impl` is made and freed by
// the element's own init and finalize.
namespace luciole::media::bink2 {
  struct Demux;
}

struct LucioleBink2Demux {
  GstElement parent;
  luciole::media::bink2::Demux* impl;
};

namespace luciole::media::bink2 {
  // The sound of one track on its way: the samples of the file before the next buffer, and whether its stream ended.
  struct SoundClock {
    std::uint64_t samples = 0;
    std::int64_t lastFrame = -1; // the last frame with sound, or -1
    bool discont = true;         // the next buffer follows a jump
    bool ended = false;          // its end of stream was sent
  };

  struct Demux {
    GstElement* element = nullptr;
    GstPad* sink = nullptr;
    GstPad* video = nullptr;
    std::vector<GstPad*> sound; // owned by the element, one per track
    GstFlowCombiner* combiner = nullptr;
    PullReader reader;

    // Streaming thread, under the sink pad's stream lock.
    std::unique_ptr<Stream> stream;
    std::atomic<GstBufferPool*> pool{nullptr}; // also read, only to flush it, while the element goes down
    GstBuffer* before = nullptr; // the picture of frame `next - 1`, owned, kept as the reference for the next frame
    std::uint64_t next = 0;      // the frame to decode next
    std::uint64_t target = 0;    // the first frame to hand on; the ones before it only lead up to it
    bool again = false;          // hand `before` on once more instead of decoding
    bool discont = true;         // the next buffer follows a jump
    bool segmentPending = true;
    std::vector<SoundClock> clocks;
    std::uint64_t audioNext = 0; // the next frame whose sound is not decoded; at most `next`
    GstClockTime soundStart = 0; // sound that ends before this is not handed on

    // Read from any thread.
    std::mutex lock;
    // Held by a seek from its start and by releaseStream, so a seek never meets the stream while it is freed.
    std::mutex streamLock;
    GstSegment segment;                                      // guarded by `lock`
    std::uint32_t seqnum = 0;                                // of the seek that made `segment`; guarded by `lock`
    std::atomic<GstClockTime> duration{GST_CLOCK_TIME_NONE}; // set once the header is read
    std::atomic<GstClockTime> position{GST_CLOCK_TIME_NONE}; // the newest frame handed on
    std::atomic<bool> seekable{false};

    explicit Demux(GstElement* owner, GstPad* sinkPad) : element(owner), sink(sinkPad), reader(sinkPad) {
      gst_segment_init(&segment, GST_FORMAT_TIME);
      combiner = gst_flow_combiner_new();
    }
    ~Demux() {
      // Normally released when the element left PAUSED; an element freed without that frees them here.
      if (before != nullptr) {
        gst_buffer_unref(before);
      }
      if (GstBufferPool* held = pool.exchange(nullptr)) {
        (void)gst_buffer_pool_set_active(held, FALSE);
        gst_object_unref(held);
      }
      gst_flow_combiner_free(combiner);
    }
    Demux(const Demux&) = delete;
    Demux& operator=(const Demux&) = delete;
  };

  // The debug category of the element: GST_DEBUG=luciole-bink2:5.
  GST_DEBUG_CATEGORY_EXTERN(bink2Debug);
  // The instance's state, from the object GStreamer hands to the pad functions.
  inline Demux& demuxOf(GstObject* object) { return *reinterpret_cast<LucioleBink2Demux*>(object)->impl; }
  // The function the sink pad's task runs: one streaming pass; `data` is the Demux.
  void runStreamingTask(gpointer data);
  // The streaming task: one pass reads the header, or decodes and hands on one frame, or ends the stream.
  void streamingPass(Demux& demux);
  // The queries and events the source pads answer: duration, position and seeking; seeks.
  gboolean sourceQuery(GstPad* pad, GstObject* parent, GstQuery* query);
  gboolean sourceEvent(GstPad* pad, GstObject* parent, GstEvent* event);
  // A seek that came upstream through a source pad; whether it was done.
  bool seek(Demux& demux, GstEvent* event);
  // Stops the stream and frees what it holds, so the element may start again. The task must be stopped.
  void releaseStream(Demux& demux);
  // The caps of the pictures, and of one sound track.
  GstCaps* videoCaps(const Info& info);
  GstCaps* soundCaps(const AudioTrack& track);

  // Frame numbers and times, from the file's frame rate.
  inline GstClockTime frameTime(const Info& info, std::uint64_t frame) {
    return gst_util_uint64_scale(frame * info.frameRateDenominator, GST_SECOND, info.frameRateNumerator);
  }
  // The frame showing at `time`.
  inline std::uint64_t frameAt(const Info& info, GstClockTime time) {
    return gst_util_uint64_scale(
        time, info.frameRateNumerator, static_cast<std::uint64_t>(info.frameRateDenominator) * GST_SECOND
    );
  }
} // namespace luciole::media::bink2

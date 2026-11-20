#include "media/bink2_demux_state.h"

#include <algorithm>
#include <utility>

namespace {
  using namespace luciole::media::bink2;

  // The key frames around `frame`: the last at or before it, and the first at or after it (frames, when none).
  struct Around {
    std::uint64_t before = 0;
    std::uint64_t after = 0;
  };

  Around keysAround(const Stream& stream, std::uint64_t frame) {
    const auto& keys = stream.keyFrames();
    const auto it = std::ranges::lower_bound(keys, frame);
    Around around{.before = 0, .after = stream.info().frames};
    if (it != keys.end()) {
      around.after = *it;
    }
    if (it != keys.end() && *it == frame) {
      around.before = frame;
    } else if (it != keys.begin()) {
      around.before = *std::prev(it);
    }
    return around;
  }

  // Which frame a key frame seek lands on: the key frame the flags and the distance pick.
  std::uint64_t snapped(const Around& around, std::uint64_t frame, GstSeekFlags flags, std::uint64_t frames) {
    const bool wantBefore = (flags & GST_SEEK_FLAG_SNAP_BEFORE) != 0;
    const bool wantAfter = (flags & GST_SEEK_FLAG_SNAP_AFTER) != 0;
    if (wantBefore && !wantAfter) {
      return around.before;
    }
    if (wantAfter && !wantBefore) {
      return around.after <= frames ? around.after : around.before;
    }
    if (around.after >= frames) {
      return around.before;
    }
    const bool nearestIsAfter = (flags & GST_SEEK_FLAG_SNAP_NEAREST) == GST_SEEK_FLAG_SNAP_NEAREST
        && around.after - frame < frame - around.before;
    return nearestIsAfter ? around.after : around.before;
  }

  // Sends flush-start to every pad, so a streaming pass stuck in a pull or a push comes back.
  void startFlush(Demux& demux) {
    GstEvent* event = gst_event_new_flush_start();
    for (GstPad* pad : demux.sound) {
      (void)gst_pad_push_event(pad, gst_event_ref(event));
    }
    (void)gst_pad_push_event(demux.video, gst_event_ref(event));
    // The file's side, so a pull in flight returns.
    (void)gst_pad_push_event(demux.sink, event);
    gst_buffer_pool_set_flushing(demux.pool.load(), TRUE);
  }

  void stopFlush(Demux& demux, guint32 seqnum) {
    gst_buffer_pool_set_flushing(demux.pool.load(), FALSE);
    const auto stop = [seqnum] {
      GstEvent* event = gst_event_new_flush_stop(TRUE);
      gst_event_set_seqnum(event, seqnum);
      return event;
    };
    (void)gst_pad_push_event(demux.sink, stop());
    for (GstPad* pad : demux.sound) {
      (void)gst_pad_push_event(pad, stop());
    }
    (void)gst_pad_push_event(demux.video, stop());
    gst_flow_combiner_reset(demux.combiner);
    demux.reader.clearFailure();
  }

  // The frame the sound must be decoded from to have it at `time`: for each track, the last frame with sound before the
  // frame that holds the samples of that time, so the block there is mixed with the block before it as in a file
  // played through, however many frames without sound lie between; the earliest of those, and `from`, the picture's
  // frame, when the sound comes later.
  std::uint64_t soundStartFrame(const Demux& demux, GstClockTime time, std::uint64_t from) {
    std::uint64_t start = from;
    const auto& tracks = demux.stream->info().audio;
    for (std::size_t track = 0; track < tracks.size(); ++track) {
      const int index = static_cast<int>(track);
      const auto sample = gst_util_uint64_scale(time, tracks[track].rate, GST_SECOND);
      const auto frame = demux.stream->soundFrameAt(index, sample);
      const auto before = demux.stream->soundBefore(index, frame);
      const std::uint64_t previous = before > 0 ? demux.stream->soundFrameAt(index, before - 1) : 0;
      start = std::min({start, previous, frame});
    }
    return start;
  }

  // What a seek to `target` needs of the decoder: the frame it starts decoding from and whether the frame before
  // the target, still held, is already the right picture. A file with sound always starts again, since the sound
  // before the target that was decoded is gone with the flush.
  void plan(Demux& demux, std::uint64_t target, std::uint64_t from, GstClockTime time) {
    const auto& info = demux.stream->info();
    const bool held = demux.before != nullptr && demux.next > 0 && target < info.frames && info.audio.empty();
    demux.target = target;
    demux.again = false;
    demux.soundStart = time;
    if (held && target + 1 == demux.next) {
      demux.again = true; // the frame at hand
      return;
    }
    if (held && demux.next <= target && keysAround(*demux.stream, target).before < demux.next) {
      return; // the same run of frames: go on forward
    }
    if (demux.before != nullptr) {
      gst_buffer_unref(std::exchange(demux.before, nullptr));
    }
    demux.next = from;
    if (info.audio.empty()) {
      return;
    }
    const std::uint64_t soundFrom = soundStartFrame(demux, time, from);
    demux.stream->resetAudio();
    demux.audioNext = soundFrom;
    for (std::size_t track = 0; track < demux.clocks.size(); ++track) {
      demux.clocks[track].samples = demux.stream->soundBefore(static_cast<int>(track), soundFrom);
      demux.clocks[track].discont = true;
      demux.clocks[track].ended = false;
    }
  }
} // namespace

gboolean luciole::media::bink2::sourceQuery(GstPad* pad, GstObject* parent, GstQuery* query) {
  auto& demux = demuxOf(parent);
  switch (GST_QUERY_TYPE(query)) {
  case GST_QUERY_DURATION: {
    GstFormat format = GST_FORMAT_UNDEFINED;
    gst_query_parse_duration(query, &format, nullptr);
    const GstClockTime duration = demux.duration;
    if (format != GST_FORMAT_TIME || !GST_CLOCK_TIME_IS_VALID(duration)) {
      return FALSE;
    }
    gst_query_set_duration(query, GST_FORMAT_TIME, static_cast<gint64>(duration));
    return TRUE;
  }
  case GST_QUERY_POSITION: {
    GstFormat format = GST_FORMAT_UNDEFINED;
    gst_query_parse_position(query, &format, nullptr);
    const GstClockTime position = demux.position;
    if (format != GST_FORMAT_TIME || !GST_CLOCK_TIME_IS_VALID(position)) {
      return FALSE;
    }
    gst_query_set_position(query, GST_FORMAT_TIME, static_cast<gint64>(position));
    return TRUE;
  }
  case GST_QUERY_SEEKING: {
    GstFormat format = GST_FORMAT_UNDEFINED;
    gst_query_parse_seeking(query, &format, nullptr, nullptr, nullptr);
    const GstClockTime duration = demux.duration;
    if (format != GST_FORMAT_TIME || !demux.seekable) {
      return FALSE;
    }
    gst_query_set_seeking(query, GST_FORMAT_TIME, TRUE, 0, static_cast<gint64>(duration));
    return TRUE;
  }
  default:
    return gst_pad_query_default(pad, parent, query);
  }
}

gboolean luciole::media::bink2::sourceEvent(GstPad* pad, GstObject* parent, GstEvent* event) {
  auto& demux = demuxOf(parent);
  switch (GST_EVENT_TYPE(event)) {
  case GST_EVENT_SEEK: {
    const bool done = seek(demux, event);
    gst_event_unref(event);
    return done ? TRUE : FALSE;
  }
  case GST_EVENT_QOS:
  case GST_EVENT_LATENCY:
    // Every frame is decoded; a sink that is late drops it itself.
    gst_event_unref(event);
    return TRUE;
  default:
    return gst_pad_event_default(pad, parent, event);
  }
}

bool luciole::media::bink2::seek(Demux& demux, GstEvent* event) {
  gdouble rate = 1.0;
  GstFormat format = GST_FORMAT_UNDEFINED;
  GstSeekFlags flags = GST_SEEK_FLAG_NONE;
  GstSeekType startType = GST_SEEK_TYPE_NONE;
  GstSeekType stopType = GST_SEEK_TYPE_NONE;
  gint64 start = 0;
  gint64 stop = 0;
  gst_event_parse_seek(event, &rate, &format, &flags, &startType, &start, &stopType, &stop);
  // Only the seeks a player makes: forward at normal speed, flushing, by time.
  const std::scoped_lock streaming(demux.streamLock);
  if (!demux.seekable
      || demux.video == nullptr
      || demux.pool.load() == nullptr
      || format != GST_FORMAT_TIME
      || rate != 1.0
      || (flags & GST_SEEK_FLAG_FLUSH) == 0
      || (flags & (GST_SEEK_FLAG_SEGMENT | GST_SEEK_FLAG_TRICKMODE)) != 0) {
    return false;
  }
  const guint32 seqnum = gst_event_get_seqnum(event);
  startFlush(demux);
  gst_pad_pause_task(demux.sink);
  GST_PAD_STREAM_LOCK(demux.sink);
  const auto& info = demux.stream->info();
  GstSegment segment;
  {
    const std::scoped_lock lock(demux.lock);
    segment = demux.segment;
  }
  gboolean update = FALSE;
  gst_segment_do_seek(&segment, rate, GST_FORMAT_TIME, flags, startType, start, stopType, stop, &update);
  std::uint64_t frame = segment.start >= segment.duration ? info.frames : frameAt(info, segment.start);
  frame = std::min(frame, info.frames);
  std::uint64_t from = info.frames;
  if (frame < info.frames) {
    const auto around = keysAround(*demux.stream, frame);
    if ((flags & GST_SEEK_FLAG_KEY_UNIT) != 0) {
      frame = snapped(around, frame, flags, info.frames);
      from = frame;
    } else {
      from = around.before;
    }
  }
  // The segment starts on the picture the seek lands on, so its time is that picture's.
  const GstClockTime landed = frame < info.frames ? frameTime(info, frame) : segment.duration;
  segment.start = landed;
  segment.time = landed;
  segment.position = landed;
  plan(demux, frame, from, landed);
  GST_CAT_DEBUG_OBJECT(
      bink2Debug, demux.element,
      "seek to frame %" G_GUINT64_FORMAT " from %" G_GUINT64_FORMAT ", next %" G_GUINT64_FORMAT " again %d", frame,
      from, demux.next, demux.again ? 1 : 0
  );
  {
    const std::scoped_lock lock(demux.lock);
    demux.segment = segment;
    demux.seqnum = seqnum;
  }
  demux.position = landed;
  demux.segmentPending = true;
  demux.discont = true;
  stopFlush(demux, seqnum);
  GST_PAD_STREAM_UNLOCK(demux.sink);
  // A down-transition may have stopped the pads meanwhile: a task is only started on a pad that is still active.
  if (!demux.stream || demux.video == nullptr || !GST_PAD_IS_ACTIVE(demux.sink) || GST_PAD_IS_FLUSHING(demux.sink)) {
    return false;
  }
  return gst_pad_start_task(demux.sink, runStreamingTask, &demux, nullptr) != FALSE;
}

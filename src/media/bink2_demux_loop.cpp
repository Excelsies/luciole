#include "media/bink2_demux.h"
#include "media/bink2_demux_state.h"

#include <algorithm>
#include <format>
#include <gst/audio/audio.h>
#include <gst/video/video.h>
#include <string_view>
#include <utility>

namespace {
  using namespace luciole::media::bink2;

  constexpr int kPoolMinimum = 2;

  void postError(Demux& demux, gint code, const std::string& text, const char* debug = nullptr) {
    gst_element_message_full(
        demux.element, GST_MESSAGE_ERROR, GST_STREAM_ERROR, code, g_strdup(text.c_str()),
        debug != nullptr ? g_strdup(debug) : nullptr, __FILE__, "streamingPass", __LINE__
    );
  }

  // Pushes `event` to every source pad, each with its own reference; a sound pad that ended takes no more.
  void pushToAll(Demux& demux, GstEvent* event) {
    for (std::size_t track = 0; track < demux.sound.size(); ++track) {
      if (!demux.clocks[track].ended || GST_EVENT_TYPE(event) != GST_EVENT_EOS) {
        (void)gst_pad_push_event(demux.sound[track], gst_event_ref(event));
      }
    }
    if (demux.video != nullptr) {
      (void)gst_pad_push_event(demux.video, gst_event_ref(event));
    }
    gst_event_unref(event);
  }

  // Ends the task for a flow result that is not OK: a flush and an end are quiet; a pad nobody listens on or a
  // failure is an error, and the streams end. A plain error was posted by whoever returned it.
  void endOn(Demux& demux, GstFlowReturn flow) {
    GST_CAT_DEBUG_OBJECT(bink2Debug, demux.element, "ending the task on %s", gst_flow_get_name(flow));
    gst_pad_pause_task(demux.sink);
    if (flow == GST_FLOW_EOS || flow == GST_FLOW_FLUSHING) {
      return;
    }
    if (flow == GST_FLOW_NOT_LINKED || (flow < GST_FLOW_EOS && flow != GST_FLOW_ERROR)) {
      postError(demux, GST_STREAM_ERROR_FAILED, "Internal data stream error.", gst_flow_get_name(flow));
      pushToAll(demux, gst_event_new_eos());
    }
  }

  void endSound(Demux& demux);

  // The pull that failed ended the pass: a flush is the seek's business and stays quiet; anything else, the end of
  // the file included, means the file ended early or changed after it was opened.
  void readFailed(Demux& demux) {
    const GstFlowReturn failure = std::exchange(demux.reader.failureRef(), GST_FLOW_OK);
    if (failure == GST_FLOW_FLUSHING) {
      endOn(demux, GST_FLOW_FLUSHING);
      return;
    }
    postError(demux, GST_STREAM_ERROR_DECODE, "The video file ended early or changed.", gst_flow_get_name(failure));
    endOn(demux, GST_FLOW_ERROR);
  }

  // A stream the pads of which are made: one picture pad and a pad for each sound track.
  bool addPads(Demux& demux) {
    const auto& info = demux.stream->info();
    const guint group = gst_util_group_id_next();
    const auto expose = [&](const char* name, GstCaps* caps, const char* id) -> GstPad* {
      GstElementClass* klass = GST_ELEMENT_GET_CLASS(demux.element);
      const char* family = std::string_view(name).starts_with("video") ? "video" : "audio_%u";
      GstPad* pad = gst_pad_new_from_template(gst_element_class_get_pad_template(klass, family), name);
      if (pad == nullptr) {
        gst_caps_unref(caps);
        return nullptr;
      }
      gst_pad_set_query_function(pad, [](GstPad* source, GstObject* parent, GstQuery* query) -> gboolean {
        return luciole::media::bink2::sourceQuery(source, parent, query);
      });
      gst_pad_set_event_function(pad, [](GstPad* source, GstObject* parent, GstEvent* event) -> gboolean {
        return luciole::media::bink2::sourceEvent(source, parent, event);
      });
      gst_pad_use_fixed_caps(pad);
      gst_pad_set_active(pad, TRUE);
      gchar* streamId = gst_pad_create_stream_id(pad, demux.element, id);
      GstEvent* start = gst_event_new_stream_start(streamId);
      gst_event_set_group_id(start, group);
      g_free(streamId);
      (void)gst_pad_push_event(pad, start);
      (void)gst_pad_push_event(pad, gst_event_new_caps(caps));
      gst_caps_unref(caps);
      (void)gst_element_add_pad(demux.element, pad);
      gst_flow_combiner_add_pad(demux.combiner, pad);
      return pad;
    };
    demux.video = expose("video_0", videoCaps(info), "video0");
    bool all = demux.video != nullptr;
    for (std::size_t track = 0; track < info.audio.size() && all; ++track) {
      GstPad* pad = expose(std::format("audio_{}", track).c_str(), soundCaps(info.audio[track]), "audio");
      all = pad != nullptr;
      if (all) {
        demux.sound.push_back(pad);
      }
    }
    return all;
  }

  GstBufferPool* makePool(const Info& info, const Layout& layout) {
    GstBufferPool* pool = gst_buffer_pool_new();
    GstCaps* caps = videoCaps(info);
    GstStructure* config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, caps, static_cast<guint>(layout.total), kPoolMinimum, kPoolMaximum);
    gst_caps_unref(caps);
    if (gst_buffer_pool_set_config(pool, config) == FALSE || gst_buffer_pool_set_active(pool, TRUE) == FALSE) {
      gst_object_unref(pool);
      return nullptr;
    }
    return pool;
  }

  // Pipeline thread of the streaming task: reads the header, makes the pads. False when the task ended.
  bool startStream(Demux& demux) {
    gint64 size = 0;
    if (gst_pad_peer_query_duration(demux.sink, GST_FORMAT_BYTES, &size) == FALSE || size <= 0) {
      postError(demux, GST_STREAM_ERROR_TYPE_NOT_FOUND, "The size of the video file is not known.");
      gst_pad_pause_task(demux.sink);
      return false;
    }
    auto opened = Stream::open(
        [&demux](std::uint64_t offset, void* destination, std::size_t count) {
          return demux.reader.read(offset, destination, count);
        },
        static_cast<std::uint64_t>(size), [&demux] { return GST_PAD_IS_FLUSHING(demux.sink) != 0; }
    );
    if (!opened) {
      if (demux.reader.failure() == GST_FLOW_FLUSHING || GST_PAD_IS_FLUSHING(demux.sink)) {
        gst_pad_pause_task(demux.sink);
      } else {
        postError(demux, GST_STREAM_ERROR_DEMUX, opened.error());
        gst_pad_pause_task(demux.sink);
      }
      return false;
    }
    demux.stream = std::move(*opened);
    const auto& info = demux.stream->info();
    demux.pool = makePool(info, demux.stream->layout());
    demux.clocks.assign(info.audio.size(), SoundClock{});
    for (std::size_t track = 0; track < info.audio.size(); ++track) {
      const auto total = demux.stream->soundBefore(static_cast<int>(track), info.frames);
      if (total > 0) {
        demux.clocks[track].lastFrame =
            static_cast<std::int64_t>(demux.stream->soundFrameAt(static_cast<int>(track), total - 1));
      }
    }
    demux.audioNext = 0;
    demux.soundStart = 0;
    // Known before the pads appear, so a handler of their appearing can ask.
    {
      const std::scoped_lock lock(demux.lock);
      demux.segment.duration = frameTime(info, info.frames);
      demux.segment.stop = GST_CLOCK_TIME_NONE;
    }
    demux.duration = frameTime(info, info.frames);
    demux.seekable = true;
    if (demux.pool == nullptr || !addPads(demux)) {
      postError(demux, GST_STREAM_ERROR_FAILED, "The video's streams could not be made.");
      gst_pad_pause_task(demux.sink);
      return false;
    }
    gst_element_no_more_pads(demux.element);
    return true;
  }

  void sendSegment(Demux& demux) {
    GstEvent* event = nullptr;
    {
      const std::scoped_lock lock(demux.lock);
      event = gst_event_new_segment(&demux.segment);
      gst_event_set_seqnum(event, demux.seqnum != 0 ? demux.seqnum : gst_util_seqnum_next());
    }
    pushToAll(demux, event);
    demux.segmentPending = false;
    endSound(demux);
  }

  // Sets the time, the flags and the layout of one decoded picture. It still has a single owner.
  void describePicture(Demux& demux, GstBuffer* picture, std::uint64_t frame, bool key) {
    const auto& info = demux.stream->info();
    const auto& layout = demux.stream->layout();
    GST_BUFFER_PTS(picture) = frameTime(info, frame);
    GST_BUFFER_DTS(picture) = GST_BUFFER_PTS(picture);
    GST_BUFFER_DURATION(picture) = frameTime(info, frame + 1) - frameTime(info, frame);
    GST_BUFFER_OFFSET(picture) = frame;
    GST_BUFFER_OFFSET_END(picture) = frame + 1;
    if (!key) {
      GST_BUFFER_FLAG_SET(picture, GST_BUFFER_FLAG_DELTA_UNIT);
    }
    gsize offsets[GST_VIDEO_MAX_PLANES] = {0, layout.size[0], layout.size[0] + layout.size[1], 0};
    gint strides[GST_VIDEO_MAX_PLANES] = {layout.stride[0], layout.stride[1], layout.stride[2], 0};
    (void)gst_buffer_add_video_meta_full(
        picture, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_I420, static_cast<guint>(info.width),
        static_cast<guint>(info.height), 3, offsets, strides
    );
  }

  GstFlowReturn handOnPicture(Demux& demux, GstBuffer* picture) {
    demux.discont = false;
    GstFlowReturn flow = gst_pad_push(demux.video, gst_buffer_ref(picture));
    flow = gst_flow_combiner_update_pad_flow(demux.combiner, demux.video, flow);
    return flow;
  }

  // Ends the stream of each track whose last sound was handled.
  void endSound(Demux& demux) {
    for (std::size_t track = 0; track < demux.sound.size(); ++track) {
      auto& clock = demux.clocks[track];
      if (!clock.ended && static_cast<std::int64_t>(demux.audioNext) > clock.lastFrame) {
        clock.ended = true;
        (void)gst_pad_push_event(demux.sound[track], gst_event_new_eos());
      }
    }
  }

  // The sound of the frame at `audioNext`, just decoded: counted, and handed on when it ends after the place the
  // segment starts at. Sound runs ahead of the pictures, so this can be a frame before the first one handed on.
  GstFlowReturn handOnSound(Demux& demux) {
    GstFlowReturn flow = GST_FLOW_OK;
    const auto& tracks = demux.stream->info().audio;
    for (std::size_t track = 0; track < tracks.size() && flow == GST_FLOW_OK; ++track) {
      const auto pcm = demux.stream->audio(static_cast<int>(track));
      const auto frameBytes = static_cast<std::size_t>(tracks[track].channels) * 2;
      auto& clock = demux.clocks[track];
      if (frameBytes == 0 || clock.ended) {
        continue;
      }
      // The frame's own chunk says how much sound it holds, and the decoder returns exactly that.
      const std::uint64_t wanted = demux.stream->soundBefore(static_cast<int>(track), demux.audioNext + 1)
          - demux.stream->soundBefore(static_cast<int>(track), demux.audioNext);
      const std::uint64_t samples = pcm.size() / frameBytes;
      if (samples != wanted) {
        postError(demux, GST_STREAM_ERROR_DECODE, std::format("The sound of frame {} is damaged.", demux.audioNext));
        return GST_FLOW_ERROR;
      }
      if (wanted == 0) {
        continue;
      }
      const GstClockTime pts = gst_util_uint64_scale(clock.samples, GST_SECOND, tracks[track].rate);
      const GstClockTime duration = gst_util_uint64_scale(samples, GST_SECOND, tracks[track].rate);
      clock.samples += samples;
      if (pts + duration <= demux.soundStart) {
        continue;
      }
      GstBuffer* buffer = gst_buffer_new_memdup(pcm.data(), samples * frameBytes);
      GST_BUFFER_PTS(buffer) = pts;
      GST_BUFFER_DURATION(buffer) = duration;
      if (clock.discont) {
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
        clock.discont = false;
      }
      flow = gst_pad_push(demux.sound[track], buffer);
      flow = gst_flow_combiner_update_pad_flow(demux.combiner, demux.sound[track], flow);
    }
    return flow;
  }

  // Decodes the sound of a frame that comes before the picture the seek starts from.
  void soundOnly(Demux& demux) {
    const auto decoded = demux.stream->decodeSound(demux.audioNext);
    if (!decoded) {
      if (demux.reader.failure() != GST_FLOW_OK) {
        readFailed(demux);
      } else {
        postError(demux, GST_STREAM_ERROR_DECODE, decoded.error());
        endOn(demux, GST_FLOW_ERROR);
      }
      return;
    }
    GstFlowReturn flow = handOnSound(demux);
    ++demux.audioNext;
    endSound(demux);
    if (flow != GST_FLOW_OK) {
      endOn(demux, flow);
    }
  }

  // Hands the picture decoded before on once more: a seek to the frame already at hand needs no decoding.
  void repeatPicture(Demux& demux) {
    demux.again = false;
    GstBuffer* copy = gst_buffer_copy_region(
        demux.before, static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_ALL & ~GST_BUFFER_COPY_DEEP), 0, -1
    );
    if (copy == nullptr) {
      postError(demux, GST_STREAM_ERROR_DECODE, "A picture of the video could not be handed on again.");
      endOn(demux, GST_FLOW_ERROR);
      return;
    }
    if (demux.discont) {
      GST_BUFFER_FLAG_SET(copy, GST_BUFFER_FLAG_DISCONT);
    }
    demux.discont = false;
    GstFlowReturn flow = gst_pad_push(demux.video, copy);
    flow = gst_flow_combiner_update_pad_flow(demux.combiner, demux.video, flow);
    if (flow != GST_FLOW_OK) {
      endOn(demux, flow);
    }
  }

  bool reachedStop(Demux& demux) {
    const auto& info = demux.stream->info();
    const std::scoped_lock lock(demux.lock);
    return GST_CLOCK_TIME_IS_VALID(demux.segment.stop) && frameTime(info, demux.next) >= demux.segment.stop;
  }

  void decodeOne(Demux& demux) {
    const std::uint64_t frame = demux.next;
    GST_CAT_TRACE_OBJECT(
        bink2Debug, demux.element, "decode frame %" G_GUINT64_FORMAT " target %" G_GUINT64_FORMAT, frame, demux.target
    );
    const bool key = std::ranges::binary_search(demux.stream->keyFrames(), static_cast<std::uint32_t>(frame));
    GstBuffer* picture = nullptr;
    GstFlowReturn flow = gst_buffer_pool_acquire_buffer(demux.pool, &picture, nullptr);
    if (flow != GST_FLOW_OK) {
      endOn(demux, flow);
      return;
    }
    GstMapInfo map;
    GstMapInfo reference;
    const bool mapped = gst_buffer_map(picture, &map, GST_MAP_WRITE) != FALSE;
    const bool referenced = demux.before != nullptr && !key && gst_buffer_map(demux.before, &reference, GST_MAP_READ);
    if (!mapped || (!key && !referenced)) {
      if (mapped) {
        gst_buffer_unmap(picture, &map);
      }
      gst_buffer_unref(picture);
      postError(demux, GST_STREAM_ERROR_DECODE, "A picture of the video could not be mapped.");
      endOn(demux, GST_FLOW_ERROR);
      return;
    }
    const auto& layout = demux.stream->layout();
    Planes planes;
    planes.plane = {map.data, map.data + layout.size[0], map.data + layout.size[0] + layout.size[1]};
    Planes earlier;
    if (referenced) {
      earlier.plane = {
          reference.data, reference.data + layout.size[0], reference.data + layout.size[0] + layout.size[1]
      };
    }
    const auto decoded = demux.stream->decode(frame, planes, referenced ? &earlier : nullptr);
    if (referenced) {
      gst_buffer_unmap(demux.before, &reference);
    }
    gst_buffer_unmap(picture, &map);
    if (!decoded) {
      gst_buffer_unref(picture);
      // A pull that failed for a flush is not the file's fault; the seek that flushed takes over.
      if (demux.reader.failure() != GST_FLOW_OK) {
        readFailed(demux);
      } else {
        postError(demux, GST_STREAM_ERROR_DECODE, decoded.error());
        endOn(demux, GST_FLOW_ERROR);
      }
      return;
    }
    // Every picture is described, as one may be handed on again by a later seek to it.
    describePicture(demux, picture, frame, key);
    const bool hand = frame >= demux.target;
    GstBuffer* handed = nullptr;
    if (hand) {
      if (demux.discont) {
        GST_BUFFER_FLAG_SET(picture, GST_BUFFER_FLAG_DISCONT);
      }
      handed = picture;
    }
    GstBuffer* old = std::exchange(demux.before, picture);
    if (old != nullptr) {
      gst_buffer_unref(old);
    }
    demux.next = frame + 1;
    flow = GST_FLOW_OK;
    if (handed != nullptr) {
      demux.position = GST_BUFFER_PTS(handed);
      flow = handOnPicture(demux, handed);
    }
    if (flow == GST_FLOW_OK && !demux.clocks.empty()) {
      flow = handOnSound(demux);
      demux.audioNext = frame + 1;
      endSound(demux);
    }
    if (flow != GST_FLOW_OK) {
      endOn(demux, flow);
    }
  }
} // namespace

void luciole::media::bink2::streamingPass(Demux& demux) {
  if (!demux.stream && !startStream(demux)) {
    return;
  }
  if (demux.segmentPending) {
    sendSegment(demux);
  }
  if (!demux.clocks.empty() && demux.audioNext < demux.next) {
    soundOnly(demux);
    return;
  }
  if (demux.again && demux.before != nullptr) {
    repeatPicture(demux);
    return;
  }
  if (demux.next >= demux.stream->info().frames || reachedStop(demux)) {
    gst_pad_pause_task(demux.sink);
    pushToAll(demux, gst_event_new_eos());
    return;
  }
  decodeOne(demux);
}

void luciole::media::bink2::releaseStream(Demux& demux) {
  const std::scoped_lock streaming(demux.streamLock);
  if (demux.before != nullptr) {
    gst_buffer_unref(std::exchange(demux.before, nullptr));
  }
  if (GstBufferPool* held = demux.pool.exchange(nullptr)) {
    (void)gst_buffer_pool_set_active(held, FALSE);
    gst_object_unref(held);
  }
  for (GstPad* pad : demux.sound) {
    gst_flow_combiner_remove_pad(demux.combiner, pad);
    (void)gst_element_remove_pad(demux.element, pad);
  }
  demux.sound.clear();
  if (demux.video != nullptr) {
    gst_flow_combiner_remove_pad(demux.combiner, demux.video);
    (void)gst_element_remove_pad(demux.element, std::exchange(demux.video, nullptr));
  }
  demux.stream.reset();
  demux.clocks.clear();
  demux.audioNext = 0;
  demux.soundStart = 0;
  demux.next = 0;
  demux.target = 0;
  demux.again = false;
  demux.discont = true;
  demux.segmentPending = true;
  demux.duration = GST_CLOCK_TIME_NONE;
  demux.position = GST_CLOCK_TIME_NONE;
  demux.seekable = false;
  demux.reader.clearFailure();
  demux.reader.forget();
  const std::scoped_lock lock(demux.lock);
  gst_segment_init(&demux.segment, GST_FORMAT_TIME);
  demux.seqnum = 0;
}

GstCaps* luciole::media::bink2::videoCaps(const Info& info) {
  GstVideoInfo video;
  gst_video_info_init(&video);
  gst_video_info_set_format(
      &video, GST_VIDEO_FORMAT_I420, static_cast<guint>(info.width), static_cast<guint>(info.height)
  );
  GST_VIDEO_INFO_FPS_N(&video) = static_cast<gint>(info.frameRateNumerator);
  GST_VIDEO_INFO_FPS_D(&video) = static_cast<gint>(info.frameRateDenominator);
  video.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;
  video.colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT601;
  // Only what the decoder states is claimed: the data are full range and its own conversion uses BT.601. The file
  // names neither the transfer nor the primaries.
  video.colorimetry.transfer = GST_VIDEO_TRANSFER_UNKNOWN;
  video.colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_UNKNOWN;
  return gst_video_info_to_caps(&video);
}

GstCaps* luciole::media::bink2::soundCaps(const AudioTrack& track) {
  GstAudioInfo sound;
  gst_audio_info_init(&sound);
  gst_audio_info_set_format(&sound, GST_AUDIO_FORMAT_S16LE, track.rate, track.channels, nullptr);
  return gst_audio_info_to_caps(&sound);
}

void luciole::media::bink2::runStreamingTask(gpointer data) {
  auto& demux = *static_cast<Demux*>(data);
  // A C++ exception must not leave a GStreamer task: it ends the stream with an error.
  try {
    streamingPass(demux);
  } catch (const std::exception& error) {
    postError(demux, GST_STREAM_ERROR_FAILED, std::format("The video could not be played: {}.", error.what()));
    gst_pad_pause_task(demux.sink);
  }
}

#include "media/audio_tap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <gst/audio/audio.h>
#include <mutex>
#include <utility>
#include <vector>

namespace {
  // Each conversion takes at most a fiftieth of a second of input, so its output stays far smaller than the ring.
  constexpr int kChunksPerSecond = 50;
  // A buffer that starts this far from where the previous one ended starts the samples again.
  constexpr GstClockTime kGapTolerance = 20 * GST_MSECOND;
  // The input the tap converts; anything else gives no samples.
  constexpr int kMinInputRate = 1000;
  constexpr int kMaxInputRate = 768000;
  constexpr int kMaxChannels = 64;
  // The tap sits before the player's one-second audio queue, so the sound it sees runs up to about 1.3 s ahead of
  // what is heard; the ring keeps three seconds.
  constexpr std::size_t kRingSeconds = 3;
} // namespace

struct luciole::media::AudioTap {
  AudioTap(int samplesPerSecond, int windowLength)
      : rate(samplesPerSecond), length(windowLength), ring(static_cast<std::size_t>(samplesPerSecond) * kRingSeconds) {}
  AudioTap(const AudioTap&) = delete;
  AudioTap& operator=(const AudioTap&) = delete;
  ~AudioTap() {
    if (converter != nullptr) {
      gst_audio_converter_free(converter);
    }
  }

  const int rate;
  const int length;

  // Guarded by `lock`: the ring holds samples `written - ring.size()` to `written`, and sample 0 is heard at the
  // stream time `anchor`, which is empty until samples arrive after a start, a flush or a gap.
  std::mutex lock;
  std::vector<float> ring;
  std::int64_t written = 0;
  std::optional<std::int64_t> anchor;
  // Changes, under the lock, each time the samples start again.
  std::atomic<std::uint64_t> generation{0};

  // Streaming thread only: buffers and serialized events reach the pad one at a time.
  GstAudioConverter* converter = nullptr;
  GstAudioInfo input{};
  GstSegment segment{};
  bool primed = false;  // the pad's caps and segment were read when the probe first ran
  bool flowing = false; // samples follow on from the last buffer
  std::uint64_t ownGeneration = 0;
  GstClockTime inputStart = 0;
  std::uint64_t inputFrames = 0;
  gsize skip = 0; // the converter's leading output, which comes before the first input sample
  std::vector<float> converted;

  // Main context only.
  std::int64_t lastEnd = 0;
  std::uint64_t lastGeneration = 0;
};

namespace {
  using luciole::media::AudioTap;

  // Drops the samples, so the window starts again. Any thread.
  void restart(AudioTap& tap) {
    const std::scoped_lock lock(tap.lock);
    tap.written = 0;
    tap.anchor.reset();
    tap.generation.fetch_add(1);
  }

  // Converts sound of `caps` to the tap's mono rate from now on; without a converter nothing is taken.
  void configure(AudioTap& tap, GstCaps* caps) {
    if (tap.converter != nullptr) {
      gst_audio_converter_free(std::exchange(tap.converter, nullptr));
    }
    tap.flowing = false;
    GstAudioInfo input;
    if (caps == nullptr || gst_audio_info_from_caps(&input, caps) == FALSE) {
      return;
    }
    const int rate = GST_AUDIO_INFO_RATE(&input);
    const int channels = GST_AUDIO_INFO_CHANNELS(&input);
    if (rate < kMinInputRate || rate > kMaxInputRate || channels < 1 || channels > kMaxChannels) {
      return;
    }
    GstAudioInfo output;
    gst_audio_info_set_format(&output, GST_AUDIO_FORMAT_F32, tap.rate, 1, nullptr);
    tap.converter = gst_audio_converter_new(GST_AUDIO_CONVERTER_FLAG_NONE, &input, &output, nullptr);
    tap.input = input;
    if (tap.converter != nullptr) {
      const auto chunk = static_cast<gsize>(std::max(1, rate / kChunksPerSecond));
      tap.converted.resize(gst_audio_converter_get_out_frames(tap.converter, chunk));
    }
  }

  // Reads the caps and segment that passed the pad before the probe was attached.
  void prime(AudioTap& tap, GstPad* pad) {
    tap.primed = true;
    if (GstCaps* caps = gst_pad_get_current_caps(pad)) {
      configure(tap, caps);
      gst_caps_unref(caps);
    }
    if (GstEvent* event = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0)) {
      const GstSegment* segment = nullptr;
      gst_event_parse_segment(event, &segment);
      gst_segment_copy_into(segment, &tap.segment);
      gst_event_unref(event);
    }
  }

  // The samples that follow are heard from `at` on.
  void anchorAt(AudioTap& tap, GstClockTime at) {
    gst_audio_converter_reset(tap.converter);
    tap.skip = gst_util_uint64_scale_int_round(
        gst_audio_converter_get_max_latency(tap.converter), tap.rate, GST_AUDIO_INFO_RATE(&tap.input)
    );
    const std::scoped_lock lock(tap.lock);
    tap.written = 0;
    tap.anchor = static_cast<std::int64_t>(at);
    tap.ownGeneration = tap.generation.fetch_add(1) + 1;
    tap.inputStart = at;
    tap.inputFrames = 0;
    tap.flowing = true;
  }

  // Appends converted samples to the ring, unless the samples started again meanwhile.
  void publish(AudioTap& tap, std::span<const float> samples) {
    const std::scoped_lock lock(tap.lock);
    if (tap.generation != tap.ownGeneration) {
      tap.flowing = false;
      return;
    }
    const auto size = std::ssize(tap.ring);
    const auto total = std::ssize(samples);
    const auto kept = samples.last(static_cast<std::size_t>(std::min(total, size)));
    const auto at = (tap.written + total - std::ssize(kept)) % size;
    const auto first = std::min(std::ssize(kept), size - at);
    std::ranges::copy(kept.first(static_cast<std::size_t>(first)), tap.ring.begin() + at);
    std::ranges::copy(kept.subspan(static_cast<std::size_t>(first)), tap.ring.begin());
    tap.written += total;
  }

  // Converts a mapped buffer a chunk at a time.
  void convert(AudioTap& tap, const GstAudioBuffer& audio) {
    if (audio.n_planes < 1 || audio.n_planes > kMaxChannels) {
      return;
    }
    const auto frames = audio.n_samples;
    const auto chunk = static_cast<gsize>(std::max(1, GST_AUDIO_INFO_RATE(&tap.input) / kChunksPerSecond));
    const bool interleaved = GST_AUDIO_INFO_LAYOUT(&tap.input) == GST_AUDIO_LAYOUT_INTERLEAVED;
    const auto stride =
        static_cast<gsize>(interleaved ? GST_AUDIO_INFO_BPF(&tap.input) : GST_AUDIO_INFO_BPS(&tap.input));
    // Each pass converts one chunk, so the passes end with the buffer.
    for (gsize done = 0; done < frames; done += chunk) {
      const gsize part = std::min(chunk, frames - done);
      std::array<gpointer, kMaxChannels> in{};
      for (int plane = 0; plane < audio.n_planes; ++plane) {
        in.at(static_cast<std::size_t>(plane)) = static_cast<guint8*>(audio.planes[plane]) + (done * stride);
      }
      const gsize count = gst_audio_converter_get_out_frames(tap.converter, part);
      if (tap.converted.size() < count) {
        tap.converted.resize(count);
      }
      gpointer out = tap.converted.data();
      if (gst_audio_converter_samples(tap.converter, GST_AUDIO_CONVERTER_FLAG_NONE, in.data(), part, &out, count)
          == FALSE) {
        tap.flowing = false;
        return;
      }
      const gsize skipped = std::min(tap.skip, count);
      tap.skip -= skipped;
      publish(tap, std::span(tap.converted).first(count).subspan(skipped));
    }
  }

  // Places a buffer's sound at its stream time, starting the samples again after a jump.
  void take(AudioTap& tap, GstBuffer* buffer) {
    if (buffer == nullptr) {
      return;
    }
    const GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (tap.converter == nullptr || tap.segment.format != GST_FORMAT_TIME || !GST_CLOCK_TIME_IS_VALID(pts)) {
      return;
    }
    const GstClockTime at = gst_segment_to_stream_time(&tap.segment, GST_FORMAT_TIME, pts);
    if (!GST_CLOCK_TIME_IS_VALID(at)) {
      return;
    }
    const GstClockTime expected =
        tap.inputStart + gst_util_uint64_scale_int(tap.inputFrames, GST_SECOND, GST_AUDIO_INFO_RATE(&tap.input));
    const GstClockTime drift = at > expected ? at - expected : expected - at;
    if (!tap.flowing || tap.generation != tap.ownGeneration || drift > kGapTolerance) {
      anchorAt(tap, at);
    }
    GstAudioBuffer audio;
    if (gst_audio_buffer_map(&audio, &tap.input, buffer, GST_MAP_READ) == FALSE) {
      return;
    }
    convert(tap, audio);
    tap.inputFrames += audio.n_samples;
    gst_audio_buffer_unmap(&audio);
  }

  void hear(AudioTap& tap, GstEvent* event) {
    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_FLUSH_STOP:
      tap.flowing = false;
      restart(tap);
      break;
    case GST_EVENT_CAPS: {
      GstCaps* caps = nullptr;
      gst_event_parse_caps(event, &caps);
      configure(tap, caps);
      break;
    }
    case GST_EVENT_SEGMENT: {
      const GstSegment* segment = nullptr;
      gst_event_parse_segment(event, &segment);
      gst_segment_copy_into(segment, &tap.segment);
      tap.flowing = false;
      break;
    }
    default:
      break;
    }
  }

  // Streaming thread, and for a flush start the thread that flushes: only `restart` runs there.
  GstPadProbeReturn watch(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
    auto& tap = **static_cast<std::shared_ptr<AudioTap>*>(data);
    const auto type = GST_PAD_PROBE_INFO_TYPE(info);
    constexpr auto kEvents = GST_PAD_PROBE_TYPE_EVENT_BOTH | GST_PAD_PROBE_TYPE_EVENT_FLUSH;
    GstEvent* event = (type & kEvents) != 0 ? gst_pad_probe_info_get_event(info) : nullptr;
    if (event != nullptr && GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) {
      restart(tap);
      return GST_PAD_PROBE_OK;
    }
    if (!tap.primed) {
      prime(tap, pad);
    }
    if (event != nullptr) {
      hear(tap, event);
    } else if ((type & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
      take(tap, gst_pad_probe_info_get_buffer(info));
    } else if ((type & GST_PAD_PROBE_TYPE_BUFFER_LIST) != 0) {
      GstBufferList* list = gst_pad_probe_info_get_buffer_list(info);
      // The list's length bounds the pass.
      const guint count = list != nullptr ? gst_buffer_list_length(list) : 0;
      for (guint i = 0; i < count; ++i) {
        take(tap, gst_buffer_list_get(list, i));
      }
    }
    return GST_PAD_PROBE_OK;
  }
} // namespace

std::shared_ptr<AudioTap> luciole::media::makeAudioTap(int rate, int length) {
  return std::make_shared<AudioTap>(rate, length);
}

gulong luciole::media::attachAudioTap(GstPad* pad, const std::shared_ptr<AudioTap>& tap) {
  constexpr auto kWatched = static_cast<GstPadProbeType>(
      GST_PAD_PROBE_TYPE_BUFFER
      | GST_PAD_PROBE_TYPE_BUFFER_LIST
      | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM
      | GST_PAD_PROBE_TYPE_EVENT_FLUSH
  );
  return gst_pad_add_probe(pad, kWatched, watch, new std::shared_ptr<AudioTap>(tap), [](gpointer data) {
    delete static_cast<std::shared_ptr<AudioTap>*>(data);
  });
}

luciole::AudioWindow
luciole::media::copyAudioWindow(AudioTap& tap, std::optional<std::int64_t> heard, std::span<float> samples) {
  const std::scoped_lock lock(tap.lock);
  const std::uint64_t generation = tap.generation;
  std::int64_t end = generation == tap.lastGeneration ? tap.lastEnd : 0;
  if (heard && tap.anchor) {
    const std::int64_t since = *heard - *tap.anchor;
    end = since <= 0
        ? 0
        : static_cast<std::int64_t>(
              gst_util_uint64_scale_int(static_cast<guint64>(since), tap.rate, static_cast<gint>(GST_SECOND))
          );
  }
  end = std::clamp<std::int64_t>(end, 0, tap.written);
  const auto size = std::ssize(tap.ring);
  const std::int64_t oldest = std::max<std::int64_t>(0, tap.written - size);
  const std::int64_t count = std::max<std::int64_t>(
      0, std::min({end - oldest, std::int64_t{tap.length}, static_cast<std::int64_t>(samples.size())})
  );
  const auto from = (end - count) % size;
  const auto first = std::min(count, size - from);
  std::copy_n(tap.ring.begin() + from, first, samples.begin());
  std::copy_n(tap.ring.begin(), count - first, samples.begin() + first);
  const bool fresh = count > 0 && (end != tap.lastEnd || generation != tap.lastGeneration);
  tap.lastEnd = end;
  tap.lastGeneration = generation;
  return {.count = static_cast<int>(count), .fresh = fresh};
}

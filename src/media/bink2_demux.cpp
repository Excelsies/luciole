#include "media/bink2_demux.h"

#include "media/bink2_demux_state.h"
#include "media/bink2_stream.h"

#include <gst/video/video.h>
#include <mutex>
#include <utility>

GST_DEBUG_CATEGORY(luciole::media::bink2::bink2Debug);

namespace {
  using namespace luciole::media::bink2;

  constexpr auto kSignature = "KB2";
  constexpr gsize kSignatureBytes = 4;

  GstStaticPadTemplate sinkTemplate =
      GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-luciole-bink2"));
  GstStaticPadTemplate videoTemplate = GST_STATIC_PAD_TEMPLATE(
      "video", GST_PAD_SRC, GST_PAD_SOMETIMES, GST_STATIC_CAPS("video/x-raw, format = (string) I420")
  );
  GstStaticPadTemplate soundTemplate = GST_STATIC_PAD_TEMPLATE(
      "audio_%u", GST_PAD_SRC, GST_PAD_SOMETIMES,
      GST_STATIC_CAPS(
          "audio/x-raw, format = (string) S16LE, layout = (string) interleaved, "
          "channels = (int) [ 1, 2 ]"
      )
  );

  gboolean activateSink(GstPad* pad, GstObject* parent) {
    GstQuery* query = gst_query_new_scheduling();
    const bool pull = gst_pad_peer_query(pad, query) != FALSE
        && gst_query_has_scheduling_mode_with_flags(query, GST_PAD_MODE_PULL, GST_SCHEDULING_FLAG_SEEKABLE) != FALSE;
    gst_query_unref(query);
    if (!pull) {
      gst_element_message_full(
          GST_ELEMENT(parent), GST_MESSAGE_ERROR, GST_STREAM_ERROR, GST_STREAM_ERROR_WRONG_TYPE,
          g_strdup("A Bink 2 video can only be played from a seekable local file."), nullptr, __FILE__, "activateSink",
          __LINE__
      );
      return FALSE;
    }
    return gst_pad_activate_mode(pad, GST_PAD_MODE_PULL, TRUE);
  }

  gboolean activateSinkMode(GstPad* pad, GstObject* parent, GstPadMode mode, gboolean active) {
    if (mode != GST_PAD_MODE_PULL) {
      return FALSE;
    }
    return active != FALSE ? gst_pad_start_task(pad, runStreamingTask, &demuxOf(parent), nullptr)
                           : gst_pad_stop_task(pad);
  }
} // namespace

struct LucioleBink2DemuxClass {
  GstElementClass parent;
};

G_DEFINE_TYPE(LucioleBink2Demux, luciole_bink2_demux, GST_TYPE_ELEMENT)

namespace {
  void finalizeDemux(GObject* object) {
    auto* self = reinterpret_cast<LucioleBink2Demux*>(object);
    delete std::exchange(self->impl, nullptr);
    G_OBJECT_CLASS(luciole_bink2_demux_parent_class)->finalize(object);
  }

  GstStateChangeReturn changeState(GstElement* element, GstStateChange transition) {
    auto& demux = demuxOf(GST_OBJECT(element));
    // Going down, a streaming pass may be parked waiting for a buffer that a consumer holds: the pool stops handing
    // them out first, or stopping the task would wait for ever.
    if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
      if (GstBufferPool* pool = demux.pool.load()) {
        gst_buffer_pool_set_flushing(pool, TRUE);
      }
    }
    const GstStateChangeReturn result =
        GST_ELEMENT_CLASS(luciole_bink2_demux_parent_class)->change_state(element, transition);
    // Going down, the parent has stopped the task, so nothing streams while the stream is freed.
    if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
      releaseStream(demux);
    }
    return result;
  }

  void findBink2(GstTypeFind* find, gpointer /*data*/) {
    const guint8* head = gst_type_find_peek(find, 0, kSignatureBytes);
    if (head == nullptr || head[0] != kSignature[0] || head[1] != kSignature[1] || head[2] != kSignature[2]) {
      return;
    }
    // The revisions the decoder knows, 'f' to 'n'.
    if (head[3] < 'f' || head[3] > 'n') {
      return;
    }
    // The magic alone is likely; a header the player would accept is certain. The header and the track tables are
    // read when the stream has them.
    constexpr gsize kHeaderBytes = 44;
    constexpr gsize kMostHeadBytes = kHeaderBytes + 4 + 7 * 12;
    gsize available = kMostHeadBytes;
    const guint8* more = gst_type_find_peek(find, 0, available);
    if (more == nullptr) {
      available = kHeaderBytes;
      more = gst_type_find_peek(find, 0, available);
    }
    const bool sane = more != nullptr && checkContainerHead({more, available}, std::nullopt).has_value();
    GstCaps* caps = gst_static_caps_get(&sinkTemplate.static_caps);
    gst_type_find_suggest(find, sane ? GST_TYPE_FIND_MAXIMUM : GST_TYPE_FIND_LIKELY, caps);
    gst_caps_unref(caps);
  }

  gboolean initPlugin(GstPlugin* plugin) {
    GST_DEBUG_CATEGORY_INIT(bink2Debug, "luciole-bink2", 0, "Bink 2 demuxer");
    GstCaps* caps = gst_static_caps_get(&sinkTemplate.static_caps);
    const bool typefinder =
        gst_type_find_register(
            plugin, "luciole_bink2_typefind", GST_RANK_PRIMARY, findBink2, nullptr, caps, nullptr, nullptr
        )
        != FALSE;
    gst_caps_unref(caps);
    return typefinder
        && gst_element_register(plugin, kDemuxName, GST_RANK_PRIMARY + 1, luciole_bink2_demux_get_type()) != FALSE;
  }
} // namespace

static void luciole_bink2_demux_class_init(LucioleBink2DemuxClass* klass) {
  GstElementClass* element = GST_ELEMENT_CLASS(klass);
  G_OBJECT_CLASS(klass)->finalize = finalizeDemux;
  element->change_state = changeState;
  gst_element_class_add_static_pad_template(element, &sinkTemplate);
  gst_element_class_add_static_pad_template(element, &videoTemplate);
  gst_element_class_add_static_pad_template(element, &soundTemplate);
  gst_element_class_set_static_metadata(
      element, "Bink 2 decoder", "Codec/Demuxer", "Decodes the video and sound of a Bink 2 file", "Luciole"
  );
}

static void luciole_bink2_demux_init(LucioleBink2Demux* self) {
  GstPad* sink = gst_pad_new_from_static_template(&sinkTemplate, "sink");
  gst_pad_set_activate_function(sink, activateSink);
  gst_pad_set_activatemode_function(sink, activateSinkMode);
  (void)gst_element_add_pad(GST_ELEMENT(self), sink);
  self->impl = new luciole::media::bink2::Demux(GST_ELEMENT(self), sink);
}

const std::string& luciole::media::bink2::registerElements() {
  static const std::string problem = [] {
    const gboolean done = gst_plugin_register_static(
        GST_VERSION_MAJOR, GST_VERSION_MINOR, "luciole-bink2", "Bink 2 video and sound", initPlugin, "1.0", "LGPL",
        "luciole", "luciole", "https://github.com/noctalia-dev/luciole"
    );
    return done != FALSE ? std::string{} : std::string("GStreamer did not take the Bink 2 elements.");
  }();
  return problem;
}

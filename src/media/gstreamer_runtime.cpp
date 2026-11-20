#include "media/gstreamer_runtime.h"

#include "media/bink2_demux.h"

#include <cerrno>
#include <format>
#include <sys/stat.h>

namespace {
  constexpr std::string_view kLocalOnly = "Only local files are played.";
  constexpr std::string_view kRegularOnly = "Only regular files are played.";

  // Lowers every decoder whose class says it runs on a graphics device below automatic choice.
  void preferProcessorDecoders() {
    GList* decoders = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_NONE);
    for (GList* item = decoders; item != nullptr; item = item->next) {
      auto* factory = static_cast<GstElementFactory*>(item->data);
      const char* klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
      if (klass != nullptr && std::string_view(klass).contains("Hardware")) {
        gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_NONE);
      }
    }
    gst_plugin_feature_list_free(decoders);
  }

  // Demuxers that fetch their parts, and sources and parsers that open sockets. An RTP depayloader's class names
  // Network too, so a local file holding an RTP stream is refused as well.
  bool reachesNetwork(std::string_view klass) {
    // At most one pass per part of the class.
    while (!klass.empty()) {
      const auto slash = klass.find('/');
      const auto part = klass.substr(0, slash);
      if (part == "Network" || part == "Adaptive") {
        return true;
      }
      klass = slash == std::string_view::npos ? std::string_view{} : klass.substr(slash + 1);
    }
    return false;
  }

  // Ends `bin` with an error that says only local files are played.
  void postLocalOnly(GstBin* bin) {
    GError* error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_AUTHORIZED, kLocalOnly.data());
    (void)gst_element_post_message(GST_ELEMENT(bin), gst_message_new_error(GST_OBJECT(bin), error, nullptr));
    g_error_free(error);
  }

  bool factoryReachesNetwork(GstElementFactory* factory) {
    const char* klass =
        factory != nullptr ? gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS) : nullptr;
    return klass != nullptr && reachesNetwork(klass);
  }

  // The values of GstAutoplugSelectResult, which the plugin does not install a header for.
  constexpr gint kAutoplugTry = 0;
  constexpr gint kAutoplugSkip = 2;

  // A parser or decoder bin asks before it builds an element: one that would reach the network is never made, so
  // its streaming threads never start fetching.
  gint
  refuseFactory(GstElement* bin, GstPad* /*pad*/, GstCaps* /*caps*/, GstElementFactory* factory, gpointer /*data*/) {
    if (!factoryReachesNetwork(factory)) {
      return kAutoplugTry;
    }
    postLocalOnly(GST_BIN(bin));
    return kAutoplugSkip;
  }

  // Refuses an element that would reach the network: it keeps its state and the bin ends with an error. A parser or
  // decoder bin is also told not to build one.
  void refuseElement(GstBin* bin, GstBin* /*parent*/, GstElement* element, gpointer /*data*/) {
    if (g_signal_lookup("autoplug-select", G_OBJECT_TYPE(element)) != 0) {
      g_signal_connect(element, "autoplug-select", G_CALLBACK(refuseFactory), nullptr);
    }
    if (!factoryReachesNetwork(gst_element_get_factory(element))) {
      return;
    }
    gst_element_set_locked_state(element, TRUE);
    postLocalOnly(bin);
  }

  // Whether the opened system output stands in a sink that plays nothing, as it does when no device opens.
  bool playsNothing(GstElement* sink) {
    if (!GST_IS_CHILD_PROXY(sink)) {
      return false;
    }
    const guint count = gst_child_proxy_get_children_count(GST_CHILD_PROXY(sink));
    for (guint i = 0; i < count; ++i) {
      GObject* child = gst_child_proxy_get_child_by_index(GST_CHILD_PROXY(sink), i);
      bool fake = false;
      if (child != nullptr && GST_IS_ELEMENT(child)) {
        GstElementFactory* factory = gst_element_get_factory(GST_ELEMENT(child));
        fake = factory != nullptr && std::string_view(GST_OBJECT_NAME(factory)).starts_with("fake");
      }
      if (child != nullptr) {
        g_object_unref(child);
      }
      if (fake) {
        return true;
      }
    }
    return false;
  }

  GstElement* silentSink() {
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    if (sink != nullptr) {
      gst_object_ref_sink(sink);
      g_object_set(sink, "sync", TRUE, "enable-last-sample", FALSE, nullptr);
    }
    return sink;
  }
} // namespace

const std::string& luciole::media::startProblem() {
  // Never freed, so a pipeline built while the process exits still reads it.
  static const std::string* const problem = new std::string([] {
    GError* error = nullptr;
    if (gst_init_check(nullptr, nullptr, &error) == FALSE) {
      std::string text = std::format("GStreamer cannot start: {}", error != nullptr ? error->message : "");
      g_clear_error(&error);
      return text;
    }
    if (const auto& problem = bink2::registerElements(); !problem.empty()) {
      return problem;
    }
    for (const char* name : {"playbin", "appsink", "videoconvertscale", "capsfilter", "fakesink", "identity"}) {
      GstElementFactory* factory = gst_element_factory_find(name);
      if (factory == nullptr) {
        return std::format("GStreamer has no {} element.", name);
      }
      gst_object_unref(factory);
    }
    preferProcessorDecoders();
    return std::string{};
  }());
  return *problem;
}

std::string luciole::media::fileProblem(const std::string& path) {
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0) {
    return std::format("The file cannot be read: {}.", g_strerror(errno));
  }
  return S_ISREG(status.st_mode) ? std::string{} : std::string(kRegularOnly);
}

void luciole::media::refuseNetwork(GstElement* bin) {
  g_signal_connect(bin, "deep-element-added", G_CALLBACK(refuseElement), nullptr);
}

luciole::media::AudioSink luciole::media::openAudioSink(AudioOutput output) {
  if (output == AudioOutput::Silent) {
    return {.element = silentSink(), .deviceMissing = false};
  }
  GstElement* system = gst_element_factory_make("autoaudiosink", nullptr);
  if (system != nullptr) {
    gst_object_ref_sink(system);
    if (gst_element_set_state(system, GST_STATE_READY) == GST_STATE_CHANGE_SUCCESS && !playsNothing(system)) {
      return {.element = system, .deviceMissing = false};
    }
    (void)gst_element_set_state(system, GST_STATE_NULL);
    gst_object_unref(system);
  }
  return {.element = silentSink(), .deviceMissing = true};
}

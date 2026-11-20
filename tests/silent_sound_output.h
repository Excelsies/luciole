#pragma once

#include "test_check.h"

#include <filesystem>
#include <gst/gst.h>
#include <stdlib.h>

// For tests that play sound through the system's output: GStreamer keeps its registry in the test's private folder,
// and the system's output becomes an element that keeps the clock and plays nothing, so no test makes a sound.
namespace test::audio {
  // A bin around an appsink that keeps one buffer and drops the rest, in step with the clock.
  inline GType silentSinkType() {
    static const GType type = [] {
      const auto classInit = [](gpointer klass, gpointer /*data*/) {
        gst_element_class_set_static_metadata(
            GST_ELEMENT_CLASS(klass), "Silent sink", "Sink/Audio", "Keeps the clock and plays nothing", "tests"
        );
      };
      const auto instanceInit = [](GTypeInstance* instance, gpointer /*klass*/) {
        GstElement* sink = gst_element_factory_make("appsink", nullptr);
        TEST_CHECK(sink != nullptr);
        g_object_set(
            sink, "sync", TRUE, "max-buffers", 1U, "drop", TRUE, "wait-on-eos", FALSE, "enable-last-sample", FALSE,
            nullptr
        );
        gst_bin_add(GST_BIN(instance), sink);
        GstPad* entry = gst_element_get_static_pad(sink, "sink");
        TEST_CHECK(gst_element_add_pad(GST_ELEMENT(instance), gst_ghost_pad_new("sink", entry)) == TRUE);
        gst_object_unref(entry);
      };
      return g_type_register_static_simple(
          GST_TYPE_BIN, "LucioleSilentAudioSink", sizeof(GstBinClass), classInit, sizeof(GstBin), instanceInit,
          GTypeFlags{}
      );
    }();
    return type;
  }

  // Starts GStreamer with its registry in `folder` and makes the system's output silent for the rest of the
  // process, checking that the silent element is what gets made.
  inline void useSilentSystemOutput(const std::filesystem::path& folder) {
    TEST_CHECK(::setenv("GST_REGISTRY", (folder / "registry.bin").c_str(), 1) == 0);
    gst_init(nullptr, nullptr);
    TEST_CHECK(gst_element_register(nullptr, "autoaudiosink", GST_RANK_NONE, silentSinkType()) == TRUE);
    GstElement* sink = gst_element_factory_make("autoaudiosink", nullptr);
    TEST_CHECK(sink != nullptr && G_TYPE_CHECK_INSTANCE_TYPE(sink, silentSinkType()));
    if (sink != nullptr) {
      gst_object_unref(gst_object_ref_sink(sink));
    }
  }
} // namespace test::audio

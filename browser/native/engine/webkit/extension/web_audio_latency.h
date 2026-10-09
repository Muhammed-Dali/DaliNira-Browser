#pragma once

#include <gst/gst.h>
#include <cstring>

namespace dalinira::engine::webkit {

// WebKitGTK's Web Audio destination uses an independent GStreamer pipeline
// and forces autoaudiosink's child to a 100 ms buffer. latencyHint does not
// configure that sink in WebKitGTK 2.52. Tune only this output, before PLAYING;
// normal media sinks retain WebKit's clock/latency negotiation.
inline void tuneWebAudioSink(GstElement *element) {
  if (GST_STATE(element) > GST_STATE_READY) return;
  const GType audioSinkType = g_type_from_name("GstAudioBaseSink");
  if (!audioSinkType || !g_type_is_a(G_OBJECT_TYPE(element), audioSinkType)) return;
  gint64 bufferTime = 0, latencyTime = 0;
  g_object_get(element, "buffer-time", &bufferTime, "latency-time", &latencyTime, nullptr);
  if (bufferTime > 30000) g_object_set(element, "buffer-time", static_cast<gint64>(30000), nullptr);
  if (latencyTime > 10000) g_object_set(element, "latency-time", static_cast<gint64>(10000), nullptr);
}

inline void tuneWebAudioOutput(GstElement *element) {
  tuneWebAudioSink(element);
  if (!GST_IS_BIN(element)) return;
  GstIterator *iterator = gst_bin_iterate_recurse(GST_BIN(element));
  GValue child = G_VALUE_INIT;
  bool done = false;
  while (!done) {
    switch (gst_iterator_next(iterator, &child)) {
      case GST_ITERATOR_OK:
        tuneWebAudioSink(GST_ELEMENT(g_value_get_object(&child)));
        g_value_reset(&child);
        break;
      case GST_ITERATOR_RESYNC: gst_iterator_resync(iterator); break;
      default: done = true; break;
    }
  }
  if (G_VALUE_TYPE(&child)) g_value_unset(&child);
  gst_iterator_free(iterator);
}

inline void installWebAudioLatencyPolicy() {
  static const bool installed = [] {
    // No command-line parsing and no device opening.
    if (!gst_init_check(nullptr, nullptr, nullptr)) return false;
    gpointer binClass = g_type_class_ref(GST_TYPE_BIN);
    const guint signal = g_signal_lookup("deep-element-added", GST_TYPE_BIN);
    const gulong hook = g_signal_add_emission_hook(signal, 0,
        +[](GSignalInvocationHint *, guint count, const GValue *values, gpointer) -> gboolean {
          if (count != 3) return TRUE;
          auto *pipeline = GST_ELEMENT(g_value_get_object(&values[0]));
          if (!GST_IS_PIPELINE(pipeline)) return TRUE;
          const char *name = GST_OBJECT_NAME(pipeline);
          constexpr const char *prefix = "audio-destination";
          if (!name || !g_str_has_prefix(name, prefix)) return TRUE;
          const char *suffix = name + std::strlen(prefix);
          // Older WebKit releases use the unnumbered pipeline name.
          if (*suffix) {
            if (*suffix++ != '-' || !*suffix) return TRUE;
            for (const char *c = suffix; *c; ++c) if (!g_ascii_isdigit(*c)) return TRUE;
          }
          tuneWebAudioOutput(GST_ELEMENT(g_value_get_object(&values[2])));
          return TRUE;
        }, nullptr, nullptr);
    g_type_class_unref(binClass);
    return hook != 0;
  }();
  (void)installed;
}

} // namespace dalinira::engine::webkit

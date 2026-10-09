#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

namespace dalinira::engine {

// The GStreamer runtime WebKitGTK's media stack needs, checked against the
// same plugin registry WebKit's web processes use. See docs/webkit-media-runtime.md
// for the per-distribution packages that provide these elements.
class WebKitMediaRuntime {
 public:
  struct Gap {
    QString element;  // First acceptable element, e.g. "vp9dec".
    QString plugin;   // GStreamer plugin that provides it, e.g. "vpx".
    QString module;   // Upstream GStreamer module, e.g. "gst-plugins-good".
    QString purpose;
    bool fatal = false;
  };
  struct Report {
    // False when WebKitGTK would abort a web process as soon as a page creates
    // a media element; media must then stay disabled.
    bool playbackAvailable = false;
    QList<Gap> missing;
  };

  // Pure evaluation over an element lookup, so the policy is testable without
  // manipulating the system registry.
  static Report evaluate(const std::function<bool(const char *element)> &hasElement);
  // Evaluates the live GStreamer registry (initializing GStreamer if needed).
  static Report probe();
  // probe(), computed once per process.
  static const Report &cached();
  // One log line naming the missing elements and their modules.
  static QString describe(const Report &report);
};

} // namespace dalinira::engine

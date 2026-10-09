#include "engine/webkit/webkit_media_runtime.h"

#include <gst/gst.h>

namespace dalinira::engine {

namespace {

struct Requirement {
  // Any one of these elements satisfies the requirement.
  std::initializer_list<const char *> elements;
  const char *plugin;
  const char *module;
  const char *purpose;
  bool fatal;
};

// Elements WebKitGTK 2.52's GStreamer media player (MediaPlayerPrivateGStreamer),
// its MSE append pipeline and its video sink create for HTML5 media, plus the
// decoders and MSE parsers for the formats YouTube serves (VP9/Opus WebM,
// H.264/AAC MP4). Checked against the webkitgtk-2.52.6 sources.
// Only a missing audio sink is fatal: WebKit RELEASE_ASSERTs in
// createAudioSink(). Anything else only makes a format report "unsupported".
const Requirement kRequirements[] = {
    {{"autoaudiosink"}, "autodetect", "gst-plugins-good", "audio output (web process aborts without it)", true},
    {{"playbin"}, "playback", "gst-plugins-base", "media pipeline", false},
    {{"parsebin"}, "playback", "gst-plugins-base", "MSE demuxing", false},
    {{"typefind"}, "coreelements", "gstreamer", "container detection", false},
    {{"appsrc"}, "app", "gst-plugins-base", "MSE and network sources", false},
    {{"appsink"}, "app", "gst-plugins-base", "MSE append pipeline", false},
    {{"audioconvert"}, "audioconvert", "gst-plugins-base", "audio conversion", false},
    {{"audioresample"}, "audioresample", "gst-plugins-base", "audio resampling", false},
    {{"volume"}, "volume", "gst-plugins-base", "volume and mute", false},
    {{"videoconvert"}, "videoconvertscale", "gst-plugins-base", "video conversion", false},
    {{"videoscale"}, "videoconvertscale", "gst-plugins-base", "video scaling", false},
    {{"glupload"}, "opengl", "gst-plugins-base", "GPU video upload", false},
    {{"glcolorconvert"}, "opengl", "gst-plugins-base", "GPU color conversion", false},
    {{"scaletempo"}, "audiofx", "gst-plugins-good", "playback-rate changes", false},
    {{"matroskademux"}, "matroska", "gst-plugins-good", "WebM (YouTube VP9/Opus)", false},
    {{"qtdemux"}, "isomp4", "gst-plugins-good", "MP4 (YouTube H.264/AAC)", false},
    {{"aacparse"}, "audioparsers", "gst-plugins-good", "AAC streams", false},
    {{"vp9dec", "avdec_vp9"}, "vpx", "gst-plugins-good", "VP9 video", false},
    {{"opusdec"}, "opus", "gst-plugins-base", "Opus audio", false},
    {{"avdec_h264", "openh264dec", "vah264dec"}, "libav", "gst-libav", "H.264 video", false},
    {{"avdec_aac", "fdkaacdec", "faad"}, "libav", "gst-libav", "AAC audio", false},
    // MSE only (GStreamerRegistryScanner / AppendPipeline): Opus is advertised
    // to MSE only when an Opus parser exists; vp9parse and h264parse complete
    // the track metadata of YouTube's VP9 and H.264 streams.
    {{"opusparse"}, "opusparse", "gst-plugins-bad", "Opus through MSE (YouTube audio)", false},
    {{"vp9parse"}, "videoparsersbad", "gst-plugins-bad", "VP9 through MSE", false},
    {{"h264parse"}, "videoparsersbad", "gst-plugins-bad", "H.264 through MSE", false},
};

}  // namespace

WebKitMediaRuntime::Report WebKitMediaRuntime::evaluate(
    const std::function<bool(const char *)> &hasElement) {
  Report report;
  report.playbackAvailable = true;
  for (const Requirement &requirement : kRequirements) {
    bool satisfied = false;
    for (const char *element : requirement.elements) {
      if (hasElement(element)) {
        satisfied = true;
        break;
      }
    }
    if (satisfied) continue;
    Gap gap;
    gap.element = QString::fromLatin1(*requirement.elements.begin());
    gap.plugin = QString::fromLatin1(requirement.plugin);
    gap.module = QString::fromLatin1(requirement.module);
    gap.purpose = QString::fromLatin1(requirement.purpose);
    gap.fatal = requirement.fatal;
    if (gap.fatal) report.playbackAvailable = false;
    report.missing.append(gap);
  }
  return report;
}

WebKitMediaRuntime::Report WebKitMediaRuntime::probe() {
  if (!gst_init_check(nullptr, nullptr, nullptr)) {
    Report report;
    Gap gap;
    gap.element = QStringLiteral("gstreamer");
    gap.plugin = QStringLiteral("core");
    gap.module = QStringLiteral("gstreamer");
    gap.purpose = QStringLiteral("GStreamer failed to initialize");
    gap.fatal = true;
    report.missing.append(gap);
    return report;
  }
  return evaluate([](const char *element) {
    GstElementFactory *factory = gst_element_factory_find(element);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
  });
}

const WebKitMediaRuntime::Report &WebKitMediaRuntime::cached() {
  static const Report report = probe();
  return report;
}

QString WebKitMediaRuntime::describe(const Report &report) {
  QStringList parts;
  for (const Gap &gap : report.missing) {
    parts.append(QStringLiteral("%1 (%2, %3: %4)").arg(gap.element, gap.plugin, gap.module, gap.purpose));
  }
  return parts.join(QStringLiteral("; "));
}

} // namespace dalinira::engine

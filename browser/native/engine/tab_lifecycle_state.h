#pragma once

#include <QMetaType>

namespace dalinira {

// The one tab lifecycle model shared by both engines and by the browser's
// memory saver (TabPerformanceManager). Engines map it onto their own
// mechanisms:
//   Active     full speed.
//   Frozen     background: Chromium freezes the page's tasks; WebKit runs the
//              page hidden, where WebKit throttles timers and stops rendering.
//   Discarded  "sleeping": the renderer / web process resources are released
//              and only URL, title, favicon and session history are kept. The
//              page is restored (reloaded from its history entry) on waking.
enum class TabLifecycleState { Active, Frozen, Discarded };

} // namespace dalinira

Q_DECLARE_METATYPE(dalinira::TabLifecycleState)

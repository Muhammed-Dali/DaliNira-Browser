#pragma once

#include <QString>
#include <QStringList>

namespace dalinira::engine {

// Where the Gecko host comes from: the Gecko runtime (the GRE, the directory
// holding the `firefox` executable and libxul) and the DaliNira Gecko Host
// application (application.ini and its chrome, browser/resources/gecko-host)
// that the runtime runs. Gecko runs only in that separate host process,
// never inside DaliNira.
struct GeckoRuntimePaths {
  QString greDirectory;
  QString hostApplicationDirectory;
};

// A runtime that passed GeckoRuntime::validate(): every path is canonical.
struct GeckoValidatedRuntime {
  QString greDirectory;
  QString executable;              // <gre>/firefox
  QString hostApplicationDirectory;
  QString applicationIni;          // <app>/application.ini
  QString updatePolicyFile;        // <app>/distribution/policies.json
  QString milestone;               // Gecko version of the runtime (platform.ini)

  bool isValid() const { return !executable.isEmpty(); }
};

class GeckoRuntime {
 public:
  // Checks everything the host launch relies on, before anything runs:
  //  - both directories, and every directory above them, can be changed
  //    only by this user or root (a root-owned sticky directory such as /tmp
  //    is allowed), and the executable is a regular executable file of this
  //    user or root that nobody else can write, inside the GRE;
  //  - the GRE holds libxul and the content process executable, and its
  //    major version is the one application.ini accepts;
  //  - application updates can be disabled: a Nightly development runtime,
  //    whose policy engine reads the host's own update policy (packaged
  //    runtimes need DaliNira's dedicated Gecko build, not done yet), and no
  //    system or runtime policy file that would take precedence over it;
  //  - the host application is DaliNira's, with exactly its update policy.
  // An invalid result lists every problem found.
  static GeckoValidatedRuntime validate(const GeckoRuntimePaths &paths, QStringList *problems);

  // Development configuration: DALINIRA_GECKO_RUNTIME and
  // DALINIRA_GECKO_HOST_APP from the environment, else the build's
  // DALINIRA_GECKO_DEVELOPMENT_RUNTIME and the source tree's host application.
  static GeckoRuntimePaths developmentPaths();
};

}  // namespace dalinira::engine

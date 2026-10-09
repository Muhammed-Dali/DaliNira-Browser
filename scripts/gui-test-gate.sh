#!/bin/sh
# ctest wrapper for DaliNira tests that open real windows (the GUI tier).
#
# A plain `ctest` must stay invisible and light on a developer's desktop, so
# these tests only run when DALINIRA_GUI_TESTS=1 is set. That is done by
# scripts/safe-gui-test.sh (the desktop runner: one test at a time, resource
# monitor, crash checks) and by CI on a virtual display. Otherwise the test
# reports "Skipped" (exit 77, the tests' SKIP_RETURN_CODE).
#
# Core dumps are off for the test and every engine process it starts
# (RLIMIT_CORE is inherited): processing one WebKit core dump takes
# systemd-coredump about 5.5 GB of memory. Crashes are still logged.
# DALINIRA_GUI_KEEP_CORES=1 keeps them for debugging.

if [ "${DALINIRA_GUI_TESTS:-0}" != 1 ]; then
  echo "SKIPPED: this test opens real windows. Run it with scripts/safe-gui-test.sh"
  echo "(or set DALINIRA_GUI_TESTS=1 on a virtual display, as CI does)."
  exit 77
fi
[ "${DALINIRA_GUI_KEEP_CORES:-0}" = 1 ] || ulimit -c 0
exec "$@"

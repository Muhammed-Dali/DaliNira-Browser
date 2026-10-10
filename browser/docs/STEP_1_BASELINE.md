# Step 1 — live-page transfer baseline

The test executable `dalinira-browser-state-test` proves only the mechanism used
by the prototype: one `QWebEngineView` with the same `QWebEnginePage` is
reparented from a main native host to a detached native host and back.

It asserts after each transfer:

- identical `QWebEnginePage` pointer;
- JavaScript marker is unchanged;
- `history.length` is unchanged;
- scroll position is unchanged;
- the page's media element is still present.

It does **not** prove active audio/video playback continuity. A later test
needs a local media fixture and verified `currentTime` progression. It also
does not validate Wayland global-coordinate based attach behaviour.

Run:

```bash
./scripts/build-dalinira.sh
./build-dalinira/dalinira-browser-state-test
```

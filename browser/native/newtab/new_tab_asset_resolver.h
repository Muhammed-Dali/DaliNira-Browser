#pragma once

#include <QByteArray>
#include <QString>

QString resolveNewTabAssetsDirectory(const QString &applicationDirectory);

// One static asset of the DaliNira New Tab page (background, logo, search
// engine icons, control icons), shared by the Chromium and WebKit dalinira://
// scheme handlers. The allow-list maps request paths to known files only: no
// part of a URL is ever joined into a filesystem path.
struct NewTabStaticAsset {
  QByteArray mimeType;
  QString fileName;          // Relative to the installed new-tab assets directory.
  QString embeddedResource;  // Qt resource fallback when the file is not installed.
};

bool newTabStaticAsset(const QString &requestPath, NewTabStaticAsset *asset);
// Reads the asset from the assets directory, else from the embedded resource.
QByteArray readNewTabStaticAsset(const NewTabStaticAsset &asset, const QString &assetsDirectory);

#pragma once

#include <QObject>
#include <QString>
#include "engine/engine_type.h"

namespace dalinira::engine {

class EngineProfile : public QObject {
  Q_OBJECT
 public:
  explicit EngineProfile(EngineType type, const QString &storagePath, QObject *parent = nullptr)
      : QObject(parent), type_(type), storagePath_(storagePath) {}
  ~EngineProfile() override = default;

  EngineType engineType() const { return type_; }
  QString storagePath() const { return storagePath_; }

 private:
  EngineType type_;
  QString storagePath_;
};

} // namespace dalinira::engine

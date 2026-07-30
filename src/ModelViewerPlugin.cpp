#include "ModelViewerPlugin.h"

#include "ModelViewerWidget.h"

#include <QFileInfo>

namespace Farman {

bool ModelViewerPlugin::canHandle(const QString& filePath) const {
  const QString ext = QFileInfo(filePath).suffix().toLower();
  return !ext.isEmpty() && supportedExtensions().contains(ext, Qt::CaseInsensitive);
}

QWidget* ModelViewerPlugin::createViewer(const QString& filePath, QWidget* parent,
                                         const PluginContext& /*ctx*/) {
  auto*   widget = new ModelViewerWidget(parent);  // ツールバー + 3D ビュー
  QString err;
  if (!widget->loadModel(filePath, &err)) {
    // 読み込み失敗でもウィジェットは返す (空の 3D ビュー)。呼び出し側が所有。
    qWarning("ModelViewerPlugin: load failed: %s", qPrintable(err));
  }
  return widget;
}

} // namespace Farman

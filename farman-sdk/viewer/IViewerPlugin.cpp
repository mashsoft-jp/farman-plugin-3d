// IViewerPlugin の out-of-line 仮想関数の最小実装 (外部プラグイン用)。
//
// farman 本体の src/viewer/IViewerPlugin.cpp は Logger / Settings に依存するため
// そのままは持ち込めない。ここでは基底クラスの vtable / typeinfo を成立させる
// のに必要な version() / canHandle() だけを、farman 内部に依存しない形で定義する
// (syncPluginFromHostSettings() は外部プラグインでは使わないので定義しない)。
// プラグイン側でこれらを override すれば、その実装が使われる。

#include "viewer/IViewerPlugin.h"

#include <QFileInfo>

namespace Farman {

QString IViewerPlugin::version() const {
#ifdef FARMAN_VERSION
  return QStringLiteral(QT_STRINGIFY(FARMAN_VERSION));
#else
  return {};
#endif
}

bool IViewerPlugin::canHandle(const QString& filePath) const {
  const QString ext = QFileInfo(filePath).suffix().toLower();
  return !ext.isEmpty() && supportedExtensions().contains(ext, Qt::CaseInsensitive);
}

} // namespace Farman

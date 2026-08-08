#include "ModelViewerPlugin.h"

#include "ModelView.h"
#include "ModelViewerWidget.h"

#include <QFileInfo>
#include <QKeySequence>
#include <QStringList>
#include <QVariantMap>

namespace Farman {

bool ModelViewerPlugin::canHandle(const QString& filePath) const {
  const QString ext = QFileInfo(filePath).suffix().toLower();
  return !ext.isEmpty() && supportedExtensions().contains(ext, Qt::CaseInsensitive);
}

QList<ViewerCommandDef> ModelViewerPlugin::shortcutCommands() const {
  const QString v = QStringLiteral("model");
  auto def = [&](const char* id, const QString& label,
                 Qt::Key key) -> ViewerCommandDef {
    return ViewerCommandDef{ v, QStringLiteral("viewer.model.") + QLatin1String(id),
                             label, { QKeySequence(key) } };
  };
  return {
    def("rotate_left",      QStringLiteral("回転: 左"),                 Qt::Key_Left),
    def("rotate_right",     QStringLiteral("回転: 右"),                 Qt::Key_Right),
    def("rotate_up",        QStringLiteral("回転: 上"),                 Qt::Key_Up),
    def("rotate_down",      QStringLiteral("回転: 下"),                 Qt::Key_Down),
    def("pan_up",           QStringLiteral("パン: 上"),                 Qt::Key_W),
    def("pan_down",         QStringLiteral("パン: 下"),                 Qt::Key_S),
    def("pan_left",         QStringLiteral("パン: 左"),                 Qt::Key_A),
    def("pan_right",        QStringLiteral("パン: 右"),                 Qt::Key_D),
    def("zoom_in",          QStringLiteral("拡大"),                     Qt::Key_U),
    def("zoom_out",         QStringLiteral("縮小"),                     Qt::Key_J),
    def("reset",            QStringLiteral("視点をリセット"),           Qt::Key_R),
    def("info",             QStringLiteral("情報を表示"),               Qt::Key_I),
    def("toggle_texture",   QStringLiteral("テクスチャ表示の切り替え"), Qt::Key_T),
    def("toggle_grid",      QStringLiteral("グリッド表示の切り替え"),   Qt::Key_G),
    def("toggle_wireframe", QStringLiteral("ワイヤーフレーム表示の切り替え"), Qt::Key_F),
    def("toggle_bones",     QStringLiteral("ボーン表示の切り替え"),     Qt::Key_B),
    def("toggle_help",      QStringLiteral("ヘルプ表示の切り替え"),     Qt::Key_H),
    def("toggle_animation", QStringLiteral("アニメーション再生 / 一時停止"), Qt::Key_Space),
  };
}

QWidget* ModelViewerPlugin::createViewer(const QString& filePath, QWidget* parent,
                                         const PluginContext& /*ctx*/) {
  auto*   widget = new ModelViewerWidget(parent);  // ツールバー + 3D ビュー
  QString err;
  if (!widget->loadModel(filePath, &err)) {
    // 読み込み失敗でもウィジェットは返す (空の 3D ビュー)。呼び出し側が所有。
    qWarning("ModelViewerPlugin: load failed: %s", qPrintable(err));
  }
  // 既定キーで初期化しておく。本体 (対応版 farman) が applyShortcutBindings で
  // ユーザー割り当てを push すれば上書きされる。push が来ない環境 (未対応 farman
  // 等) でも、既定キーで 3D 操作が効くようにするための保険。
  if (ModelView* mv = widget->view()) {
    QVariantMap defaults;
    for (const ViewerCommandDef& d : shortcutCommands()) {
      QStringList keys;
      for (const QKeySequence& k : d.defaultKeys) {
        if (!k.isEmpty()) {
          keys << k.toString(QKeySequence::PortableText);
        }
      }
      defaults.insert(d.commandId, keys);
    }
    mv->applyShortcutBindings(defaults);
  }
  return widget;
}

} // namespace Farman

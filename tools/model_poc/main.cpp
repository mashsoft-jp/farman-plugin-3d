// farman 3D モデルビュアー PoC。
// ModelViewerWidget (ツールバー + ModelView) を表示する検証用アプリ。
//   farman_model_poc <model file>                       … ウィンドウ表示
//   farman_model_poc <model file> --shot <png>          … 3D をオフスクリーン PNG
//   farman_model_poc <model file> --shot <png> --time <s> … 指定アニメ時刻で
//   farman_model_poc <model file> --uishot <png>        … UI 込みの見た目を PNG
//   ... --no-texture                                    … テクスチャ OFF
//   ... --infosel <ノード名>                            … --infoshot でそのノードを選択 (前方一致)
//   farman_model_poc <model file> --infoshot <png>      … 情報パネル (i) を開いた状態を PNG に保存
//   farman_model_poc <model file> --info                … 情報ウィンドウの内容を標準出力へ

#include "ModelView.h"
#include "ModelViewerWidget.h"

#include <QApplication>
#include <QDialog>
#include <QTreeWidget>
#include <QImage>
#include <QPixmap>
#include <QStringList>
#include <QSurfaceFormat>

#include <cstdio>

namespace {

QString textureInfo(const Farman::ModelViewerWidget& v) {
  if (!v.hasTexture()) return QStringLiteral("テクスチャ: なし");
  QStringList parts;
  if (v.hasEmbeddedTexture()) parts << QStringLiteral("埋め込み");
  parts << v.resolvedTexturePaths();
  for (const QString& p : v.unresolvedTexturePaths()) parts << (p + QStringLiteral(" (見つからず)"));
  QString s = QStringLiteral("テクスチャ: ") + parts.join(QStringLiteral(" / "));
  if (!v.hasUV()) s += QStringLiteral("  ※メッシュに UV なし");
  return s;
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  QSurfaceFormat fmt;
  fmt.setVersion(3, 3);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);

  QString modelPath, shotPath, uishotPath;
  bool    noTexture = false;
  bool    wire      = false;
  bool    bones     = false;
  bool    info      = false;
  QString infoshotPath, infoSelect;
  bool    verboseInfo = false;
  double  timeOpt   = -1.0;
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); ++i) {
    if (args[i] == QLatin1String("--shot") && i + 1 < args.size())
      shotPath = args[++i];
    else if (args[i] == QLatin1String("--uishot") && i + 1 < args.size())
      uishotPath = args[++i];
    else if (args[i] == QLatin1String("--time") && i + 1 < args.size())
      timeOpt = args[++i].toDouble();
    else if (args[i] == QLatin1String("--no-texture"))
      noTexture = true;
    else if (args[i] == QLatin1String("--wire"))
      wire = true;
    else if (args[i] == QLatin1String("--bones"))
      bones = true;
    else if (args[i] == QLatin1String("--info"))
      info = true;
    else if (args[i] == QLatin1String("--infoshot") && i + 1 < args.size())
      infoshotPath = args[++i];
    else if (args[i] == QLatin1String("--info-nodes"))
      info = verboseInfo = true;
    else if (args[i] == QLatin1String("--infosel") && i + 1 < args.size())
      infoSelect = args[++i];
    else if (modelPath.isEmpty())
      modelPath = args[i];
  }
  if (modelPath.isEmpty()) {
    qWarning("usage: farman_model_poc <model file> [--shot <png>] [--uishot <png>] "
             "[--time <sec>] [--no-texture]");
    return 2;
  }

  auto* view = new Farman::ModelViewerWidget;
  view->resize(1000, 800);

  QString err;
  if (!view->loadModel(modelPath, &err)) {
    qWarning("load failed: %s", qPrintable(err));
    return 1;
  }
  if (noTexture) view->setTextureEnabled(false);
  if (wire) view->view()->setWireframe(true);
  if (bones) view->view()->setShowBones(true);

  qInfo("loaded: %s", qPrintable(view->summary()));
  qInfo("%s", qPrintable(textureInfo(*view)));
  for (const QString& p : view->recordedTexturePaths())
    qInfo("  外部テクスチャ記録パス: %s", qPrintable(p));
  for (const QString& p : view->resolvedTexturePaths())
    qInfo("  解決パス              : %s", qPrintable(p));
  if (view->hasAnimation()) qInfo("animation: %.2f s", view->animationDuration());
  if (info) {
    // 情報ウィンドウ (i) と同じ内容。
    puts(qPrintable(view->view()->infoLines().join(QStringLiteral("\n"))));
    if (verboseInfo) {
      // --info-nodes: 各ノードの詳細 (情報ウィンドウの下の枠と同じ内容) も出す。
      for (const Farman::ModelView::InfoNode& n : view->view()->infoTree()) {
        puts(qPrintable(QStringLiteral("\n--- ") + n.label));
        puts(qPrintable(view->view()->nodeInfoLines(n.node).join(QStringLiteral("\n"))));
      }
    }
    return 0;
  }

  if (!infoshotPath.isEmpty()) {
    // 情報パネルを開いて (i キー相当)、ウィジェット全体を PNG に保存する。
    view->resize(1100, 640);
    view->show();
    QMetaObject::invokeMethod(view->view(), "infoRequested");
    QApplication::processEvents();
    if (!infoSelect.isEmpty()) {
      // --infosel <ノード名>: 階層でそのノードを選んだ状態にする。
      if (auto* tree = view->findChild<QTreeWidget*>()) {
        const auto hits = tree->findItems(infoSelect, Qt::MatchStartsWith | Qt::MatchRecursive);
        if (!hits.isEmpty()) tree->setCurrentItem(hits.first());
        QApplication::processEvents();
      }
    }
    view->view()->renderToImage();
    const bool ok = view->grab().save(infoshotPath);
    qInfo("infoshot %s: %s", ok ? "saved" : "FAILED", qPrintable(infoshotPath));
    return ok ? 0 : 1;
  }
  if (!uishotPath.isEmpty()) {
    view->renderToImage();  // 3D フレームを用意
    const QPixmap pm = view->grab();
    const bool    ok = pm.save(uishotPath);
    qInfo("uishot %s: %s", ok ? "saved" : "FAILED", qPrintable(uishotPath));
    return ok ? 0 : 1;
  }
  if (!shotPath.isEmpty()) {
    if (view->hasAnimation()) {
      const double t = timeOpt >= 0 ? timeOpt : view->animationDuration() * 0.5;
      view->setAnimationTime(t);
    }
    const QImage img = view->renderToImage();
    const bool   ok  = img.save(shotPath);
    qInfo("shot %s: %s (%dx%d)", ok ? "saved" : "FAILED", qPrintable(shotPath), img.width(),
          img.height());
    return ok ? 0 : 1;
  }

  view->setWindowTitle(QStringLiteral("farman 3D PoC — ") + view->summary());
  view->show();
  return app.exec();
}

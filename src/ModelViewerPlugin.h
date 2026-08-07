#pragma once

#include "viewer/IViewerPlugin.h"

#include <QObject>

namespace Farman {

// 3D モデル (.fbx) を表示する farman 用の外部ビュアープラグイン。
// IViewerPlugin (IID com.farman.IViewerPlugin/5.0) を実装する。farman 本体には
// 同梱せず外部配布し、ユーザーが外部プラグインディレクトリ
// <AppData>/plugins/viewers/ に置くと farman が起動時に動的ロードする。
//
// 描画本体は ModelView (Assimp + QOpenGLWidget)。まず FBX に対応し、今後
// obj/gltf 等へ拡張予定。IViewerPlugin.cpp は farman 内部 (Logger/Settings) に
// 依存するため vendoring せず、version() / canHandle() はここで override する。
class ModelViewerPlugin : public QObject, public IViewerPlugin {
  Q_OBJECT
  // metadata.json の "MinHostVersion" で必要な farman 本体の最小バージョンを宣言。
  // 本プラグインは libQt6OpenGL を要求するため、それを同梱する farman 0.9.9 以降が
  // 必要。farman はロード前にこの値を読み、満たさなければ実用的な理由を出して
  // スキップする (farman-sdk/viewer/IViewerPlugin.h の MinHostVersion 節を参照)。
  Q_PLUGIN_METADATA(IID FarmanIViewerPlugin_iid FILE "metadata.json")
  Q_INTERFACES(Farman::IViewerPlugin)

public:
  ModelViewerPlugin()           = default;
  ~ModelViewerPlugin() override = default;

  QString pluginId() const override { return QStringLiteral("model_viewer"); }
  QString pluginName() const override { return QStringLiteral("3D モデルビュアー"); }
  QString author() const override { return QStringLiteral("Mashsoft Inc."); }
  QString authorUrl() const override { return QStringLiteral("https://www.mashsoft.co.jp"); }
  QString version() const override {
#ifdef THREED_PLUGIN_VERSION
    return QStringLiteral(THREED_PLUGIN_VERSION);
#else
    return QStringLiteral("dev");
#endif
  }
  // 外部プラグインは priority 0〜9999 (10000 以上は同梱公式の予約域)。
  int priority() const override { return 100; }

  QStringList supportedExtensions() const override { return {QStringLiteral("fbx")}; }
  QStringList supportedMimeTypes() const override { return {}; }

  bool     canHandle(const QString& filePath) const override;
  QWidget* createViewer(const QString& filePath, QWidget* parent,
                        const PluginContext& ctx) override;
};

} // namespace Farman

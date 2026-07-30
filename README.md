# farman-plugin-3d

farman 用の外部ビュアープラグイン。**3D モデル (.fbx) を farman 内で表示**します。
（まず FBX に対応。今後 obj / gltf などへ拡張予定 → v1.0.0）

- 描画: [Assimp](https://github.com/assimp/assimp) で読み込み、OpenGL 3.3 Core
  (QOpenGLWidget) で表示。テクスチャ (外部/埋め込み)・スケルタルアニメ・複数
  マテリアル・グリッド/座標軸・ワイヤーフレーム・オービット/自動フィットに対応。
- 成果物 `ModelViewerPlugin.(dylib|so|dll)` を farman の外部プラグインディレクトリ
  `<AppData>/plugins/viewers/` に置き、設定 → プラグインで「外部プラグインの
  読込みを許可する」をオンにして再起動すると有効になります。

## ビルド

```bash
cmake -B build -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/qt;/opt/homebrew/opt/assimp"
cmake --build build
# → build/dist/ModelViewerPlugin.dylib
```

依存: Qt6 (Core/Gui/Widgets/OpenGL/OpenGLWidgets) と Assimp。farman 本体と同じ
Qt バージョンでビルドすること (ABI 一致が必要)。

## ライセンス
MIT (farman 本体に準拠)。

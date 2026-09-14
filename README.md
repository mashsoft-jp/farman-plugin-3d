# farman-plugin-3d

farman 用の外部ビュアープラグイン。**3D モデル (.fbx) を farman 内で表示**します。
（まず FBX に対応。今後 obj / gltf などへ拡張予定 → v1.0.0）

- 描画: [Assimp](https://github.com/assimp/assimp) で読み込み、OpenGL 3.3 Core を
  オフスクリーン FBO に描画 → QImage 表示 (macOS の埋め込み合成問題を回避)。
  テクスチャ (外部/埋め込み)・スケルタルアニメ・複数マテリアル・ノード階層の
  変換 (インスタンス配置・鏡像) 適用・裏面カリング (Unity 等と同じ片面表示、
  切替可)・グリッド・ワイヤーフレーム (陰影なし)・ボーン (スケルトン) 表示・
  右上の座標軸ギズモ・キーボード操作・オービット/自動フィットに対応。
- 成果物 `ModelViewerPlugin.(dylib|so|dll)` を farman の外部プラグインディレクトリ
  `<AppData>/plugins/viewers/` に置き、設定 → プラグインで「外部プラグインの
  読込みを許可する」をオンにして再起動すると有効になります。

## ビルド (ローカル / macOS)

```bash
cmake -B build -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/qt;/opt/homebrew/opt/assimp"
cmake --build build
# → build/dist/ModelViewerPlugin.dylib
```

依存: Qt6 (Core/Gui/Widgets/**OpenGL**) と Assimp。**farman 本体と同じ Qt
バージョン**でビルドすること (ABI 一致が必要)。QOpenGLWidget は使わないため
OpenGLWidgets は不要。

## CI / 配布

`.github/workflows/` で 3 OS 分を自動ビルドする:

- **build.yml** — push / PR / 手動起動で macOS(arm64)・Linux(x86_64)・Windows(x64)
  をビルドし、成果物を Artifacts に上げる (Windows/Linux 動作確認用)。
- **release.yml** — `v*` タグ push で同じくビルドし、macOS は署名+公証して
  GitHub Releases に **draft** 添付 (確認後に手動 Publish)。

依存の扱い (重要):

- **assimp は farman 本体が使わない依存**なので、CI で **静的リンク**して
  プラグインを自己完結させる (配布物は単一ファイル)。CI は assimp `v6.0.4` を
  ソースから静的ビルドする (Unix はシステム zlib、Windows は同梱 zlib)。
- **Qt6OpenGL は farman 本体が同梱**する (本体 `CMakeLists.txt` に `Qt6::OpenGL`
  をリンク済み)。プラグインは farman プロセスに既ロードの Qt を再利用するため、
  Qt を同梱しない。macOS は Qt framework の参照を
  `@executable_path/../Frameworks/` に書き換えて farman.app 内へ解決させる。

⚠ プラグインが Qt6OpenGL を要求するため、**farman 本体は `Qt6::OpenGL` を同梱
したビルド**が必要 (2026-07-31 以降のビルド)。それ以前の配布 farman では
このプラグインはロードに失敗する。

## ライセンス
MIT (farman 本体に準拠)。

## ライセンス
MIT (farman 本体に準拠)。

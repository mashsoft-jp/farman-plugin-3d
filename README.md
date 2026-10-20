# farman-plugin-3d

farman 用の外部ビュアープラグイン。**3D モデル (.fbx / .obj / .gltf / .glb / .blend) を
farman 内で表示**します。

## 対応形式

| 形式 | 拡張子 | 内容 |
|---|---|---|
| FBX | `.fbx` | メッシュ・マテリアル・テクスチャ (外部/埋め込み)・骨格・スキニング・アニメ・ノード階層 |
| glTF | `.gltf` `.glb` | 同上 (`.glb` の埋め込みテクスチャ、`.gltf` の外部ファイルに対応) |
| OBJ | `.obj` | メッシュ・マテリアル (`.mtl`)・テクスチャ。骨格・アニメは形式として持たない |
| Blender | `.blend` | メッシュ・マテリアルのみ。骨格・アニメは読み込めない (assimp の制約)。新しい Blender で保存した `.blend` (圧縮形式など) は読めないことがある |

Blender は Z が上の座標系なので、読み込み時に Y 上へ回して立たせます。アニメが複数ある
ファイルは、先頭のクリップを再生します。ツールバーの選択ボックスか `[` / `]` キーで、
別のクリップに切り替えられます。

## 機能

- 描画: [Assimp](https://github.com/assimp/assimp) で読み込み、OpenGL 3.3 Core を
  オフスクリーン FBO に描画 → QImage 表示 (macOS の埋め込み合成問題を回避)。
  テクスチャ・スケルタルアニメ・複数マテリアル・ノード階層の変換 (インスタンス配置・
  鏡像)・裏面カリング (Unity 等と同じ片面表示、切替可)・グリッド・ワイヤーフレーム
  (陰影なし)・ボーン (スケルトン) 表示・右上の座標軸ギズモ・キーボード操作・
  オービット/自動フィットに対応。
- アニメの無い骨格付きモデルは、T ポーズではなく、ファイルに記録された関節の現在姿勢で
  表示します (Unity Editor の既定の見た目に近い)。
- **ノード情報パネル** (`i` キー / ツールバーの「i」ボタン、初期は非表示): ノード階層を
  接続線つきの開閉できるツリーで表示し、選んだノードの詳細 (種別・親子・位置/回転/
  スケール・メッシュの頂点数/三角形数/マテリアル・ボーンの影響頂点数) を出します。
  選んだノードは 3D ビューでも強調されます。モデル全体の情報は左上のオーバーレイに出ます。
## インストール

成果物 `ModelViewerPlugin.(dylib|so|dll)` を farman の外部プラグインディレクトリ
`<AppData>/plugins/viewers/` に置き、設定 → プラグインで「外部プラグインの
読込みを許可する」をオンにして再起動すると有効になります。

## ビルド (ローカル / macOS)

```bash
cmake -B build -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/qt;/opt/homebrew/opt/assimp"
cmake --build build
# → build/dist/ModelViewerPlugin.dylib
```

検証用に `build/model_poc` も作られる。`./build/model_poc <model> --uishot out.png` で画像を、
`--info-nodes` でノード情報を出せる (`--time <秒>` でアニメ時刻、`--infoshot` で
情報パネル付き、`--list-clips` / `--clip <番号>` でクリップ、`--yaw` / `--pitch` / `--dist` で視点、
`--size WxH` で画像サイズ)。

依存: Qt6 (Core/Gui/Widgets/**OpenGL**) と Assimp。**farman 本体と同じ Qt
バージョン**でビルドすること (ABI 一致が必要)。QOpenGLWidget は使わないため
OpenGLWidgets は不要。

## CI / 配布

`.github/workflows/` で 3 OS 分を自動ビルドする:

- **build.yml** — push / PR / 手動起動で macOS(arm64)・Linux(x86_64)・Windows(x64)
  をビルドし、成果物を Artifacts に上げる (動作確認用)。macOS は Developer ID で
  **署名のみ** (公証なし)。farman.app は Hardened Runtime でライブラリ検証が
  働くため、同じ Team ID の署名が無い dylib はロードできない。ブラウザ経由で
  ダウンロードして quarantine が付いた場合は `xattr -d com.apple.quarantine`
  が必要 (`gh run download` なら不要)。
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

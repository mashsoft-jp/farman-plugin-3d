# farman-sdk (vendored)

farman 本体から取り込んだ ABI ヘッダです。プラグインを farman 本体と同じ
インターフェースでビルドするために使います。

- `viewer/IViewerPlugin.h` — 外部ビュアープラグインのインターフェース
  (IID `com.farman.IViewerPlugin/<major>.<minor>`)。farman 本体の
  `src/viewer/IViewerPlugin.h` と同一。farman 側で IID メジャーが上がったら
  ここも更新すること。

注: `IViewerPlugin.cpp` は farman 内部 (Logger/Settings) に依存するため取り込ま
ない。既定実装が必要な `version()` / `canHandle()` はプラグイン側で override する。

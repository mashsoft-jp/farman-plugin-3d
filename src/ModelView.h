#pragma once

// 3D モデルビュアーの描画ウィジェット。
//  - Assimp 読み込み / ディフューズテクスチャ (外部・埋め込み) / UV
//  - スケルタルアニメーション (GPU スキニング, 4 ボーン加重)
//  - 複数マテリアル・メッシュ (サブメッシュ単位)
//  - ノード階層の変換をメッシュに適用 (インスタンス配置・鏡像に対応)
//  - グリッド + 座標軸 / ワイヤーフレーム / 裏面カリング / オービット・自動フィット
//
// 描画は「隠し QOpenGLContext + QOffscreenSurface + FBO にオフスクリーン描画し、
// 得た QImage を paintEvent で表示する」方式。ネイティブ GL 面 (QOpenGLWidget)
// を使わないため、macOS で QStackedWidget 等に埋め込んでも黒くならず正しく合成
// される (farman の動画ビュアーが QVideoWidget → QGraphicsVideoItem にした理由と
// 同じ問題への対処)。単体では farman-plugin-3d の中核として流用する。

#include <QElapsedTimer>
#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QQuaternion>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector3D>
#include <QWidget>
#include "viewer/ViewerShortcutMap.h"

#include <memory>
#include <vector>

class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLFramebufferObject;
class QOpenGLTexture;
class QTimer;

namespace Farman {

class ModelView : public QWidget, protected QOpenGLFunctions_3_3_Core {
  Q_OBJECT

public:
  explicit ModelView(QWidget* parent = nullptr);
  ~ModelView() override;

  bool loadModel(const QString& path, QString* error = nullptr);

  // 強制的に 1 フレーム描画して画像を返す (オフスクリーン検証 / スナップショット用)。
  QImage renderToImage();

  QString     summary() const { return m_summary; }
  QStringList infoLines() const { return m_infoLines; }  // 情報ダイアログ用 (ヘッダ + 階層の全文)
  struct InfoNode {
    QString label;
    int     depth = 0;
    int     node  = -1;  // m_nodes の番号 ([nodeInfoLines] に渡す)
  };
  // 指定ノードの詳細 (種別・親子・ローカル変換・メッシュの頂点数/三角形数など)。
  QStringList nodeInfoLines(int node) const;
  QStringList                  infoHeaderLines() const { return m_infoHeader; }  // 階層より前の行
  const std::vector<InfoNode>& infoTree() const { return m_infoTree; }           // ノード階層 (前順)

  // ── テクスチャ情報 ──
  bool        hasUV() const { return m_hasUV; }
  int         materialCount() const { return int(m_materials.size()); }
  bool        hasTexture() const;
  bool        hasEmbeddedTexture() const;
  QStringList recordedTexturePaths() const;
  QStringList resolvedTexturePaths() const;
  QStringList unresolvedTexturePaths() const;

  // ── アニメーション ──
  bool   hasAnimation() const { return m_hasAnim; }
  double animationDuration() const { return m_ticksPerSec > 0 ? m_animDurationTicks / m_ticksPerSec : 0; }

public slots:
  void setTextureEnabled(bool on);
  void setAnimationPlaying(bool on);
  void setAnimationTime(double sec);
  void setShowGrid(bool on);
  void setShowHelp(bool on);
  void setShowBones(bool on);
  // ノード階層で選択したノードを 3D ビューで強調する (-1 で解除)。
  //  メッシュ / グループ: 配下のメッシュをオレンジ寄りに色付けし、他を少し暗くする。
  //  ボーン: そのボーンが影響する範囲をウェイトに応じて色付けし、関節にリングと名前を出す。
  void setHighlightNode(int node);
  void setWireframe(bool on);
  void setCullBackface(bool on);  // 裏面を描かない (Unity 等と同じ片面表示)
  void resetView();

  bool showHelp() const { return m_showHelp; }
  bool wireframe() const { return m_wireframe; }
  bool cullBackface() const { return m_cullBackface; }
  bool hasSkeleton() const { return !m_boneOffset.empty(); }

signals:
  void textureEnabledChanged(bool on);
  void showGridChanged(bool on);
  void showHelpChanged(bool on);
  void showBonesChanged(bool on);
  void wireframeChanged(bool on);
  void cullBackfaceChanged(bool on);
  void animationPlayingChanged(bool on);
  void infoRequested();  // i キー / ツールバーの情報ボタン相当

protected:
  void paintEvent(QPaintEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void showEvent(QShowEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;

public:
  // 本体 (設定 → キーバインド) からショートカット割り当てを push で受け取る。
  // bindings は commandId -> キー文字列リスト。QMetaObject::invokeMethod で
  // 本体から一様に呼べるよう Q_INVOKABLE。
  Q_INVOKABLE void applyShortcutBindings(const QVariantMap& bindings);

private:
  // commandId (viewer.model.*) に対応する操作を実行。処理したら true。
  bool dispatchShortcut(const QString& commandId);
  bool ensureContext();
  void ensureGLResources();
  void uploadIfNeeded();
  void buildGrid();
  void buildInfoText();
  void renderFrame();  // makeCurrent → FBO 描画 → toImage → update()
  void computeBoneMatrices(double timeTicks, bool bindPose);
  std::vector<QMatrix4x4> poseGlobals(const std::vector<QMatrix4x4>& locals) const;

  // 頂点: 位置3 + 法線3 + UV2 + boneID4 + weight4 = 16 float
  static constexpr int kFloatsPerVertex = 16;
  std::vector<float>        m_vertices;
  std::vector<unsigned int> m_indices;
  QVector3D                 m_center{0, 0, 0};
  float                     m_radius   = 1.0f;
  bool                      m_hasModel = false;
  bool                      m_uploaded = false;
  bool                      m_hasUV    = false;
  bool                      m_texEnabled = true;
  bool                      m_showGrid   = true;
  bool                      m_showHelp   = true;
  bool                      m_showBones  = false;
  bool                      m_wireframe  = false;
  bool                      m_cullBackface = true;
  QVector3D                 m_bboxMin{0, 0, 0};
  QVector3D                 m_bboxMax{0, 0, 0};
  QString                   m_summary;

  // グリッド + 座標軸 (unlit ライン)
  std::vector<float>       m_lineVerts;
  int                      m_lineCount = 0;
  bool                     m_linesUploaded = false;

  // ── マテリアル / サブメッシュ ──
  struct Material {
    QVector3D                       baseColor{0.72f, 0.74f, 0.78f};
    bool                            hasTexture = false;
    bool                            embedded   = false;
    bool                            resolved   = false;
    QString                         recordedPath;
    QString                         resolvedPath;
    QImage                          image;
    std::unique_ptr<QOpenGLTexture> tex;
  };
  struct SubMesh {
    int indexOffset = 0;
    int indexCount  = 0;
    int material    = 0;
    int node        = -1;  // このサブメッシュを参照するノード (m_nodes の番号)
  };
  std::vector<Material> m_materials;
  std::vector<SubMesh>  m_submeshes;

  // ノード選択の強調表示 ([setHighlightNode])。
  int                m_hiJoint   = -1;     // リングを出すボーンのノード番号
  bool               m_hiActive  = false;  // メッシュの色付け (選択配下) + 他を暗くする
  bool               m_hiBoneOn  = false;  // ボーンのウェイトによる色付け
  std::vector<char>  m_hiSub;              // サブメッシュごとに「選択配下か」
  std::vector<float> m_hiBone;             // ボーン ID ごとの 0/1 (シェーダの uHiBone)

  // ── スケルトン / アニメーション ──
  // ノードが参照するメッシュ 1 つ分の詳細 (情報ウィンドウ用)。
  struct MeshDetail {
    QString name;
    int     vertices = 0;
    int     triangles = 0;
    int     bones    = 0;  // このメッシュに影響するボーンの数
    QString material;
    bool    skinned = false;
  };
  struct Node {
    QString    name;
    QMatrix4x4 bind;
    int        parent = -1;
    // FBX の InheritType=Rrs (親のスケールを継承しない) の関節。グローバル変換を
    // 求めるとき、親の変換からスケール成分を除いて合成する ([composeGlobal])。
    bool noParentScale = false;
    // スキンのバインドポーズ (逆バインド行列の逆) から求めた、ボーンの「静止時」配置。
    // 位置・スケールはここから取り、ノードの現在姿勢からは回転だけを取る ([poseGlobals])。
    bool       isBone     = false;
    int        boneParent = -1;  // 最も近い祖先ボーンのノード番号
    QMatrix4x4 restRel;          // 祖先ボーン (無ければ原点) から見た静止時の変換
    QMatrix4x4 bindGlobal;       // バインド時のボーンのワールド配置 (メッシュ変換 * offset の逆)
    std::vector<MeshDetail> meshes;  // このノードが参照するメッシュの詳細
    int        boneWeights = 0;      // ボーンが影響する頂点の延べ数 (ウェイトの個数)
    int        boneMeshes  = 0;      // ボーンが影響するメッシュの数
    QStringList meshInfo;        // このノードが参照するメッシュ名 (情報ウィンドウの階層表示用)
  };
  struct Vec3Key { double t; QVector3D v; };
  struct QuatKey { double t; QQuaternion q; };
  struct Channel {
    bool                 used = false;
    std::vector<Vec3Key> pos;
    std::vector<QuatKey> rot;
    std::vector<Vec3Key> scale;
  };
  std::vector<Node>       m_nodes;
  std::vector<Channel>    m_channels;
  std::vector<QMatrix4x4> m_boneOffset;
  std::vector<int>        m_boneNode;
  // ボーンのワールド配置を、ボーン自身の平均スケールで割って 1 に正規化する変換
  // (スケール 1 のモデルでは単位行列)。センチ/メートル換算を含む FBX では、ボーンの
  // 寸法がメッシュの寸法とずれて記録されることがあり、そのまま姿勢を変えると関節の
  // 支点がメッシュとずれて腕などが伸びる。ボーンを正規化した寸法に揃えると、
  // スキニング結果はメッシュと整合する (全体が一様に拡大縮小されるだけ)。
  QMatrix4x4              m_boneNorm;
  float                   m_boneS0 = 1.0f;  // 正規化前のボーンの平均スケール
  QMatrix4x4              m_meshNorm;       // ボーンを祖先に持たない静的メッシュの寸法をそろえる変換
  // アニメ無しのスキン付きモデル (ノードの現在姿勢で静止描画) か。このときボーンは
  // 名前ごとに 1 つで、最終行列 = globalInverse * 現在配置 * inverse(バインド配置)。
  // メッシュごとの違い (メッシュ変換) は頂点に焼き込む。
  bool                    m_staticSkin = false;

  // ── スケルトン可視化 (関節を 2D 投影して paintEvent で重ねる) ──
  std::vector<char>       m_isBoneNode;    // ノードがボーンか
  std::vector<int>        m_boneAncestor;  // 各ノードの最近ボーン祖先ノード (無ければ -1)
  std::vector<QVector3D>  m_jointWorld;    // ノードのワールド座標 (computeBoneMatrices で更新)
  QMatrix4x4              m_lastMvp;        // 直近フレームの MVP (投影用)
  QMatrix4x4              m_globalInverse;
  std::vector<QMatrix4x4> m_boneMatrices;
  double                  m_animDurationTicks = 0.0;
  double                  m_ticksPerSec       = 25.0;
  bool                    m_hasAnim   = false;
  bool                    m_playing   = true;
  double                  m_animTimeSec = 0.0;
  qint64                  m_lastMs      = 0;
  QElapsedTimer           m_clock;
  QTimer*                 m_animTimer = nullptr;

  // ── オフスクリーン GL ──
  QOpenGLContext*           m_ctx     = nullptr;
  QOffscreenSurface*        m_surface = nullptr;
  QOpenGLFramebufferObject* m_fbo     = nullptr;
  bool                      m_glResourcesReady = false;
  QImage                    m_frame;

  QOpenGLShaderProgram     m_prog;
  QOpenGLShaderProgram     m_lineProg;
  QOpenGLBuffer            m_vbo{QOpenGLBuffer::VertexBuffer};
  QOpenGLBuffer            m_ibo{QOpenGLBuffer::IndexBuffer};
  QOpenGLVertexArrayObject m_vao;
  QOpenGLBuffer            m_lineVbo{QOpenGLBuffer::VertexBuffer};
  QOpenGLVertexArrayObject m_lineVao;

  // オービットカメラ
  float     m_yaw   = 0.7f;
  float     m_pitch = 0.35f;
  float     m_dist  = 2.6f;
  QVector3D m_pan{0, 0, 0};  // 平行移動 (ワールド)
  QPoint    m_lastPos;

  // 本体から push されたショートカット割り当て (ローカル保持。ストレージは読まない)。
  ViewerShortcutMap m_shortcuts;

  // 表示補助 (右上ギズモ / 情報テキスト)
  QStringList m_infoLines;
  QStringList m_infoHeader;
  std::vector<InfoNode> m_infoTree;
  QString     m_filePath;
};

} // namespace Farman

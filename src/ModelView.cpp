#include "ModelView.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QMap>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLTexture>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector4D>
#include <QWheelEvent>
#include <QtMath>

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace Farman {

namespace {

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aBoneIDs;
layout(location = 4) in vec4 aWeights;
const int MAX_BONES = 128;
uniform mat4 uMVP;
uniform mat3 uNormalView;
uniform mat4 uBones[MAX_BONES];
uniform bool uSkinned;
uniform float uHiBone[MAX_BONES];  // 強調するボーン (1) かどうか
uniform bool uHiBoneOn;
out vec3 vNormalView;
out vec2 vUV;
out float vHi;                     // 強調ボーンのウェイト (0〜1)
void main() {
  vec4 pos = vec4(aPos, 1.0);
  vec3 nrm = aNormal;
  float wsum = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
  if (uSkinned && wsum > 0.0001) {
    mat4 skin = aWeights.x * uBones[int(aBoneIDs.x)] +
                aWeights.y * uBones[int(aBoneIDs.y)] +
                aWeights.z * uBones[int(aBoneIDs.z)] +
                aWeights.w * uBones[int(aBoneIDs.w)];
    pos = skin * pos;
    nrm = mat3(skin) * aNormal;
  }
  vNormalView = uNormalView * nrm;
  vUV = aUV;
  vHi = 0.0;
  if (uHiBoneOn) {
    vHi = clamp(aWeights.x * uHiBone[int(aBoneIDs.x)] + aWeights.y * uHiBone[int(aBoneIDs.y)] +
                aWeights.z * uHiBone[int(aBoneIDs.z)] + aWeights.w * uHiBone[int(aBoneIDs.w)], 0.0, 1.0);
  }
  gl_Position = uMVP * pos;
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec3 vNormalView;
in vec2 vUV;
in float vHi;
uniform sampler2D uTex;
uniform bool uUseTexture;
uniform bool uUnlit;      // シェーディングを無くす (ワイヤーフレーム用)
uniform vec3 uBaseColor;
uniform float uTint;      // 選択中のメッシュ: オレンジへの混ぜ具合 (0 / 1)
uniform float uDim;       // 選択外のメッシュの明るさ (1 = そのまま)
out vec4 fragColor;
const vec3 kHi = vec3(1.0, 0.55, 0.10);
void main() {
  vec3  base = uUseTexture ? texture(uTex, vec2(vUV.x, 1.0 - vUV.y)).rgb : uBaseColor;
  float t    = clamp(uTint * 0.65 + vHi * 0.75, 0.0, 0.85);
  if (uUnlit) { fragColor = vec4(mix(base * uDim, kHi, t), 1.0); return; }
  vec3 N = normalize(vNormalView);
  if (N.z < 0.0) N = -N;
  float head = max(N.z, 0.0);
  float fill = max(dot(N, normalize(vec3(0.3, 0.6, 0.5))), 0.0) * 0.35;
  float lit  = 0.28 + 0.72 * head + fill;
  fragColor  = vec4(mix(base * lit * uDim, kHi * (0.55 + 0.45 * lit), t), 1.0);
}
)";

const char* kLineVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vColor;
void main() { vColor = aColor; gl_Position = uMVP * vec4(aPos, 1.0); }
)";

const char* kLineFragmentShader = R"(#version 330 core
in vec3 vColor;
out vec4 fragColor;
void main() { fragColor = vec4(vColor, 1.0); }
)";

QMatrix4x4 toQM(const aiMatrix4x4& m) {
  return QMatrix4x4(m.a1, m.a2, m.a3, m.a4, m.b1, m.b2, m.b3, m.b4, m.c1, m.c2, m.c3, m.c4,
                    m.d1, m.d2, m.d3, m.d4);
}

// 行列から回転・平行移動だけを残す (基底ベクトルを正規化してスケールを除く)。
QMatrix4x4 stripScale(const QMatrix4x4& m) {
  QMatrix4x4 r = m;
  for (int c = 0; c < 3; ++c) {
    QVector3D axis(m(0, c), m(1, c), m(2, c));
    const float len = axis.length();
    if (len > 1e-12f) axis /= len;
    r(0, c) = axis.x();
    r(1, c) = axis.y();
    r(2, c) = axis.z();
  }
  return r;
}

// ノードの整数メタデータ (FBX の InheritType など) を読む。無ければ fallback。
int nodeIntMeta(const aiNode* n, const char* key, int fallback) {
  if (!n->mMetaData) return fallback;
  for (unsigned int i = 0; i < n->mMetaData->mNumProperties; ++i) {
    if (n->mMetaData->mKeys[i] != aiString(key)) continue;
    const aiMetadataEntry& e = n->mMetaData->mValues[i];
    switch (e.mType) {
      case AI_INT32:  return *static_cast<int32_t*>(e.mData);
      case AI_UINT32: return int(*static_cast<uint32_t*>(e.mData));
      case AI_INT64:  return int(*static_cast<int64_t*>(e.mData));
      case AI_UINT64: return int(*static_cast<uint64_t*>(e.mData));
      default:        return fallback;
    }
  }
  return fallback;
}

bool isFbxPseudoNode(const aiNode* n) {
  return std::strstr(n->mName.C_Str(), "$AssimpFbx$") != nullptr;
}

// 親のグローバル変換と子のローカル変換からグローバル変換を作る。noParentScale の
// ノードは親のスケールを引き継がない (FBX InheritType=Rrs)。
// 線形部 (3x3) だけを残し、基底ベクトルを正規化する (回転成分だけを取り出す)。
QMatrix4x4 normalizedLinear(const QMatrix4x4& m) {
  QMatrix4x4 r;
  for (int c = 0; c < 3; ++c) {
    QVector3D axis(m(0, c), m(1, c), m(2, c));
    const float len = axis.length();
    if (len > 1e-12f) axis /= len;
    r(0, c) = axis.x();
    r(1, c) = axis.y();
    r(2, c) = axis.z();
  }
  return r;
}

float columnLength(const QMatrix4x4& m, int c) {
  return QVector3D(m(0, c), m(1, c), m(2, c)).length();
}

// 親のスケールは子の「配置」(平行移動) にだけ効き、子の向き・形 (線形部) には
// 効かない。
QMatrix4x4 composeGlobal(const QMatrix4x4& parent, const QMatrix4x4& local, bool noParentScale) {
  QMatrix4x4 g = parent * local;
  if (!noParentScale) return g;
  const QMatrix4x4 lin = stripScale(parent) * local;  // 線形部 = 親の回転 * 子のローカル
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      g(r, c) = lin(r, c);
    }
  }
  return g;
}

// 外部テクスチャの実ファイルを探す。FBX には Windows 絶対パスが記録されている
// ことが多いため、記録パスそのもの → FBX と同じ場所 → ファイル名だけ → textures/
// サブフォルダ、の順に候補を当たって最初に存在したものの絶対パスを返す。
QString resolveTexturePath(const QString& fbxPath, const QString& recorded) {
  QString rec = recorded;
  rec.replace('\\', '/');  // Windows 区切りを正規化
  const QDir        dir(QFileInfo(fbxPath).absolutePath());
  const QString     base = QFileInfo(rec).fileName();
  const QStringList candidates = {rec, dir.absoluteFilePath(rec), dir.absoluteFilePath(base),
                                  dir.absoluteFilePath("textures/" + base),
                                  dir.absoluteFilePath("../textures/" + base)};
  for (const QString& c : candidates) {
    if (QFileInfo::exists(c)) return QFileInfo(c).absoluteFilePath();
  }
  return QString();
}

template <typename Key, typename Val>
Val interpKeys(const std::vector<Key>& keys, double t, Val fallback) {
  if (keys.empty()) return fallback;
  if (keys.size() == 1 || t <= keys.front().t) return keys.front().v;
  if (t >= keys.back().t) return keys.back().v;
  for (size_t i = 0; i + 1 < keys.size(); ++i) {
    if (t < keys[i + 1].t) {
      const double f = (t - keys[i].t) / (keys[i + 1].t - keys[i].t);
      return keys[i].v * (1.0f - float(f)) + keys[i + 1].v * float(f);
    }
  }
  return keys.back().v;
}

} // namespace

ModelView::ModelView(QWidget* parent) : QWidget(parent) {
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(64, 64);
  m_animTimer = new QTimer(this);
  m_animTimer->setInterval(16);
  connect(m_animTimer, &QTimer::timeout, this, [this] {
    if (m_hasAnim && m_playing) renderFrame();
  });
}

ModelView::~ModelView() {
  if (m_ctx) {
    m_ctx->makeCurrent(m_surface);
    for (auto& m : m_materials) m.tex.reset();
    m_vbo.destroy();
    m_ibo.destroy();
    m_vao.destroy();
    m_lineVbo.destroy();
    m_lineVao.destroy();
    delete m_fbo;
    m_fbo = nullptr;
    m_ctx->doneCurrent();
  }
  delete m_surface;
  delete m_ctx;
}

// ── マテリアル横断のテクスチャ集計 ──
// マテリアルごとに独立してテクスチャを持つため、ツールバーや情報ダイアログ用に
// 「どれか1つでもテクスチャ/埋め込みがあるか」「解決済み/未解決の外部パス一覧」
// をまとめて返す。パスは重複除去する。
bool ModelView::hasTexture() const {
  for (const auto& m : m_materials) {
    if (m.hasTexture) return true;
  }
  return false;
}
bool ModelView::hasEmbeddedTexture() const {
  for (const auto& m : m_materials) {
    if (m.embedded) return true;
  }
  return false;
}
// FBX に記録された外部テクスチャのパス (解決の成否は問わない)。
QStringList ModelView::recordedTexturePaths() const {
  QStringList out;
  for (const auto& m : m_materials) {
    if (m.hasTexture && !m.embedded && !out.contains(m.recordedPath)) out << m.recordedPath;
  }
  return out;
}
// 実ファイルが見つかって読み込めた外部テクスチャの絶対パス。
QStringList ModelView::resolvedTexturePaths() const {
  QStringList out;
  for (const auto& m : m_materials) {
    if (m.resolved && !m.resolvedPath.isEmpty() && !out.contains(m.resolvedPath))
      out << m.resolvedPath;
  }
  return out;
}
// 記録はあるが実ファイルを見つけられなかった外部テクスチャのパス。
QStringList ModelView::unresolvedTexturePaths() const {
  QStringList out;
  for (const auto& m : m_materials) {
    if (m.hasTexture && !m.embedded && !m.resolved && !out.contains(m.recordedPath))
      out << m.recordedPath;
  }
  return out;
}

// Assimp でモデルを読み込み、描画に必要な情報を構築する:
//   ノード階層 → マテリアル/テクスチャ → ジオメトリ+スキンウェイト →
//   アニメーションチャンネル → スケルトン事前計算 → AABB/自動フィット。
// aiProcess_PreTransformVertices は使わない (ボーン情報が壊れるため)。
bool ModelView::loadModel(const QString& path, QString* error) {
  Assimp::Importer importer;
  const unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                             aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights |
                             aiProcess_ImproveCacheLocality;
  const aiScene* scene = importer.ReadFile(path.toUtf8().constData(), flags);
  if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
    if (error) {
      *error = QString::fromUtf8(importer.GetErrorString());
      if (path.endsWith(QLatin1String(".blend"), Qt::CaseInsensitive)) {
        // Blender の読み込みは assimp の制約が大きい (新しい形式・圧縮された .blend は読めないことがある)。
        *error += QStringLiteral("\n(新しい Blender で保存した .blend は読めないことがあります。FBX や glTF に書き出してお試しください)");
      }
    }
    return false;
  }

  const bool animated = scene->mNumAnimations > 0;
  const bool isBlend  = path.endsWith(QLatin1String(".blend"), Qt::CaseInsensitive);

  // ── ノード階層 (DFS 前順) ──
  m_nodes.clear();
  std::unordered_map<std::string, int> nodeIndex;
  std::vector<const aiNode*>           nodePtr;     // m_nodes と同順の aiNode
  std::vector<QMatrix4x4>              nodeGlobal;  // 各ノードのグローバル (ルート基準) 変換
  {
    struct Item { const aiNode* n; int parent; };
    std::vector<Item> stack{{scene->mRootNode, -1}};
    while (!stack.empty()) {
      Item it = stack.back();
      stack.pop_back();
      const int        idx   = int(m_nodes.size());
      QMatrix4x4 local = toQM(it.n->mTransformation);
      if (it.parent < 0 && isBlend) {
        // Blender は Z が上。assimp はこれを変換しないので、ルートで Y 上に回して立たせる。
        QMatrix4x4 zUpToYUp;
        zUpToYUp.rotate(-90.0f, 1.0f, 0.0f, 0.0f);
        local = zUpToYUp * local;
      }
      m_nodes.push_back({QString::fromUtf8(it.n->mName.C_Str()), local, it.parent});
      nodePtr.push_back(it.n);
      nodeGlobal.push_back(local);  // 仮置き。継承ルールを反映して下で作り直す
      // 同名ノードがある場合 (FBX でボーン "Body" とメッシュノード "Body" が衝突する等)、
      // ボーン/アニメの参照先はメッシュを持たない方を優先する。
      auto dup = nodeIndex.find(it.n->mName.C_Str());
      if (dup == nodeIndex.end() || (nodePtr[dup->second]->mNumMeshes > 0 && it.n->mNumMeshes == 0)) {
        nodeIndex[it.n->mName.C_Str()] = idx;
      }
      for (int c = int(it.n->mNumChildren) - 1; c >= 0; --c)
        stack.push_back({it.n->mChildren[c], idx});
    }
  }
  // FBX の InheritType=Rrs (値 2) の関節は、親のスケールを自分の向き・形に継承しない
  // ([composeGlobal])。Maya 出力の関節は個々に Lcl Scaling=0.01 を持つことがあり、
  // 素直に親子で掛けると階層が深いほどスケールが累積して潰れる (手・剣が 1 点に潰れて
  // 見えなくなる)。assimp は
  // 関節の前に $AssimpFbx$ の補助ノード (Translation/PreRotation/Scaling) を挟み、
  // InheritType は実ノードのメタデータにしか載らない。補助ノードは先頭の子 (= 連鎖の
  // 先) のフラグを引き継ぎ、実ノードを親に持つ連鎖の先頭だけが「親のスケールを除く」。
  {
    std::vector<char> rrs(m_nodes.size(), 0);
    for (size_t i = m_nodes.size(); i-- > 0;) {
      const aiNode* n = nodePtr[i];
      if (isFbxPseudoNode(n)) {
        rrs[i] = n->mNumChildren > 0 ? rrs[nodeIndex[n->mChildren[0]->mName.C_Str()]] : 0;
      } else {
        rrs[i] = nodeIntMeta(n, "InheritType", 1) == 2;
      }
    }
    for (size_t i = 0; i < m_nodes.size(); ++i) {
      const int p = m_nodes[i].parent;
      m_nodes[i].noParentScale = rrs[i] && p >= 0 && !isFbxPseudoNode(nodePtr[p]);
    }
  }
  m_globalInverse = toQM(scene->mRootNode->mTransformation).inverted();

  std::vector<QMatrix4x4> plainGlobal;  // ボーンを介さない素直な階層合成 (メッシュ変換の算出に使う)

  // ── ボーンのバインド配置 ──
  // スキンの offset 行列は「メッシュ空間→ボーン空間」の逆バインドで、メッシュ自身の
  // ワールド変換を含む (offset = inverse(ボーンのバインド配置) * メッシュ変換)。
  // よって メッシュ変換 * inverse(offset) がボーンのバインド時のワールド配置になり、
  // どのメッシュから求めても一致する。この値を土台に、位置・スケールはバインドポーズ、
  // 回転はノードの現在姿勢、という姿勢を作る ([poseGlobals])。FBX のノード階層から
  // スケールや平行移動まで再現しようとすると、InheritType や単位換算で破綻する。
  {
    std::vector<QMatrix4x4>& plain = plainGlobal;
    plain.assign(m_nodes.size(), QMatrix4x4());
    for (size_t i = 0; i < m_nodes.size(); ++i) {
      const int p = m_nodes[i].parent;
      plain[i]    = p < 0 ? m_nodes[i].bind
                          : composeGlobal(plain[p], m_nodes[i].bind, m_nodes[i].noParentScale);
    }
    for (size_t i = 0; i < nodePtr.size(); ++i) {
      for (unsigned int mr = 0; mr < nodePtr[i]->mNumMeshes; ++mr) {
        const unsigned int mi = nodePtr[i]->mMeshes[mr];
        if (mi >= scene->mNumMeshes) continue;
        const aiMesh* mesh = scene->mMeshes[mi];
        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
          auto it = nodeIndex.find(mesh->mBones[b]->mName.C_Str());
          if (it == nodeIndex.end() || m_nodes[it->second].isBone) continue;
          m_nodes[it->second].isBone     = true;
          m_nodes[it->second].bindGlobal = plain[i] * toQM(mesh->mBones[b]->mOffsetMatrix).inverted();
        }
      }
    }
    // ボーンの平均スケールで正規化する (スケール 1 なら何もしない)。
    {
      double sum = 0.0;
      int    cnt = 0;
      for (const Node& nd : m_nodes) {
        if (!nd.isBone) continue;
        sum += columnLength(nd.bindGlobal, 0);
        ++cnt;
      }
      const float s0 = cnt > 0 ? float(sum / cnt) : 1.0f;
      m_boneS0       = s0;
      m_boneNorm.setToIdentity();
      if (s0 > 1e-12f && std::abs(s0 - 1.0f) > 1e-3f) {
        m_boneNorm.scale(1.0f / s0);
        for (Node& nd : m_nodes) {
          if (nd.isBone) nd.bindGlobal = m_boneNorm * nd.bindGlobal;
        }
      }
    }
    {
      // スキン付きメッシュのノードのスケールの中央値 (平均は、桁違いのスケールを持つ
      // ノードが混ざると引きずられる)。
      std::vector<float> scales;
      for (size_t i = 0; i < nodePtr.size(); ++i) {
        for (unsigned int mr = 0; mr < nodePtr[i]->mNumMeshes; ++mr) {
          const unsigned int mi = nodePtr[i]->mMeshes[mr];
          if (mi >= scene->mNumMeshes || scene->mMeshes[mi]->mNumBones == 0) continue;
          scales.push_back(columnLength(plain[i], 0));
        }
      }
      std::sort(scales.begin(), scales.end());
      const float sRef = scales.empty() ? 1.0f : scales[scales.size() / 2];
      m_meshNorm.setToIdentity();
      if (sRef > 1e-12f && std::abs(sRef - 1.0f) > 1e-3f) m_meshNorm.scale(1.0f / sRef);
    }
    for (size_t i = 0; i < m_nodes.size(); ++i) {
      const int p = m_nodes[i].parent;
      m_nodes[i].boneParent = p < 0 ? -1 : (m_nodes[p].isBone ? p : m_nodes[p].boneParent);
      if (m_nodes[i].isBone) {
        const int bp       = m_nodes[i].boneParent;
        m_nodes[i].restRel = bp >= 0 ? m_nodes[bp].bindGlobal.inverted() * m_nodes[i].bindGlobal
                                     : m_nodes[i].bindGlobal;
      }
    }
    // メッシュに焼き込むノードのグローバル変換は、バインド姿勢 (ノードの現在姿勢) で作る。
    std::vector<QMatrix4x4> locals(m_nodes.size());
    for (size_t i = 0; i < m_nodes.size(); ++i) {
      locals[i] = m_nodes[i].bind;
    }
    nodeGlobal = poseGlobals(locals);
  }

  // ── マテリアル ──
  m_materials.clear();
  m_materials.resize(scene->mNumMaterials);
  for (unsigned int mi = 0; mi < scene->mNumMaterials; ++mi) {
    const aiMaterial* mat = scene->mMaterials[mi];
    Material&         out = m_materials[mi];
    aiColor3D         col(0, 0, 0);
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, col) == AI_SUCCESS && (col.r + col.g + col.b) > 0.05f)
      out.baseColor = QVector3D(col.r, col.g, col.b);
    // ディフューズテクスチャの解決。参照文字列が "*N" なら FBX に埋め込まれた
    // テクスチャ (aiScene::mTextures[N])、そうでなければ外部ファイルパス。
    aiString tp;
    if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &tp) == AI_SUCCESS && tp.length > 0) {
      out.hasTexture   = true;
      out.recordedPath = QString::fromUtf8(tp.C_Str());
      if (out.recordedPath.startsWith('*')) {
        // 埋め込みテクスチャ。
        out.embedded  = true;
        const int idx = out.recordedPath.mid(1).toInt();
        if (idx >= 0 && idx < int(scene->mNumTextures)) {
          const aiTexture* t = scene->mTextures[idx];
          if (t->mHeight == 0) {
            // 圧縮済み (PNG/JPEG 等) が丸ごと入っている。mWidth はバイト長。
            out.image.loadFromData(reinterpret_cast<const uchar*>(t->pcData), int(t->mWidth));
          } else {
            // 非圧縮の生 BGRA ピクセル (mWidth x mHeight)。QImage は ARGB 順なので
            // rgbSwapped() で並べ替え、pcData の寿命に依存しないよう copy() する。
            out.image = QImage(reinterpret_cast<const uchar*>(t->pcData), int(t->mWidth),
                               int(t->mHeight), QImage::Format_ARGB32)
                            .rgbSwapped()
                            .copy();
          }
          out.resolved = !out.image.isNull();
        }
      } else {
        // 外部ファイル。FBX の場所を基準に探索して読み込む ([resolveTexturePath])。
        out.resolvedPath = resolveTexturePath(path, out.recordedPath);
        if (!out.resolvedPath.isEmpty() && out.image.load(out.resolvedPath)) out.resolved = true;
      }
    }
  }
  if (m_materials.empty()) m_materials.resize(1);

  // アニメ無しで、ボーンを持つ (= 姿勢を現在のノード姿勢で作る) モデルか。
  bool anySkinnedMesh = false;
  for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
    anySkinnedMesh = anySkinnedMesh || scene->mMeshes[mi]->mNumBones > 0;
  }
  const bool willAnimate = animated && scene->mAnimations[0]->mDuration > 0.0 && anySkinnedMesh;
  m_staticSkin           = anySkinnedMesh && !willAnimate;
  const bool animatedSkin = anySkinnedMesh && willAnimate;  // アニメ付きのスキンモデル

  // ── ジオメトリ + サブメッシュ + スキンウェイト ──
  std::vector<float>        verts;
  std::vector<unsigned int> indices;
  m_submeshes.clear();
  m_hiJoint = -1;
  m_hiActive = m_hiBoneOn = false;
  m_hiSub.clear();
  QVector3D lo(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
              std::numeric_limits<float>::max());
  QVector3D hi(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
              -std::numeric_limits<float>::max());
  unsigned int triCount = 0;
  bool         anyUV    = false;

  m_boneOffset.clear();
  m_boneNode.clear();
  std::unordered_map<std::string, std::vector<int>> boneId;

  // メッシュは aiScene::mMeshes を直接並べるのではなく、ノード階層をたどって
  // 「各ノードが参照するメッシュ」を、そのノードのグローバル変換を頂点に焼き込んで
  // 追加する。FBX (DCC ツール出力) では同じ形状を複数ノードから参照 (インスタンス
  // 配置) し、位置・回転・スケールはノード側が持つのが普通で、これを無視すると全部が
  // 原点に重なって Unity Editor 等の見た目と一致しない。スキン付きメッシュは
  // ボーン行列 (offset 行列にメッシュノードの変換を含む) が配置を担うので変換しない。
  // 鏡像変換 (行列式 < 0) は面の向きが反転するので、インデックス順を入れ替えて
  // 表裏 (CCW) を保つ。
  for (size_t ni = 0; ni < nodePtr.size(); ++ni) {
    const aiNode* node = nodePtr[ni];
    for (unsigned int mr = 0; mr < node->mNumMeshes; ++mr) {
      const unsigned int mi = node->mMeshes[mr];
      if (mi >= scene->mNumMeshes) continue;
      const aiMesh*    mesh    = scene->mMeshes[mi];
      // スキン付きメッシュはボーン行列 (offset にメッシュの変換を含む) が配置を担う。
      // アニメの有無に関わらず、ノードの現在姿勢でスキニングして描画する。
      const bool       skinned = mesh->mNumBones > 0;
      m_nodes[ni].meshInfo << QString::fromUtf8(mesh->mName.C_Str()) +
                                  (skinned ? QStringLiteral(" (スキン)") : QString());
      // ボーンを祖先に持たない静的メッシュは、スキン付きメッシュと同じ寸法にそろえる。
      const bool       underBone = m_nodes[ni].isBone || m_nodes[ni].boneParent >= 0;
      // アニメ付きのスキンモデルで、ボーン (の子) に付いた静的メッシュ (手に持たせた剣など) は、
      // 最も近い祖先ボーンにウェイト 1 で貼り付けて、アニメに追従させる。バインド時の位置は
      // ノードのバインド姿勢 ([plainGlobal]) で頂点に焼き込み、ボーン行列は
      // 現在配置 * inverse(バインド配置) (= offset) になる。
      int rigidBone = -1;
      if (!skinned && animatedSkin) {
        rigidBone = m_nodes[ni].isBone ? int(ni) : m_nodes[ni].boneParent;
      }
      QMatrix4x4 xf;
      if (skinned && m_staticSkin) {
        // 静止描画のスキン付きメッシュ: メッシュ変換を縮尺をそろえて頂点に焼き込む。
        // ボーン行列 = 現在配置 * inverse(バインド配置) にはメッシュ変換が含まれない。
        const float sMesh = columnLength(plainGlobal[ni], 0);
        QMatrix4x4  q;
        if (sMesh > 1e-12f) q.scale(1.0f / sMesh);
        xf = q * plainGlobal[ni];
      } else if (skinned) {
        xf = QMatrix4x4();  // アニメ付き: offset 行列がメッシュの配置を担う
      } else if (rigidBone >= 0) {
        xf = plainGlobal[ni];
      } else {
        xf = underBone ? nodeGlobal[ni] : m_meshNorm * nodeGlobal[ni];
      }
      const QMatrix4x4 nxf     = xf.inverted().transposed();  // 法線用 (逆転置)
      const bool       mirror  = !(skinned && !m_staticSkin) && xf.determinant() < 0.0;
      const bool    meshUV = mesh->HasTextureCoords(0);
      anyUV                = anyUV || meshUV;
      const unsigned int base = static_cast<unsigned int>(verts.size() / ModelView::kFloatsPerVertex);
      const int          subStart = int(indices.size());
      const unsigned int triBefore = triCount;

      std::vector<std::array<float, 4>> vid(mesh->mNumVertices, {0, 0, 0, 0});
      std::vector<std::array<float, 4>> vwt(mesh->mNumVertices, {0, 0, 0, 0});
      std::vector<int>                  cnt(mesh->mNumVertices, 0);
      if (rigidBone >= 0) {
        // 祖先ボーンへの剛体スキニング: この (名前, offset) のエントリを探し、無ければ作る。
        const std::string bname = nodePtr[rigidBone]->mName.C_Str();
        const QMatrix4x4  off   = plainGlobal[rigidBone].inverted();
        auto&             ids   = boneId[bname];
        int               id    = -1;
        for (int cand : ids) {
          bool same = true;
          for (int k = 0; k < 16 && same; ++k) {
            same = std::abs(m_boneOffset[cand].constData()[k] - off.constData()[k]) <= 1e-4f;
          }
          if (same) {
            id = cand;
            break;
          }
        }
        if (id < 0 && m_boneOffset.size() < 128) {
          id = int(m_boneOffset.size());
          ids.push_back(id);
          m_boneOffset.push_back(off);
          m_boneNode.push_back(rigidBone);
        }
        if (id >= 0) {
          for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            vid[v][0] = float(id);
            vwt[v][0] = 1.0f;
          }
        }
      }
      for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
        const aiBone*     bone = mesh->mBones[b];
        const std::string name = bone->mName.C_Str();
        {
          auto bn = nodeIndex.find(name);
          if (bn != nodeIndex.end()) {
            m_nodes[bn->second].boneWeights += int(bone->mNumWeights);
            ++m_nodes[bn->second].boneMeshes;
          }
        }
        // 同名ボーンでも逆バインド行列 (offset) はメッシュごとに異なることがある
        // (FBX のクラスタごとにメッシュ変換が違う場合)。名前だけで共有すると最初の
        // メッシュの行列が他のメッシュにも使われて位置がずれるので、名前 + 行列の
        // 組でエントリを分ける。
        const QMatrix4x4 offset = toQM(bone->mOffsetMatrix);
        int              id     = -1;
        auto&            ids    = boneId[name];
        // アニメ無しの静止描画はボーン名ごとに 1 つ (メッシュ差は頂点に焼き込み済み)。
        // アニメ付きは (名前, 行列) ごと。ただしシェーダの上限 (128) を超える分は、
        // 同名の先頭エントリを使い回す。
        if (m_staticSkin && !ids.empty()) {
          id = ids[0];
        } else if (!m_staticSkin && ids.size() > 0 && m_boneOffset.size() >= 128) {
          id = ids[0];
        }
        for (int cand : ids) {
          if (id >= 0) break;
          const QMatrix4x4& o = m_boneOffset[cand];
          bool              same = true;
          for (int k = 0; k < 16 && same; ++k) {
            same = std::abs(o.constData()[k] - offset.constData()[k]) <= 1e-4f;
          }
          if (same) {
            id = cand;
            break;
          }
        }
        if (id < 0) {
          id = int(m_boneOffset.size());
          ids.push_back(id);
          m_boneOffset.push_back(offset);
          auto found = nodeIndex.find(name);
          m_boneNode.push_back(found == nodeIndex.end() ? -1 : found->second);
        }
        for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
          const unsigned int v  = bone->mWeights[w].mVertexId;
          const float        wt = bone->mWeights[w].mWeight;
          if (cnt[v] < 4) {
            vid[v][cnt[v]] = float(id);
            vwt[v][cnt[v]] = wt;
            ++cnt[v];
          }
        }
      }

      for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
        const aiVector3D& p0 = mesh->mVertices[v];
        const aiVector3D  n0 = mesh->HasNormals() ? mesh->mNormals[v] : aiVector3D(0, 0, 1);
        const aiVector3D  t  = meshUV ? mesh->mTextureCoords[0][v] : aiVector3D(0, 0, 0);
        // ノード変換を頂点に焼き込む (スキン付きは xf = 単位行列なので変化なし)。
        const QVector3D p = xf.map(QVector3D(p0.x, p0.y, p0.z));
        QVector3D       n = nxf.mapVector(QVector3D(n0.x, n0.y, n0.z));
        if (n.lengthSquared() > 1e-12f) n.normalize();
        // ボーンウェイトは合計 1 に正規化しておく (シェーダ側は正規化を仮定)。
        std::array<float, 4> w = vwt[v];
        const float          s = w[0] + w[1] + w[2] + w[3];
        if (s > 0.0f) {
          for (float& x : w) {
            x /= s;
          }
        }
        // 1 頂点 = 位置3 + 法線3 + UV2 + boneID4 + weight4 = 16 float でインタリーブ。
        verts.insert(verts.end(), {p.x(), p.y(), p.z(), n.x(), n.y(), n.z(), t.x, t.y, vid[v][0],
                                   vid[v][1], vid[v][2], vid[v][3], w[0], w[1], w[2], w[3]});
        lo = QVector3D(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
        hi = QVector3D(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
      }
      for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace& face = mesh->mFaces[f];
        if (face.mNumIndices != 3) continue;
        indices.push_back(base + face.mIndices[0]);
        indices.push_back(base + face.mIndices[mirror ? 2 : 1]);
        indices.push_back(base + face.mIndices[mirror ? 1 : 2]);
        ++triCount;
      }
      {
        MeshDetail md;
        md.name      = QString::fromUtf8(mesh->mName.C_Str());
        md.vertices  = int(mesh->mNumVertices);
        md.triangles = int(triCount - triBefore);
        md.bones     = int(mesh->mNumBones);
        md.skinned   = skinned;
        if (mesh->mMaterialIndex < scene->mNumMaterials) {
          md.material = QString::fromUtf8(scene->mMaterials[mesh->mMaterialIndex]->GetName().C_Str());
        }
        m_nodes[ni].meshes.push_back(md);
      }
      SubMesh sm;
      sm.indexOffset = subStart;
      sm.indexCount  = int(indices.size()) - subStart;
      sm.material    = std::min<int>(mesh->mMaterialIndex, int(m_materials.size()) - 1);
      sm.node        = int(ni);
      if (sm.indexCount > 0) m_submeshes.push_back(sm);
    }  // for mr (ノードが参照するメッシュ)
  }  // for ni (ノード)

  if (verts.empty() || indices.empty()) {
    if (error) *error = QStringLiteral("メッシュが見つかりませんでした");
    return false;
  }

  // ── アニメーションチャンネル ──
  m_channels.assign(m_nodes.size(), Channel{});
  m_hasAnim           = false;
  m_animDurationTicks = 0.0;
  m_ticksPerSec       = 25.0;
  m_clips.clear();
  m_clip = 0;
  if (animated) {
    // すべてのクリップを読み込んでおき、先頭を既定で再生する (setAnimationClip で切替)。
    for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai) {
      const aiAnimation* anim = scene->mAnimations[ai];
      Clip               clip;
      clip.name          = QString::fromUtf8(anim->mName.C_Str());
      clip.durationTicks = anim->mDuration;
      clip.ticksPerSec   = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 25.0;
      clip.channels.assign(m_nodes.size(), Channel{});
      for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
        const aiNodeAnim* ch = anim->mChannels[c];
        auto              it = nodeIndex.find(ch->mNodeName.C_Str());
        if (it == nodeIndex.end()) continue;
        // ノード名でチャンネルを対応付け、位置 / 回転 / スケールのキーフレーム列を貯める。
        Channel& out = clip.channels[it->second];
        out.used     = true;
        for (unsigned int k = 0; k < ch->mNumPositionKeys; ++k) {
          out.pos.push_back({ch->mPositionKeys[k].mTime,
                             QVector3D(ch->mPositionKeys[k].mValue.x, ch->mPositionKeys[k].mValue.y,
                                       ch->mPositionKeys[k].mValue.z)});
        }
        for (unsigned int k = 0; k < ch->mNumRotationKeys; ++k) {
          const aiQuaternion& q = ch->mRotationKeys[k].mValue;
          out.rot.push_back({ch->mRotationKeys[k].mTime, QQuaternion(q.w, q.x, q.y, q.z)});
        }
        for (unsigned int k = 0; k < ch->mNumScalingKeys; ++k) {
          out.scale.push_back({ch->mScalingKeys[k].mTime,
                               QVector3D(ch->mScalingKeys[k].mValue.x, ch->mScalingKeys[k].mValue.y,
                                         ch->mScalingKeys[k].mValue.z)});
        }
      }
      m_clips.push_back(std::move(clip));
    }
  }
  selectClip(0);

  // スケルトン描画用の事前計算。ボーンに対応するノードに印を付け、各ノードから
  // 見た「最も近い祖先ボーンノード」を求めておく。これにより、間に挟まる非ボーンの
  // 変換ノードを飛ばして、ボーン同士だけを線でつなげる (paintEvent のスケルトン描画)。
  m_isBoneNode.assign(m_nodes.size(), 0);
  for (int bn : m_boneNode) {
    if (bn >= 0) m_isBoneNode[bn] = 1;
  }
  m_boneAncestor.assign(m_nodes.size(), -1);
  for (size_t i = 0; i < m_nodes.size(); ++i) {
    int p = m_nodes[i].parent;
    while (p >= 0 && !m_isBoneNode[p]) {
      p = m_nodes[p].parent;  // ボーンでない親はスキップして遡る
    }
    m_boneAncestor[i] = p;
  }
  m_jointWorld.clear();

  m_vertices = std::move(verts);
  m_indices  = std::move(indices);

  // スキン付きモデルは、静止頂点の AABB では実際に表示されるポーズと大きくずれる
  // ことがある (バインドポーズと初期ポーズが違う)。そこでバインドポーズのボーン行列で
  // CPU 側スキニングした頂点位置から AABB を取り直し、自動フィット (中心・半径) が
  // 破綻しないようにする。頂点フォーマットは 16 float/頂点 (末尾に boneID4 + weight4)。
  if (!m_boneOffset.empty()) {
    computeBoneMatrices(0.0, /*bindPose=*/true);
    lo = QVector3D(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max());
    hi = -lo;
    const int stride = ModelView::kFloatsPerVertex;
    for (size_t i = 0; i + stride <= m_vertices.size(); i += stride) {
      const QVector3D p(m_vertices[i], m_vertices[i + 1], m_vertices[i + 2]);
      const float w0 = m_vertices[i + 12], w1 = m_vertices[i + 13], w2 = m_vertices[i + 14],
                  w3 = m_vertices[i + 15];
      QVector3D sp = p;
      if (w0 + w1 + w2 + w3 > 0.0001f) {
        const int  id0 = int(m_vertices[i + 8]), id1 = int(m_vertices[i + 9]),
                  id2 = int(m_vertices[i + 10]), id3 = int(m_vertices[i + 11]);
        // 4 ボーン加重の線形合成 (シェーダの GPU スキニングと同じ式を CPU で再現)。
        QMatrix4x4 skin;
        skin.fill(0);
        auto add = [&](int id, float w) {
          if (id >= 0 && id < int(m_boneMatrices.size())) {
            for (int r = 0; r < 4; ++r) {
              for (int c = 0; c < 4; ++c) {
                skin(r, c) += w * m_boneMatrices[id](r, c);
              }
            }
          }
        };
        add(id0, w0);
        add(id1, w1);
        add(id2, w2);
        add(id3, w3);
        sp = skin.map(p);
      }
      lo = QVector3D(std::min(lo.x(), sp.x()), std::min(lo.y(), sp.y()), std::min(lo.z(), sp.z()));
      hi = QVector3D(std::max(hi.x(), sp.x()), std::max(hi.y(), sp.y()), std::max(hi.z(), sp.z()));
    }
  }

  // アニメ無しでもボーンがあれば、バインドポーズの関節位置を用意しておく。

  m_center   = (lo + hi) * 0.5f;
  m_radius   = std::max((hi - lo).length() * 0.5f, 1e-4f);
  m_bboxMin  = lo;
  m_bboxMax  = hi;
  m_hasModel = true;
  m_hasUV    = anyUV;
  m_uploaded = false;
  m_linesUploaded = false;
  m_animTimeSec = 0.0;
  m_lastMs      = 0;
  buildGrid();

  m_summaryBase = QStringLiteral("%1 メッシュ · %2 マテリアル · %3 頂点 · %4 三角形")
                      .arg(m_submeshes.size())
                      .arg(m_materials.size())
                      .arg(m_vertices.size() / ModelView::kFloatsPerVertex)
                      .arg(triCount);
  m_boneCount = int(boneId.size());
  updateSummary();

  m_filePath = path;
  buildInfoText();

  if (m_hasAnim && m_playing && isVisible()) m_animTimer->start();
  if (isVisible()) renderFrame();
  return true;
}

// 各ノードのグローバル変換を求める (locals = ノードごとのローカル変換)。
// ボーンは、静止時の配置 ([Node::restRel]: 位置・スケール) を土台に、ノードの
// 回転だけを現在姿勢に差し替える。FBX のノード階層からスケールや平行移動まで再現
// しようとすると、InheritType やセンチ/メートルの単位換算で破綻するため、バインド
// ポーズが確実に持つ値を使う。ボーン以外のノードは通常どおり親と合成する。
std::vector<QMatrix4x4> ModelView::poseGlobals(const std::vector<QMatrix4x4>& locals) const {
  const size_t            n = m_nodes.size();
  std::vector<QMatrix4x4> global(n);
  std::vector<QMatrix4x4> acc(n);  // 最も近い祖先ボーンからの累積回転
  for (size_t i = 0; i < n; ++i) {
    const Node& nd   = m_nodes[i];
    const int   p    = nd.parent;
    QMatrix4x4  base;
    if (p >= 0 && !m_nodes[p].isBone) base = acc[p];
    acc[i] = normalizedLinear(base * locals[i]);
    if (nd.isBone) {
      // 回転はノードの現在姿勢、位置とスケールはバインド時の値。
      QMatrix4x4 m = acc[i];
      for (int c = 0; c < 3; ++c) {
        const float s = columnLength(nd.restRel, c);
        for (int r = 0; r < 3; ++r) {
          m(r, c) *= s;
        }
      }
      for (int r = 0; r < 3; ++r) {
        m(r, 3) = nd.restRel(r, 3);
      }
      global[i] = nd.boneParent >= 0 ? global[nd.boneParent] * m : m;
    } else {
      global[i] = p < 0 ? locals[i] : composeGlobal(global[p], locals[i], nd.noParentScale);
    }
  }
  return global;
}

// 指定時刻のスケルトン姿勢を計算する。
//  1. 各ノードのローカル変換を求める (bindPose ならバインド姿勢そのまま、
//     アニメ時はキーフレームを補間した T*R*S)。
//  2. 親から順に掛けてノードのグローバル変換を得る (m_nodes は DFS 前順なので、
//     ループ内で親は必ず計算済み)。
//  3. スケルトン描画用にノードのワールド座標 m_jointWorld を保存。
//  4. 各ボーンの最終スキニング行列 = globalInverse * nodeGlobal * boneOffset を作る
//     (シェーダ uBones に渡す)。
void ModelView::computeBoneMatrices(double timeTicks, bool bindPose) {
  const size_t            n = m_nodes.size();
  std::vector<QMatrix4x4> global(n);
  if (bindPose && m_staticSkin) {
    // 静止 (アニメ無し) は、バインドポーズ基準の [poseGlobals]。
    std::vector<QMatrix4x4> locals(n);
    for (size_t i = 0; i < n; ++i) {
      locals[i] = m_nodes[i].bind;
    }
    global = poseGlobals(locals);
  }
  for (size_t i = 0; i < n && !(bindPose && m_staticSkin); ++i) {
    QMatrix4x4 local = m_nodes[i].bind;
    if (!bindPose && i < m_channels.size() && m_channels[i].used) {
      // 位置・スケールは線形補間、回転は球面線形補間 (slerp) する。
      const QVector3D   bindT = m_nodes[i].bind.column(3).toVector3D();
      const QVector3D   p     = interpKeys(m_channels[i].pos, timeTicks, bindT);
      const QQuaternion q =
          m_channels[i].rot.empty()
              ? QQuaternion()
              : [&] {
                  // 回転キーを挟む 2 点を探して slerp。範囲外は端をクランプ。
                  const auto& keys = m_channels[i].rot;
                  if (keys.size() == 1 || timeTicks <= keys.front().t) return keys.front().q;
                  if (timeTicks >= keys.back().t) return keys.back().q;
                  for (size_t k = 0; k + 1 < keys.size(); ++k) {
                    if (timeTicks < keys[k + 1].t) {
                      const float f =
                          float((timeTicks - keys[k].t) / (keys[k + 1].t - keys[k].t));
                      return QQuaternion::slerp(keys[k].q, keys[k + 1].q, f);
                    }
                  }
                  return keys.back().q;
                }();
      const QVector3D s = interpKeys(m_channels[i].scale, timeTicks, QVector3D(1, 1, 1));
      local.setToIdentity();
      local.translate(p);
      local.rotate(q);
      local.scale(s);
    }
    // 親 (m_nodes[i].parent) は前順走査で計算済みなので、そのまま合成できる。
    global[i] = (m_nodes[i].parent < 0)
                    ? local
                    : composeGlobal(global[m_nodes[i].parent], local, m_nodes[i].noParentScale);
  }
  // スケルトン描画用にノードのワールド座標を保存 (行列の平行移動成分 = 原点の位置)。
  m_jointWorld.resize(n);
  for (size_t i = 0; i < n; ++i) {
    m_jointWorld[i] = (m_globalInverse * global[i]).column(3).toVector3D();
  }
  // 各ボーンの最終スキニング行列。boneOffset はメッシュ空間→ボーン空間の逆バインド。
  m_boneMatrices.resize(m_boneOffset.size());
  for (size_t b = 0; b < m_boneOffset.size(); ++b) {
    const int        node = m_boneNode[b];
    const QMatrix4x4 g    = (node >= 0) ? global[node] : QMatrix4x4();
    if (m_staticSkin) {
      // 現在配置 * inverse(バインド配置)。メッシュ変換は頂点に焼き込み済み。
      const QMatrix4x4 invBind = (node >= 0 && m_nodes[node].isBone) ? m_nodes[node].bindGlobal.inverted()
                                                                      : m_boneOffset[b];
      m_boneMatrices[b] = m_globalInverse * g * invBind;
    } else {
      m_boneMatrices[b] = m_globalInverse * g * m_boneOffset[b];
    }
  }
}

void ModelView::buildGrid() {
  m_lineVerts.clear();
  const float     y    = m_bboxMin.y();
  const float     half = std::max(m_radius * 1.2f, 1e-3f);
  const int       div  = 20;
  const float     step = (half * 2.0f) / div;
  const QVector3D c(m_center.x(), y, m_center.z());
  auto            line = [&](QVector3D a, QVector3D b, QVector3D col) {
    m_lineVerts.insert(m_lineVerts.end(),
                       {a.x(), a.y(), a.z(), col.x(), col.y(), col.z(), b.x(), b.y(), b.z(),
                        col.x(), col.y(), col.z()});
  };

  // 床グリッド (座標軸はシーンに描かず、右上ギズモとして paintEvent で表示)
  const QVector3D gcol(0.32f, 0.34f, 0.38f);
  const QVector3D gcol2(0.42f, 0.44f, 0.48f);
  for (int i = -div / 2; i <= div / 2; ++i) {
    const float     o   = i * step;
    const QVector3D col = (i == 0) ? gcol2 : gcol;
    line({c.x() - half, y, c.z() + o}, {c.x() + half, y, c.z() + o}, col);
    line({c.x() + o, y, c.z() - half}, {c.x() + o, y, c.z() + half}, col);
  }
  m_lineCount = int(m_lineVerts.size() / 6);
}

void ModelView::buildInfoText() {
  m_infoLines.clear();
  m_infoLines << QFileInfo(m_filePath).fileName();
  m_infoLines << m_summary;
  if (hasTexture()) {
    for (const QString& p : resolvedTexturePaths()) m_infoLines << QStringLiteral("tex: ") + p;
    if (hasEmbeddedTexture()) m_infoLines << QStringLiteral("tex: 埋め込み");
    for (const QString& p : unresolvedTexturePaths())
      m_infoLines << QStringLiteral("tex: ") + p + QStringLiteral(" (見つからず)");
  } else {
    m_infoLines << QStringLiteral("テクスチャ: なし");
  }
  if (!m_hasUV && hasTexture()) m_infoLines << QStringLiteral("※メッシュに UV なし");

  m_infoHeader = m_infoLines;
  m_infoTree.clear();

  // FBX 内部のノード階層。assimp が関節の前に挟む補助ノード ($AssimpFbx$) は隠す。
  // m_nodes は前順 (親が先) なので、表示深さは親の深さから求められる。
  const auto isPseudo = [](const QString& name) {
    return name.contains(QLatin1String("$AssimpFbx$"));
  };
  int shown = 0, bones = 0;
  for (const Node& nd : m_nodes) {
    if (isPseudo(nd.name)) continue;
    ++shown;
    if (nd.isBone) ++bones;
  }
  m_infoLines << QString();
  m_infoLines << QStringLiteral("ノード階層 (%1 ノード · ボーン %2)").arg(shown).arg(bones);
  std::vector<int> depth(m_nodes.size(), 0);
  for (size_t i = 0; i < m_nodes.size(); ++i) {
    const int p      = m_nodes[i].parent;
    const int pDepth = p < 0 ? -1 : depth[p];
    if (isPseudo(m_nodes[i].name)) {
      depth[i] = pDepth;  // 補助ノードは深さを増やさない
      continue;
    }
    depth[i] = pDepth + 1;
    QString line = QString(depth[i] * 2, QLatin1Char(' ')) + m_nodes[i].name;
    if (m_nodes[i].isBone) line += QStringLiteral("  [bone]");
    if (!m_nodes[i].meshInfo.isEmpty()) {
      // 同じメッシュ名の重複は ×N にまとめる (登場順を保つ)。
      QStringList names, parts;
      QMap<QString, int> counts;
      for (const QString& m : m_nodes[i].meshInfo) {
        if (!counts.contains(m)) names << m;
        ++counts[m];
      }
      for (const QString& m : names) {
        parts << (counts[m] > 1 ? m + QStringLiteral(" ×%1").arg(counts[m]) : m);
      }
      line += QStringLiteral("  ▸ ") + parts.join(QStringLiteral(", "));
    }
    m_infoLines << line;
    m_infoTree.push_back({line.trimmed(), depth[i], int(i)});
  }
}

void ModelView::setHighlightNode(int node) {
  m_hiJoint  = -1;
  m_hiActive = m_hiBoneOn = false;
  m_hiSub.assign(m_submeshes.size(), 0);
  m_hiBone.assign(128, 0.0f);
  if (node >= 0 && node < int(m_nodes.size())) {
    if (m_nodes[node].isBone) {
      // ボーン: 影響するウェイトの色付けと、関節のリング。
      for (size_t id = 0; id < m_boneNode.size() && id < m_hiBone.size(); ++id) {
        if (m_boneNode[id] == node) m_hiBone[id] = 1.0f;
      }
      m_hiBoneOn = true;
      m_hiJoint  = node;
    } else {
      // メッシュ / グループ: 配下 (自分を含む) のサブメッシュ。全部が対象 (根など) なら強調しない。
      size_t hit = 0;
      for (size_t i = 0; i < m_submeshes.size(); ++i) {
        for (int p = m_submeshes[i].node; p >= 0; p = m_nodes[p].parent) {
          if (p == node) {
            m_hiSub[i] = 1;
            ++hit;
            break;
          }
        }
      }
      m_hiActive = hit > 0 && hit < m_submeshes.size();
    }
  }
  renderFrame();
}

QStringList ModelView::nodeInfoLines(int ni) const {
  QStringList out;
  if (ni < 0 || ni >= int(m_nodes.size())) return out;
  const auto isPseudo = [](const QString& n) { return n.contains(QLatin1String("$AssimpFbx$")); };
  // assimp の補助ノード ($AssimpFbx$) を飛ばした、最も近い実ノードの親。
  const auto realParent = [&](int i) {
    int p = m_nodes[i].parent;
    while (p >= 0 && isPseudo(m_nodes[p].name)) {
      p = m_nodes[p].parent;
    }
    return p;
  };
  const Node& nd = m_nodes[ni];
  out << QStringLiteral("名前: %1").arg(nd.name);
  out << QStringLiteral("種別: %1")
             .arg(nd.isBone ? QStringLiteral("ボーン")
                            : (nd.meshes.empty() ? QStringLiteral("グループ / Null") : QStringLiteral("メッシュ")));
  const int rp = realParent(ni);
  out << QStringLiteral("親: %1").arg(rp >= 0 ? m_nodes[rp].name : QStringLiteral("(なし)"));
  int children = 0;
  for (size_t j = 0; j < m_nodes.size(); ++j) {
    if (!isPseudo(m_nodes[j].name) && realParent(int(j)) == ni) ++children;
  }
  out << QStringLiteral("子: %1 個").arg(children);

  // ローカル変換: 補助ノード (Translation/PreRotation/…) を含めて親から見た変換にまとめる。
  QMatrix4x4 local = nd.bind;
  for (int p = nd.parent; p >= 0 && isPseudo(m_nodes[p].name); p = m_nodes[p].parent) {
    local = m_nodes[p].bind * local;
  }
  const QVector3D t(local(0, 3), local(1, 3), local(2, 3));
  const QVector3D s(columnLength(local, 0), columnLength(local, 1), columnLength(local, 2));
  QMatrix3x3      rm;
  for (int r = 0; r < 3; ++r) {
    for (int cc = 0; cc < 3; ++cc) {
      rm(r, cc) = s[cc] > 1e-12f ? local(r, cc) / s[cc] : (r == cc ? 1.0f : 0.0f);
    }
  }
  const QVector3D e = QQuaternion::fromRotationMatrix(rm).toEulerAngles();
  out << QStringLiteral("位置: (%1, %2, %3)").arg(t.x(), 0, 'f', 3).arg(t.y(), 0, 'f', 3).arg(t.z(), 0, 'f', 3);
  out << QStringLiteral("回転: (%1°, %2°, %3°)").arg(e.x(), 0, 'f', 1).arg(e.y(), 0, 'f', 1).arg(e.z(), 0, 'f', 1);
  out << QStringLiteral("スケール: (%1, %2, %3)").arg(s.x(), 0, 'f', 3).arg(s.y(), 0, 'f', 3).arg(s.z(), 0, 'f', 3);

  if (nd.isBone) {
    out << QStringLiteral("影響する頂点: 延べ %1 (メッシュ %2 個)").arg(nd.boneWeights).arg(nd.boneMeshes);
  }
  for (const MeshDetail& m : nd.meshes) {
    out << QString();
    out << QStringLiteral("メッシュ: %1%2").arg(m.name, m.skinned ? QStringLiteral(" (スキン)") : QString());
    out << QStringLiteral("  頂点 %1 · 三角形 %2").arg(m.vertices).arg(m.triangles);
    out << QStringLiteral("  マテリアル: %1")
               .arg(m.material.isEmpty() ? QStringLiteral("(なし)") : m.material);
    if (m.skinned) out << QStringLiteral("  影響ボーン: %1 本").arg(m.bones);
  }

  // 子孫 (自分を含む) のメッシュ合計。親ノードや、メッシュを子に持つグループで役に立つ。
  int totalV = 0, totalT = 0, totalM = 0;
  for (size_t j = 0; j < m_nodes.size(); ++j) {
    bool under = false;
    for (int p = int(j); p >= 0; p = m_nodes[p].parent) {
      if (p == ni) {
        under = true;
        break;
      }
    }
    if (!under) continue;
    for (const MeshDetail& m : m_nodes[j].meshes) {
      totalV += m.vertices;
      totalT += m.triangles;
      ++totalM;
    }
  }
  if (totalM > 0 && (children > 0)) {
    out << QString();
    out << QStringLiteral("配下のメッシュ合計: %1 個 · 頂点 %2 · 三角形 %3").arg(totalM).arg(totalV).arg(totalT);
  }
  return out;
}

void ModelView::setTextureEnabled(bool on) {
  if (m_texEnabled == on) return;
  m_texEnabled = on;
  emit textureEnabledChanged(on);
  renderFrame();
}
void ModelView::setShowGrid(bool on) {
  if (m_showGrid == on) return;
  m_showGrid = on;
  emit showGridChanged(on);
  renderFrame();
}
void ModelView::setShowHelp(bool on) {
  if (m_showHelp == on) return;
  m_showHelp = on;
  emit showHelpChanged(on);
  update();  // オーバーレイのみ再描画 (再レンダリング不要)
}
void ModelView::setShowBones(bool on) {
  if (m_showBones == on) return;
  m_showBones = on;
  emit showBonesChanged(on);
  update();  // オーバーレイのみ再描画
}
void ModelView::setWireframe(bool on) {
  if (m_wireframe == on) return;
  m_wireframe = on;
  emit wireframeChanged(on);
  renderFrame();
}
void ModelView::setCullBackface(bool on) {
  if (m_cullBackface == on) return;
  m_cullBackface = on;
  emit cullBackfaceChanged(on);
  renderFrame();
}
void ModelView::resetView() {
  m_yaw   = 0.7f;
  m_pitch = 0.35f;
  m_dist  = 2.6f;
  m_pan   = QVector3D(0, 0, 0);
  renderFrame();
}
void ModelView::setAnimationPlaying(bool on) {
  if (m_playing != on) {
    m_playing = on;
    emit animationPlayingChanged(on);
  }
  if (on) {
    m_lastMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    if (isVisible()) m_animTimer->start();
  } else {
    m_animTimer->stop();
  }
  renderFrame();
}
// クリップ idx を現在のアニメとして設定する (内部用。再描画はしない)。
void ModelView::selectClip(int idx) {
  m_channels.assign(m_nodes.size(), Channel{});
  m_hasAnim           = false;
  m_animDurationTicks = 0.0;
  m_ticksPerSec       = 25.0;
  if (idx < 0 || idx >= int(m_clips.size())) return;
  m_clip              = idx;
  m_channels          = m_clips[idx].channels;
  m_animDurationTicks = m_clips[idx].durationTicks;
  m_ticksPerSec       = m_clips[idx].ticksPerSec;
  m_hasAnim           = !m_boneOffset.empty() && m_animDurationTicks > 0.0;
}
QStringList ModelView::animationClipNames() const {
  QStringList out;
  for (size_t i = 0; i < m_clips.size(); ++i) {
    out << (m_clips[i].name.isEmpty() ? QStringLiteral("clip %1").arg(i + 1) : m_clips[i].name);
  }
  return out;
}
void ModelView::updateSummary() {
  m_summary = m_summaryBase;
  if (m_hasAnim)
    m_summary += QStringLiteral(" · アニメ %1s / ボーン %2").arg(animationDuration(), 0, 'f', 1).arg(m_boneCount);
}
void ModelView::setViewAngles(float yaw, float pitch, float dist) {
  m_yaw   = yaw;
  m_pitch = std::clamp(pitch, -1.53f, 1.53f);
  m_dist  = std::clamp(dist, 0.2f, 40.0f);
  renderFrame();
}
void ModelView::setAnimationClip(int idx) {
  if (idx < 0 || idx >= int(m_clips.size()) || idx == m_clip) return;
  selectClip(idx);
  updateSummary();
  buildInfoText();
  emit animationClipChanged(idx);
  m_animTimeSec = 0.0;
  m_lastMs      = m_clock.isValid() ? m_clock.elapsed() : 0;
  renderFrame();
}
void ModelView::setAnimationTime(double sec) {
  m_playing = false;
  m_animTimer->stop();
  m_animTimeSec = sec;
  renderFrame();
}

// 描画用の隠し OpenGL コンテキストとオフスクリーンサーフェスを (初回のみ) 用意する。
// OpenGL 3.3 Core / 深度バッファ付き。作成失敗時は false を返し描画をスキップする。
bool ModelView::ensureContext() {
  if (m_ctx && m_ctx->isValid()) return m_surface && m_surface->isValid();
  QSurfaceFormat fmt;
  fmt.setVersion(3, 3);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  if (!m_ctx) {
    m_ctx = new QOpenGLContext;
    m_ctx->setFormat(fmt);
    if (!m_ctx->create()) {
      delete m_ctx;
      m_ctx = nullptr;
      return false;
    }
  }
  if (!m_surface) {
    m_surface = new QOffscreenSurface;
    m_surface->setFormat(m_ctx->format());
    m_surface->create();
  }
  return m_surface->isValid();
}

void ModelView::ensureGLResources() {
  if (m_glResourcesReady) return;
  initializeOpenGLFunctions();
  if (!m_prog.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader) ||
      !m_prog.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) ||
      !m_prog.link())
    qWarning("ModelView: shader error: %s", qPrintable(m_prog.log()));
  if (!m_lineProg.addShaderFromSourceCode(QOpenGLShader::Vertex, kLineVertexShader) ||
      !m_lineProg.addShaderFromSourceCode(QOpenGLShader::Fragment, kLineFragmentShader) ||
      !m_lineProg.link())
    qWarning("ModelView: line shader error: %s", qPrintable(m_lineProg.log()));
  m_vao.create();
  m_lineVao.create();
  m_glResourcesReady = true;
}

// 頂点/インデックス/テクスチャ/グリッド線を GPU に (変更時のみ) アップロードする。
// makeCurrent 済みで呼ぶこと。属性レイアウトは kFloatsPerVertex に対応 (0:位置3 /
// 1:法線3 / 2:UV2 / 3:boneID4 / 4:weight4)。
void ModelView::uploadIfNeeded() {
  if (!m_uploaded && !m_vertices.empty()) {
    m_vao.bind();
    if (!m_vbo.isCreated()) m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(m_vertices.data(), int(m_vertices.size() * sizeof(float)));
    if (!m_ibo.isCreated()) m_ibo.create();
    m_ibo.bind();
    m_ibo.allocate(m_indices.data(), int(m_indices.size() * sizeof(unsigned int)));
    const int stride = ModelView::kFloatsPerVertex * sizeof(float);
    auto      attr   = [&](int loc, int size, int offsetFloats) {
      glEnableVertexAttribArray(loc);
      glVertexAttribPointer(loc, size, GL_FLOAT, GL_FALSE, stride,
                            reinterpret_cast<void*>(offsetFloats * sizeof(float)));
    };
    attr(0, 3, 0);
    attr(1, 3, 3);
    attr(2, 2, 6);
    attr(3, 4, 8);
    attr(4, 4, 12);
    m_vao.release();

    for (auto& mat : m_materials) {
      if (mat.resolved && !mat.image.isNull() && !mat.tex) {
        mat.tex = std::make_unique<QOpenGLTexture>(mat.image, QOpenGLTexture::GenerateMipMaps);
        mat.tex->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
        mat.tex->setMagnificationFilter(QOpenGLTexture::Linear);
        mat.tex->setWrapMode(QOpenGLTexture::Repeat);
      }
    }
    m_uploaded = true;
  }

  if (!m_linesUploaded && !m_lineVerts.empty()) {
    m_lineVao.bind();
    if (!m_lineVbo.isCreated()) m_lineVbo.create();
    m_lineVbo.bind();
    m_lineVbo.allocate(m_lineVerts.data(), int(m_lineVerts.size() * sizeof(float)));
    const int lstride = 6 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, lstride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, lstride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    m_lineVao.release();
    m_linesUploaded = true;
  }
}

// 1 フレームをオフスクリーン (隠し context + FBO) に描画し、結果を QImage
// (m_frame) に取り出して update() で paintEvent に表示させる。ネイティブ GL 面を
// ウィジェットに持たせないため、macOS で QStackedWidget 等に埋め込んでも黒化しない
// (詳細は ModelView.h の冒頭コメント)。
void ModelView::renderFrame() {
  if (!m_hasModel || width() <= 0 || height() <= 0) return;
  if (!ensureContext()) return;
  if (!m_ctx->makeCurrent(m_surface)) return;
  ensureGLResources();

  const int w = std::max(1, int(width() * devicePixelRatioF()));
  const int h = std::max(1, int(height() * devicePixelRatioF()));
  if (!m_fbo || m_fbo->width() != w || m_fbo->height() != h) {
    delete m_fbo;
    QOpenGLFramebufferObjectFormat f;
    f.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    f.setSamples(4);
    m_fbo = new QOpenGLFramebufferObject(w, h, f);
  }
  m_fbo->bind();
  glViewport(0, 0, w, h);
  glEnable(GL_DEPTH_TEST);
  // 裏面カリング。Unity など一般的なエンジンは既定で片面描画なので、それに合わせて
  // 法線の裏側 (CW) を描かない。ワイヤーフレーム時は構造把握のため両面描く。
  if (m_cullBackface && !m_wireframe) {
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
  } else {
    glDisable(GL_CULL_FACE);
  }
  glClearColor(0.12f, 0.13f, 0.15f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  uploadIfNeeded();

  if (m_hasAnim) {
    if (m_playing) {
      if (!m_clock.isValid()) m_clock.start();
      const qint64 now = m_clock.elapsed();
      m_animTimeSec += double(now - m_lastMs) / 1000.0;
      m_lastMs = now;
    }
    double timeTicks = 0.0;
    if (m_animDurationTicks > 0.0)
      timeTicks = std::fmod(m_animTimeSec * m_ticksPerSec, m_animDurationTicks);
    computeBoneMatrices(timeTicks, /*bindPose=*/false);
  }

  const float     r      = m_radius;
  const float     dist   = m_dist * r;
  const QVector3D dir(std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                      std::cos(m_pitch) * std::cos(m_yaw));
  const QVector3D target = m_center + m_pan;
  const QVector3D eye    = target + dir * dist;

  QMatrix4x4 view;
  view.lookAt(eye, target, QVector3D(0, 1, 0));
  QMatrix4x4  proj;
  const float aspect = height() > 0 ? float(width()) / float(height()) : 1.0f;
  proj.perspective(45.0f, aspect, r * 0.01f, dist + r * 10.0f);

  const QMatrix4x4 mvp     = proj * view;
  m_lastMvp                = mvp;  // スケルトンの 2D 投影に使う
  const QMatrix3x3 normal  = view.normalMatrix();
  const bool       skinned = !m_boneMatrices.empty();

  m_prog.bind();
  m_prog.setUniformValue("uMVP", mvp);
  m_prog.setUniformValue("uNormalView", normal);
  m_prog.setUniformValue("uSkinned", skinned);
  if (skinned)
    m_prog.setUniformValueArray("uBones", m_boneMatrices.data(),
                                std::min<int>(int(m_boneMatrices.size()), 128));
  m_prog.setUniformValue("uTex", 0);
  m_prog.setUniformValue("uUnlit", m_wireframe);  // ワイヤーフレームは陰影なし
  m_prog.setUniformValue("uHiBoneOn", m_hiBoneOn && skinned);
  if (m_hiBoneOn && skinned) {
    m_prog.setUniformValueArray("uHiBone", m_hiBone.data(), std::min<int>(int(m_hiBone.size()), 128), 1);
  }

  if (m_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  m_vao.bind();
  for (size_t smi = 0; smi < m_submeshes.size(); ++smi) {
    const SubMesh&  sm     = m_submeshes[smi];
    const bool      inSel  = m_hiActive && smi < m_hiSub.size() && m_hiSub[smi];
    m_prog.setUniformValue("uTint", inSel ? 1.0f : 0.0f);
    m_prog.setUniformValue("uDim", (m_hiActive && !inSel) ? 0.45f : 1.0f);
    const Material& mat    = m_materials[std::clamp(sm.material, 0, int(m_materials.size()) - 1)];
    const bool      useTex = m_texEnabled && mat.tex && !m_wireframe;
    m_prog.setUniformValue("uUseTexture", useTex);
    // ワイヤーフレーム時はマテリアル色に依らず見やすい一定色で線を描く。
    m_prog.setUniformValue("uBaseColor",
                           m_wireframe ? QVector3D(0.55f, 0.85f, 0.65f) : mat.baseColor);
    if (useTex) mat.tex->bind(0);
    glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT,
                   reinterpret_cast<void*>(intptr_t(sm.indexOffset) * sizeof(unsigned int)));
    if (useTex) mat.tex->release(0);
  }
  m_vao.release();
  m_prog.release();
  if (m_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

  if (m_showGrid && m_linesUploaded && m_lineCount > 0) {
    m_lineProg.bind();
    m_lineProg.setUniformValue("uMVP", mvp);
    m_lineVao.bind();
    glDrawArrays(GL_LINES, 0, m_lineCount);
    m_lineVao.release();
    m_lineProg.release();
  }

  m_fbo->release();
  m_frame = m_fbo->toImage();
  m_frame.setDevicePixelRatio(devicePixelRatioF());
  m_ctx->doneCurrent();
  update();
}

QImage ModelView::renderToImage() {
  renderFrame();
  return m_frame;
}

void ModelView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(30, 33, 38));
  if (!m_frame.isNull())
    p.drawImage(QRectF(0, 0, width(), height()), m_frame,
                QRectF(0, 0, m_frame.width(), m_frame.height()));

  if (!m_hasModel) return;
  p.setRenderHint(QPainter::Antialiasing, true);

  // 右上の座標軸ギズモ (カメラ向きに追従。モデルには重ならない小さな指標)
  {
    const QVector3D dir(std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                        std::cos(m_pitch) * std::cos(m_yaw));
    const QVector3D fwd   = -dir;
    const QVector3D right = QVector3D::crossProduct(fwd, QVector3D(0, 1, 0)).normalized();
    const QVector3D up    = QVector3D::crossProduct(right, fwd).normalized();
    const qreal     len   = 22.0;
    const QPointF   center(width() - 38.0, 38.0);
    p.setBrush(QColor(20, 22, 26, 150));
    p.setPen(Qt::NoPen);
    p.drawEllipse(center, len + 10, len + 10);
    struct Ax { QVector3D v; QColor col; const char* name; };
    Ax axes[3] = {{{1, 0, 0}, QColor(226, 92, 92), "X"},
                  {{0, 1, 0}, QColor(102, 209, 115), "Y"},
                  {{0, 0, 1}, QColor(97, 148, 245), "Z"}};
    std::sort(axes, axes + 3, [&](const Ax& a, const Ax& b) {
      return QVector3D::dotProduct(a.v, fwd) < QVector3D::dotProduct(b.v, fwd);
    });
    QFont gf = p.font();
    gf.setBold(true);
    gf.setPointSize(10);
    p.setFont(gf);
    for (const Ax& a : axes) {
      const QPointF tip = center + QPointF(QVector3D::dotProduct(a.v, right) * len,
                                           -QVector3D::dotProduct(a.v, up) * len);
      p.setPen(QPen(a.col, 2.0));
      p.drawLine(center, tip);
      p.drawText(tip + QPointF(-3, 4), QString::fromLatin1(a.name));
    }
  }

  // スケルトン (ボーン) を関節線 + 関節点で重ねる (B キー / ツールバーで ON/OFF)。
  if (m_showBones && !m_boneOffset.empty() && !m_jointWorld.empty()) {
    auto project = [&](const QVector3D& wp, QPointF& out) -> bool {
      const QVector4D clip = m_lastMvp * QVector4D(wp, 1.0f);
      if (clip.w() <= 1e-6f) return false;
      const float x = clip.x() / clip.w(), y = clip.y() / clip.w();
      out = QPointF((x * 0.5f + 0.5f) * width(), (1.0f - (y * 0.5f + 0.5f)) * height());
      return true;
    };
    p.setPen(QPen(QColor(255, 170, 60, 220), 1.6));
    for (size_t i = 0; i < m_jointWorld.size(); ++i) {
      if (!m_isBoneNode[i]) continue;
      const int a = m_boneAncestor[i];
      if (a < 0) continue;
      QPointF p0, p1;
      if (project(m_jointWorld[a], p0) && project(m_jointWorld[i], p1)) p.drawLine(p0, p1);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 220, 120, 235));
    for (size_t i = 0; i < m_jointWorld.size(); ++i) {
      if (!m_isBoneNode[i]) continue;
      QPointF c;
      if (project(m_jointWorld[i], c)) p.drawEllipse(c, 2.8, 2.8);
    }
  }

  // 選択中のボーンの関節に、リングと名前を出す (スケルトン表示の ON/OFF に関わらず)。
  if (m_hiJoint >= 0 && m_hiJoint < int(m_jointWorld.size())) {
    QPointF      c;
    const QVector4D clip = m_lastMvp * QVector4D(m_jointWorld[m_hiJoint], 1.0f);
    if (clip.w() > 1e-6f) {
      c = QPointF((clip.x() / clip.w() * 0.5f + 0.5f) * width(),
                  (1.0f - (clip.y() / clip.w() * 0.5f + 0.5f)) * height());
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(QColor(255, 140, 26), 2.0));
      p.drawEllipse(c, 8.0, 8.0);
      p.setPen(QColor(255, 190, 110));
      p.drawText(QPointF(c.x() + 12, c.y() - 10), m_nodes[m_hiJoint].name);
    }
  }

  // 左上のオーバーレイ: 上に「モデル全体の情報」、その下に「操作方法」(H キー / ツールバーで
  // ON/OFF)。どちらも同じ半透明の黒い背景に載せる。
  QFont hf = p.font();
  hf.setPointSizeF(10.0);
  p.setFont(hf);
  const QFontMetrics fm(hf);
  const int          lh = fm.height() + 3;
  const int          padX = 10, padY = 8;
  const auto         drawPanel = [&](const QStringList& rows, int top, bool elide) -> int {
    int maxW = 0;
    for (const QString& r : rows) maxW = std::max(maxW, fm.horizontalAdvance(r));
    maxW += 4;                              // 測定誤差で最長の行まで省略されないように余裕を持たせる
    if (elide) maxW = std::min(maxW, 380);  // 長い行 (テクスチャのパスなど) は幅に収める
    const QRectF box(10, top, maxW + padX * 2, int(rows.size()) * lh + padY * 2 - 3);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 22, 26, 150));
    p.drawRoundedRect(box, 6, 6);
    p.setPen(QColor(210, 214, 220));
    int y = int(box.top()) + padY + fm.ascent();
    for (const QString& r : rows) {
      p.drawText(int(box.left()) + padX, y, elide ? fm.elidedText(r, Qt::ElideMiddle, maxW) : r);
      y += lh;
    }
    return int(box.bottom());
  };

  int nextTop = 10;
  if (m_hasModel) {
    QStringList info;
    for (const QString& l : m_infoHeader) {
      // 要約 (メッシュ数 · マテリアル数 · 頂点数 · 三角形数 …) は 2 項目ずつに折って縦長にする。
      if (l == m_summary) {
        const QStringList parts = l.split(QStringLiteral(" · "));
        for (int k = 0; k < parts.size(); k += 2) {
          info << (k + 1 < parts.size() ? parts[k] + QStringLiteral(" · ") + parts[k + 1] : parts[k]);
        }
      } else if (l.startsWith(QStringLiteral("tex: "))) {
        // テクスチャはファイル名だけ (フルパスだとパネルが横に広がって、モデルに重なる)。
        QString       rest   = l.mid(5);
        const QString miss   = QStringLiteral(" (見つからず)");
        QString       suffix;
        if (rest.endsWith(miss)) {
          suffix = miss;
          rest.chop(miss.size());
        }
        info << QStringLiteral("tex: ") + QFileInfo(rest).fileName() + suffix;
      } else {
        info << l;
      }
    }
    nextTop = drawPanel(info, nextTop, /*elide=*/true) + 8;
  }
  if (m_showHelp) {
    QStringList rows;
    rows << QStringLiteral("矢印 : 回転")
         << QStringLiteral("WASD : 平行移動")
         << QStringLiteral("U / J : 拡大 / 縮小")
         << QStringLiteral("R : 視点リセット")
         << QStringLiteral("T : テクスチャ")
         << QStringLiteral("G : グリッド")
         << QStringLiteral("F : ワイヤーフレーム")
         << QStringLiteral("C : 裏面カリング");
    if (!m_boneOffset.empty()) rows << QStringLiteral("B : ボーン");
    if (m_hasAnim) rows << QStringLiteral("Space : 再生 / 停止");
    if (m_clips.size() > 1) rows << QStringLiteral("[ / ] : クリップ切替");
    rows << QStringLiteral("H : 操作の表示")
         << QStringLiteral("i : モデル情報");
    drawPanel(rows, nextTop, /*elide=*/false);
  }
}

void ModelView::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  renderFrame();
}

void ModelView::showEvent(QShowEvent* e) {
  QWidget::showEvent(e);
  if (m_hasAnim && m_playing) m_animTimer->start();
  renderFrame();
}

void ModelView::mousePressEvent(QMouseEvent* e) { m_lastPos = e->pos(); }

void ModelView::mouseMoveEvent(QMouseEvent* e) {
  if (!(e->buttons() & Qt::LeftButton)) return;
  const QPoint d = e->pos() - m_lastPos;
  m_lastPos      = e->pos();
  m_yaw -= d.x() * 0.01f;
  m_pitch += d.y() * 0.01f;
  m_pitch = std::clamp(m_pitch, -1.53f, 1.53f);
  renderFrame();
}

void ModelView::wheelEvent(QWheelEvent* e) {
  const float f = e->angleDelta().y() > 0 ? 0.9f : 1.1f;
  m_dist        = std::clamp(m_dist * f, 0.2f, 40.0f);
  renderFrame();
}

void ModelView::applyShortcutBindings(const QVariantMap& bindings) {
  m_shortcuts.applyBindings(bindings);
}

bool ModelView::dispatchShortcut(const QString& cmd) {
  const float rot = 0.10f;                          // 回転ステップ (rad)
  const float pan = std::max(m_radius, 1e-3f) * 0.06f;  // 平行移動ステップ (world)
  // カメラの右 / 上ベクトル (平行移動用)
  const QVector3D dir(std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                      std::cos(m_pitch) * std::cos(m_yaw));
  const QVector3D fwd   = -dir;
  const QVector3D right = QVector3D::crossProduct(fwd, QVector3D(0, 1, 0)).normalized();
  const QVector3D up    = QVector3D::crossProduct(right, fwd).normalized();

  // 回転 (既定: 矢印)
  if (cmd == QLatin1String("viewer.model.rotate_left"))  { m_yaw -= rot; renderFrame(); return true; }
  if (cmd == QLatin1String("viewer.model.rotate_right")) { m_yaw += rot; renderFrame(); return true; }
  if (cmd == QLatin1String("viewer.model.rotate_up")) {
    m_pitch = std::clamp(m_pitch + rot, -1.53f, 1.53f); renderFrame(); return true;
  }
  if (cmd == QLatin1String("viewer.model.rotate_down")) {
    m_pitch = std::clamp(m_pitch - rot, -1.53f, 1.53f); renderFrame(); return true;
  }
  // 平行移動 (既定: WASD)
  if (cmd == QLatin1String("viewer.model.pan_up"))    { m_pan += up * pan;    renderFrame(); return true; }
  if (cmd == QLatin1String("viewer.model.pan_down"))  { m_pan -= up * pan;    renderFrame(); return true; }
  if (cmd == QLatin1String("viewer.model.pan_left"))  { m_pan -= right * pan; renderFrame(); return true; }
  if (cmd == QLatin1String("viewer.model.pan_right")) { m_pan += right * pan; renderFrame(); return true; }
  // 拡大縮小 (既定: U/J)
  if (cmd == QLatin1String("viewer.model.zoom_in")) {
    m_dist = std::clamp(m_dist * 0.9f, 0.2f, 40.0f); renderFrame(); return true;
  }
  if (cmd == QLatin1String("viewer.model.zoom_out")) {
    m_dist = std::clamp(m_dist * 1.1f, 0.2f, 40.0f); renderFrame(); return true;
  }
  // リセット / 情報 / 各種トグル
  if (cmd == QLatin1String("viewer.model.reset")) { resetView(); return true; }
  if (cmd == QLatin1String("viewer.model.info"))  { emit infoRequested(); return true; }
  if (cmd == QLatin1String("viewer.model.toggle_texture"))   { setTextureEnabled(!m_texEnabled); return true; }
  if (cmd == QLatin1String("viewer.model.toggle_grid"))      { setShowGrid(!m_showGrid); return true; }
  if (cmd == QLatin1String("viewer.model.toggle_wireframe")) { setWireframe(!m_wireframe); return true; }
  if (cmd == QLatin1String("viewer.model.toggle_backface"))  { setCullBackface(!m_cullBackface); return true; }
  if (cmd == QLatin1String("viewer.model.toggle_bones")) {
    if (!m_boneOffset.empty()) {
      setShowBones(!m_showBones);
    }
    return true;
  }
  if (cmd == QLatin1String("viewer.model.toggle_help")) { setShowHelp(!m_showHelp); return true; }
  if (cmd == QLatin1String("viewer.model.toggle_animation")) {
    if (m_hasAnim) {
      setAnimationPlaying(!m_playing);
      return true;
    }
    return false;  // アニメーションが無ければ未処理 (基底へ委譲)
  }
  return false;
}

void ModelView::keyPressEvent(QKeyEvent* e) {
  // ショートカットは本体キーバインド設定から push された割り当て (m_shortcuts) で
  // 引く。未割り当て / 未対応なら基底へ委譲する。
  const QString cmd = m_shortcuts.commandForSeq(ViewerShortcutMap::sequenceForEvent(e));
  if (!cmd.isEmpty() && dispatchShortcut(cmd)) {
    return;
  }
  // アニメのクリップ切替は本体のキーバインドに無いので、プラグイン固定のキー ([ / ]) で受ける。
  if (m_clips.size() > 1 && e->modifiers() == Qt::NoModifier &&
      (e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BracketRight)) {
    const int n    = int(m_clips.size());
    const int next = (m_clip + (e->key() == Qt::Key_BracketRight ? 1 : n - 1)) % n;
    setAnimationClip(next);
    return;
  }
  QWidget::keyPressEvent(e);
}

} // namespace Farman

#include "ModelViewerWidget.h"

#include "ModelView.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFontDatabase>
#include <QDialogButtonBox>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QSizePolicy>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace Farman {

namespace {

// ノード階層ツリー。標準の開閉記号に加えて、親子をつなぐ接続線 (├ └ │) を描く。
class NodeTree : public QTreeWidget {
 public:
  using QTreeWidget::QTreeWidget;

 protected:
  void drawBranches(QPainter* p, const QRect& rect, const QModelIndex& index) const override {
    // 行の分岐領域は (深さ + 1) 個のセルで、右端のセルが自分の開閉記号の位置。
    QVector<QModelIndex> chain;  // 根 → 自分
    for (QModelIndex a = index; a.isValid(); a = a.parent()) {
      chain.prepend(a);
    }
    const int cell  = indentation();
    const int depth = chain.size() - 1;
    if (cell > 0 && depth >= 1) {
      QColor col = palette().color(QPalette::Text);
      col.setAlpha(90);
      p->save();
      p->setPen(QPen(col, 1));
      const int left = rect.right() + 1 - cell * (depth + 1);
      const int top = rect.top(), bottom = rect.bottom(), mid = (rect.top() + rect.bottom()) / 2;
      for (int k = 1; k < depth; ++k) {  // 祖先の続き線 (後ろにきょうだいがあるときだけ)
        if (chain[k].sibling(chain[k].row() + 1, 0).isValid()) {
          const int x = left + cell * k + cell / 2;
          p->drawLine(x, top, x, bottom);
        }
      }
      const int  x        = left + cell * depth + cell / 2;
      const bool hasNext  = index.sibling(index.row() + 1, 0).isValid();
      const bool hasChild = model()->hasChildren(index);
      p->drawLine(x - cell, top, x - cell, hasNext ? bottom : mid);  // 親の幹 → この行
      p->drawLine(x - cell, mid, hasChild ? x - 6 : x + cell / 2 - 1, mid);  // 枝
      p->restore();
    }
    QTreeWidget::drawBranches(p, rect, index);
  }
};

enum class Glyph { Texture, Grid, Wire, Cull, Bones, Play, Pause, Reset, Help, Info };

void drawGlyph(QPainter& p, Glyph g, const QColor& c) {
  p.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(c, 1.6);
  pen.setJoinStyle(Qt::RoundJoin);
  pen.setCapStyle(Qt::RoundCap);
  switch (g) {
    case Glyph::Texture: {  // 角丸 + チェッカー
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(QRectF(3, 3, 12, 12), 2, 2);
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      p.drawRect(QRectF(3.5, 3.5, 5.5, 5.5));
      p.drawRect(QRectF(9, 9, 5.5, 5.5));
      break;
    }
    case Glyph::Grid: {
      p.setPen(pen);
      p.drawRect(QRectF(3, 3, 12, 12));
      p.drawLine(QPointF(7, 3), QPointF(7, 15));
      p.drawLine(QPointF(11, 3), QPointF(11, 15));
      p.drawLine(QPointF(3, 7), QPointF(15, 7));
      p.drawLine(QPointF(3, 11), QPointF(15, 11));
      break;
    }
    case Glyph::Wire: {  // 三角形をワイヤーフレーム (稜線 + 内部の分割線) で
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      const QPointF a(9, 3), b(3, 15), c(15, 15);
      p.drawLine(a, b);
      p.drawLine(b, c);
      p.drawLine(c, a);
      p.drawLine(a, QPointF(9, 15));  // 内部分割線でメッシュ感を出す
      break;
    }
    case Glyph::Cull: {  // 表面 (塗り) と裏面 (破線) の 2 枚の板 = 裏面カリング
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      QPen dashed = pen;
      dashed.setStyle(Qt::DashLine);
      dashed.setWidthF(1.2);
      p.setPen(dashed);
      p.drawRect(QRectF(6.5, 3.5, 8, 8));  // 奥 (裏面) は破線
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      p.drawRect(QRectF(3, 7, 8, 8));      // 手前 (表面) は塗り
      break;
    }
    case Glyph::Bones: {  // 関節線 + 関節点 (スケルトン)
      p.setPen(pen);
      const QPointF j0(5, 14), j1(9, 8), j2(13, 4);
      p.drawLine(j0, j1);
      p.drawLine(j1, j2);
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      p.drawEllipse(j0, 1.7, 1.7);
      p.drawEllipse(j1, 1.7, 1.7);
      p.drawEllipse(j2, 1.7, 1.7);
      break;
    }
    case Glyph::Play: {
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      QPolygonF tri;
      tri << QPointF(6, 4) << QPointF(6, 14) << QPointF(14, 9);
      p.drawPolygon(tri);
      break;
    }
    case Glyph::Pause: {
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      p.drawRoundedRect(QRectF(5, 4, 3, 10), 1, 1);
      p.drawRoundedRect(QRectF(10, 4, 3, 10), 1, 1);
      break;
    }
    case Glyph::Reset: {  // 円弧 + 矢じり (リロード風)
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      const QRectF r(4, 4, 10, 10);
      p.drawArc(r, 60 * 16, 280 * 16);
      // 矢じり (弧の始点付近)
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      QPolygonF ah;
      ah << QPointF(11.5, 3.2) << QPointF(13.6, 6.2) << QPointF(10.0, 6.0);
      p.drawPolygon(ah);
      break;
    }
    case Glyph::Help: {  // 角丸 + 「?」
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(QRectF(3, 3, 12, 12), 3, 3);
      QFont f = p.font();
      f.setBold(true);
      f.setPixelSize(11);
      p.setFont(f);
      p.setPen(QPen(c));
      p.drawText(QRectF(3, 3, 12, 12), Qt::AlignCenter, QStringLiteral("?"));
      break;
    }
    case Glyph::Info: {
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      p.drawEllipse(QRectF(3, 3, 12, 12));
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      p.drawEllipse(QPointF(9, 6.3), 1.1, 1.1);
      p.drawRoundedRect(QRectF(8.1, 8.2, 1.8, 5.0), 0.8, 0.8);
      break;
    }
  }
}

QIcon makeIcon(Glyph g, const QColor& c) {
  const qreal dpr = qApp ? qApp->devicePixelRatio() : 2.0;
  QPixmap     pm(QSize(18, 18) * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  drawGlyph(p, g, c);
  p.end();
  return QIcon(pm);
}

// farman 本体のビュアーツールバーと同じ見た目 (ホバー / 押下[checked] / フォーカス枠)
// にするための QSS。本体 src/utils/EnterClickFilter.h の toolbarStyleSheet() と
// 同じ計算・同じセレクタを、プラグインから参照できないため複製している。これを
// 当てないと Windows 既定スタイルの青い checked 背景になり、他ビュアーと見た目が
// ずれる。現在のパレットから色を計算して埋め込む。
QString toolbarStyleSheet() {
  const QPalette pal = qApp ? qApp->palette() : QPalette();
  const QColor   win = pal.color(QPalette::Active, QPalette::Window);
  const bool     dark = win.lightness() < 128;
  const QColor   hover        = dark ? win.lighter(140) : win.darker(106);
  const QColor   checked      = dark ? win.lighter(175) : win.darker(112);
  const QColor   checkedHover = dark ? win.lighter(195) : win.darker(117);
  const QColor   focusBorder  = pal.color(QPalette::Active, QPalette::Highlight);
  QString s;
  s += QStringLiteral("QToolBar { background-color: %1; border: 0px; }").arg(win.name());
  s += QStringLiteral(
           "QToolButton { padding: 3px; border: 1px solid transparent; border-radius: 3px; }"
           "QToolButton:hover { background-color: %1; }"
           "QToolButton:checked { background-color: %2; }"
           "QToolButton:checked:hover { background-color: %3; }"
           "QToolButton:focus { border: 2px solid %4; padding: 2px; }"
           "QToolButton:checked:focus { background-color: %2; border: 2px solid %4; padding: 2px; }")
           .arg(hover.name(), checked.name(), checkedHover.name(), focusBorder.name());
  return s;
}

} // namespace

ModelViewerWidget::ModelViewerWidget(QWidget* parent) : QWidget(parent) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  m_toolbar = new QToolBar(this);
  m_toolbar->setMovable(false);
  m_toolbar->setFloatable(false);
  m_toolbar->setIconSize(QSize(20, 20));
  // 本体ビュアーと同じツールバー見た目 (ホバー/押下/フォーカス枠) に揃える。
  m_toolbar->setStyleSheet(toolbarStyleSheet());
  m_view = new ModelView(this);

  // ツールバーの下は、左に 3D ビュー・右に情報パネル (初期は非表示) の左右分割。
  m_split = new QSplitter(Qt::Horizontal, this);
  m_split->setChildrenCollapsible(false);
  m_split->addWidget(m_view);
  m_infoPanel = buildInfoPanel();
  m_infoPanel->hide();
  m_split->addWidget(m_infoPanel);
  m_split->setStretchFactor(0, 1);
  m_split->setStretchFactor(1, 0);

  lay->addWidget(m_toolbar);
  lay->addWidget(m_split, 1);
  setFocusProxy(m_view);

  const QColor ic = palette().color(QPalette::ButtonText);

  m_actTexture = m_toolbar->addAction(makeIcon(Glyph::Texture, ic), QString());
  m_actTexture->setCheckable(true);
  m_actTexture->setChecked(true);
  m_actTexture->setToolTip(QStringLiteral("テクスチャ表示 (T)"));
  connect(m_actTexture, &QAction::toggled, m_view, &ModelView::setTextureEnabled);
  connect(m_view, &ModelView::textureEnabledChanged, this, [this](bool on) {
    QSignalBlocker b(m_actTexture);
    m_actTexture->setChecked(on);
  });

  m_actGrid = m_toolbar->addAction(makeIcon(Glyph::Grid, ic), QString());
  m_actGrid->setCheckable(true);
  m_actGrid->setChecked(true);
  m_actGrid->setToolTip(QStringLiteral("床グリッド (G)"));
  connect(m_actGrid, &QAction::toggled, m_view, &ModelView::setShowGrid);
  connect(m_view, &ModelView::showGridChanged, this, [this](bool on) {
    QSignalBlocker b(m_actGrid);
    m_actGrid->setChecked(on);
  });

  m_actWire = m_toolbar->addAction(makeIcon(Glyph::Wire, ic), QString());
  m_actWire->setCheckable(true);
  m_actWire->setChecked(false);
  m_actWire->setToolTip(QStringLiteral("ワイヤーフレーム表示 (陰影なし) (F)"));
  connect(m_actWire, &QAction::toggled, m_view, &ModelView::setWireframe);
  connect(m_view, &ModelView::wireframeChanged, this, [this](bool on) {
    QSignalBlocker b(m_actWire);
    m_actWire->setChecked(on);
  });

  m_actCull = m_toolbar->addAction(makeIcon(Glyph::Cull, ic), QString());
  m_actCull->setCheckable(true);
  m_actCull->setChecked(true);
  m_actCull->setToolTip(QStringLiteral("裏面カリング (裏面を描かない。Unity 等と同じ片面表示) (C)"));
  connect(m_actCull, &QAction::toggled, m_view, &ModelView::setCullBackface);
  connect(m_view, &ModelView::cullBackfaceChanged, this, [this](bool on) {
    QSignalBlocker b(m_actCull);
    m_actCull->setChecked(on);
  });

  m_actBones = m_toolbar->addAction(makeIcon(Glyph::Bones, ic), QString());
  m_actBones->setCheckable(true);
  m_actBones->setChecked(false);
  m_actBones->setToolTip(QStringLiteral("ボーン (スケルトン) 表示 (B)"));
  connect(m_actBones, &QAction::toggled, m_view, &ModelView::setShowBones);
  connect(m_view, &ModelView::showBonesChanged, this, [this](bool on) {
    QSignalBlocker b(m_actBones);
    m_actBones->setChecked(on);
  });

  m_actHelp = m_toolbar->addAction(makeIcon(Glyph::Help, ic), QString());
  m_actHelp->setCheckable(true);
  m_actHelp->setChecked(true);
  m_actHelp->setToolTip(QStringLiteral("操作方法の表示 (H)"));
  connect(m_actHelp, &QAction::toggled, m_view, &ModelView::setShowHelp);
  connect(m_view, &ModelView::showHelpChanged, this, [this](bool on) {
    QSignalBlocker b(m_actHelp);
    m_actHelp->setChecked(on);
  });

  m_actPlay = m_toolbar->addAction(makeIcon(Glyph::Pause, ic), QString());
  m_actPlay->setCheckable(true);
  m_actPlay->setChecked(true);
  m_actPlay->setToolTip(QStringLiteral("アニメ再生 / 一時停止 (Space)"));
  connect(m_actPlay, &QAction::toggled, m_view, &ModelView::setAnimationPlaying);
  connect(m_actPlay, &QAction::toggled, this,
          [this, ic](bool on) { m_actPlay->setIcon(makeIcon(on ? Glyph::Pause : Glyph::Play, ic)); });
  connect(m_view, &ModelView::animationPlayingChanged, this, [this, ic](bool on) {
    QSignalBlocker b(m_actPlay);
    m_actPlay->setChecked(on);
    m_actPlay->setIcon(makeIcon(on ? Glyph::Pause : Glyph::Play, ic));
  });

  QAction* actReset = m_toolbar->addAction(makeIcon(Glyph::Reset, ic), QString());
  actReset->setToolTip(QStringLiteral("視点をリセット (R)"));
  connect(actReset, &QAction::triggered, m_view, &ModelView::resetView);

  m_toolbar->addSeparator();
  m_pathLabel = new QLabel(m_toolbar);
  m_pathLabel->setStyleSheet(QStringLiteral("color:#8a929c; padding:0 8px;"));
  m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_toolbar->addWidget(m_pathLabel);

  // 右端に情報ボタンを寄せるためのスペーサー。
  auto* spacer = new QWidget(m_toolbar);
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  m_toolbar->addWidget(spacer);

  // 情報ボタン: 画像ビュアーと同じデザイン (太字イタリックの「i」テキスト)。
  m_infoButton = new QToolButton(m_toolbar);
  m_infoButton->setText(QStringLiteral("i"));
  QFont infoFont = m_infoButton->font();
  infoFont.setItalic(true);
  infoFont.setBold(true);
  m_infoButton->setFont(infoFont);
  m_infoButton->setToolTip(QStringLiteral("ノード情報パネルの表示 / 非表示 (i)"));
  m_infoButton->setFocusPolicy(Qt::NoFocus);
  m_infoButton->setCheckable(true);  // 表示中は押した状態
  connect(m_infoButton, &QToolButton::clicked, this, &ModelViewerWidget::toggleInfoPanel);
  m_toolbar->addWidget(m_infoButton);
  connect(m_view, &ModelView::infoRequested, this, &ModelViewerWidget::toggleInfoPanel);
}

// 右側の情報パネル (ノード階層 + 選択ノードの詳細) を作る。
QWidget* ModelViewerWidget::buildInfoPanel() {
  auto* panel = new QWidget(this);
  panel->setMinimumWidth(220);
  auto* vl = new QVBoxLayout(panel);
  vl->setContentsMargins(8, 8, 8, 8);

  auto* row = new QHBoxLayout;
  row->addWidget(new QLabel(QStringLiteral("ノード階層"), panel));
  row->addStretch(1);
  auto* expandAll   = new QPushButton(QStringLiteral("すべて展開"), panel);
  auto* collapseAll = new QPushButton(QStringLiteral("すべて閉じる"), panel);
  for (QPushButton* b : {expandAll, collapseAll}) {
    b->setAutoDefault(false);
    b->setDefault(false);
    b->setFocusPolicy(Qt::TabFocus);  // クリックではフォーカスを奪わない (ツリーに残る)
  }
  row->addWidget(expandAll);
  row->addWidget(collapseAll);
  vl->addLayout(row);

  m_infoTree = new NodeTree(panel);
  m_infoTree->setHeaderHidden(true);
  m_infoTree->setUniformRowHeights(true);
  m_infoTree->setIndentation(18);
  m_infoTree->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  m_infoTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_infoTree->setFocusPolicy(Qt::StrongFocus);
  m_infoTree->setMinimumHeight(60);
  m_infoTree->installEventFilter(this);

  m_nodeText = new QPlainTextEdit(panel);
  m_nodeText->setReadOnly(true);
  m_nodeText->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_nodeText->setPlaceholderText(QStringLiteral("ノードを選ぶと、ここに詳細を表示します"));
  m_nodeText->setMinimumHeight(48);
  m_nodeText->installEventFilter(this);

  // 階層 (上) と、選択中のノードの詳細 (下) は、つまみで高さを変えられる分割。
  auto* split = new QSplitter(Qt::Vertical, panel);
  split->addWidget(m_infoTree);
  split->addWidget(m_nodeText);
  split->setStretchFactor(0, 3);
  split->setStretchFactor(1, 2);
  split->setChildrenCollapsible(false);
  vl->addWidget(split, 1);

  connect(m_infoTree, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
            if (!cur) {
              m_nodeText->clear();
              m_view->setHighlightNode(-1);
              return;
            }
            const int node = cur->data(0, Qt::UserRole).toInt();
            m_nodeText->setPlainText(m_view->nodeInfoLines(node).join(QStringLiteral("\n")));
            // 選択したノードを 3D ビューでも強調する (パネルを開いているときだけ)。
            m_view->setHighlightNode(m_infoPanel && m_infoPanel->isVisible() ? node : -1);
          });
  // ボタンを押したら、フォーカスはツリーに戻す (Tab で当てたボタンの枠を残さない)。
  connect(expandAll, &QPushButton::clicked, this, [this] {
    m_infoTree->expandAll();
    m_infoTree->setFocus();
  });
  connect(collapseAll, &QPushButton::clicked, this, [this] {
    m_infoTree->collapseAll();
    for (int i = 0; i < m_infoTree->topLevelItemCount(); ++i) {
      m_infoTree->topLevelItem(i)->setExpanded(true);  // 最上位は開いたまま
    }
    m_infoTree->setFocus();
  });
  // Tab の順は ツリー → 一括展開 → 一括閉じ → 詳細。
  QWidget::setTabOrder(m_infoTree, expandAll);
  QWidget::setTabOrder(expandAll, collapseAll);
  QWidget::setTabOrder(collapseAll, m_nodeText);
  return panel;
}

// 情報パネルの表示 / 非表示 (i キー・「i」ボタン)。表示したらツリーに、隠したらビューにフォーカス。
void ModelViewerWidget::toggleInfoPanel() {
  const bool show = !m_infoPanel->isVisible();
  m_infoPanel->setVisible(show);
  {
    QSignalBlocker b(m_infoButton);
    m_infoButton->setChecked(show);
  }
  if (show) {
    const int total  = m_split->width();
    const int panelW = std::clamp(total / 3, 260, 420);
    m_split->setSizes({std::max(120, total - panelW), panelW});
    m_infoTree->setFocus();  // 開いてすぐ、矢印キー・クリックでツリーを操作できる
    if (auto* cur = m_infoTree->currentItem()) m_view->setHighlightNode(cur->data(0, Qt::UserRole).toInt());
  } else {
    m_view->setHighlightNode(-1);  // パネルを閉じたら強調も解除
    m_view->setFocus();
  }
}

// パネル内にフォーカスがあるときも、i / Esc で閉じられるようにする。
bool ModelViewerWidget::eventFilter(QObject* obj, QEvent* ev) {
  if ((obj == m_infoTree || obj == m_nodeText) && ev->type() == QEvent::KeyPress) {
    auto* ke = static_cast<QKeyEvent*>(ev);
    if ((ke->key() == Qt::Key_I && ke->modifiers() == Qt::NoModifier) || ke->key() == Qt::Key_Escape) {
      toggleInfoPanel();
      return true;
    }
  }
  return QWidget::eventFilter(obj, ev);
}

// 情報パネルのノード階層を、いまのモデルで作り直す。
void ModelViewerWidget::fillInfoPanel() {
  m_infoTree->clear();
  std::vector<QTreeWidgetItem*> stack;  // 深さごとの直近のノード
  for (const ModelView::InfoNode& n : m_view->infoTree()) {
    QTreeWidgetItem* item = new QTreeWidgetItem(QStringList{n.label});
    item->setData(0, Qt::UserRole, n.node);
    while (int(stack.size()) > n.depth) {
      stack.pop_back();
    }
    if (stack.empty()) {
      m_infoTree->addTopLevelItem(item);
    } else {
      stack.back()->addChild(item);
    }
    stack.push_back(item);
  }
  m_infoTree->expandAll();
  if (m_infoTree->topLevelItemCount() > 0) m_infoTree->setCurrentItem(m_infoTree->topLevelItem(0));
}

void ModelViewerWidget::refreshToolbar() {
  const bool hasTex = m_view->hasTexture();
  m_actTexture->setEnabled(hasTex);
  {
    QSignalBlocker b(m_actTexture);
    m_actTexture->setChecked(hasTex);
  }
  m_actPlay->setVisible(m_view->hasAnimation());
  m_actBones->setVisible(m_view->hasSkeleton());

  const QStringList ext = m_view->resolvedTexturePaths() + m_view->unresolvedTexturePaths();
  if (!ext.isEmpty()) {
    m_pathLabel->setText(QStringLiteral("テクスチャ: ") + ext.join(QStringLiteral("  /  ")));
    m_pathLabel->show();
  } else if (m_view->hasEmbeddedTexture()) {
    m_pathLabel->setText(QStringLiteral("テクスチャ: 埋め込み"));
    m_pathLabel->show();
  } else {
    m_pathLabel->clear();
    m_pathLabel->hide();
  }
}

bool ModelViewerWidget::loadModel(const QString& path, QString* error) {
  const bool ok = m_view->loadModel(path, error);
  refreshToolbar();
  fillInfoPanel();
  return ok;
}

QImage      ModelViewerWidget::renderToImage() { return m_view->renderToImage(); }
QString     ModelViewerWidget::summary() const { return m_view->summary(); }
bool        ModelViewerWidget::hasAnimation() const { return m_view->hasAnimation(); }
double      ModelViewerWidget::animationDuration() const { return m_view->animationDuration(); }
bool        ModelViewerWidget::hasTexture() const { return m_view->hasTexture(); }
bool        ModelViewerWidget::hasEmbeddedTexture() const { return m_view->hasEmbeddedTexture(); }
bool        ModelViewerWidget::hasUV() const { return m_view->hasUV(); }
QStringList ModelViewerWidget::recordedTexturePaths() const { return m_view->recordedTexturePaths(); }
QStringList ModelViewerWidget::resolvedTexturePaths() const { return m_view->resolvedTexturePaths(); }
QStringList ModelViewerWidget::unresolvedTexturePaths() const {
  return m_view->unresolvedTexturePaths();
}
void ModelViewerWidget::setTextureEnabled(bool on) { m_view->setTextureEnabled(on); }
void ModelViewerWidget::setAnimationTime(double sec) { m_view->setAnimationTime(sec); }

} // namespace Farman

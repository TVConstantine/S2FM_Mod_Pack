#include "rs_mainwindow.h"

#include <QtGui/QtGui>
#include <math.h>
#include <string.h>

#include "rs_bridge.h"
#include "rs_log.h"
#include "rs_switch.h"
#include "rs_theme.h"
#include "rs_window.h"
#include "../../rtao/rtao_api.h"

#include <windows.h>

namespace {

// The tool list's icon (toolimages:s2fm_reshade/appicon.ico), from the game's bin\win64.
const char* const kIconFromExe = "/../../core/tools/images/s2fm_reshade/appicon.ico";
const int kPollMs = 300;
const int kRenderSamples = 64;    // the estimate in the status bar: S2FM's render refines this many samples
const int kControlWidth = 330;    // the controls' column, one width so they line up down a card

RsMainWindow* g_window = 0;

QString L(const char* s) { return QString::fromLatin1(s); }
QString U(const char* s) { return QString::fromUtf8(s); }

bool OnQtThread()
{
    QCoreApplication* app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

QColor Muted() { return RsC(0x9d, 0x93, 0xbe); }
const char* const kStageNames[3] = { "A \xC2\xB7 after the opaque surfaces", "B \xC2\xB7 every sample, after post",
                                     "C \xC2\xB7 the finished picture" };
const char* const kStageNotes[3] = {
    "On the lit scene before the sky, fog, glass and particles: ambient occlusion, global illumination.",
    "On every refinement sample, after S2FM's own post: depth of field, anti-aliasing, bloom.",
    "Once, on the finished picture: grading, LUTs, sharpening, grain.",
};
const char kStageLetters[4] = "ABC";

QPen GlyphPen(const QColor& c, double w = 1.5)
{
    QPen pen(c, w);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

// A circular arrow, as on a refresh button.
QIcon ResetIcon()
{
    QIcon icon;
    for (int mode = 0; mode < 2; ++mode) {
        const QColor c = mode == 0 ? Muted() : RsC(0xed, 0xe9, 0xfe);
        QPixmap pm(16, 16);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(GlyphPen(c, 1.5));
        const QPointF centre(8, 8);
        const double r = 5.0;
        p.drawArc(QRectF(centre.x() - r, centre.y() - r, 2 * r, 2 * r), 30 * 16, 290 * 16);
        const double a = 320.0 * 3.14159265 / 180.0;
        const QPointF end(centre.x() + r * cos(a), centre.y() - r * sin(a));
        const QPointF dir(-sin(a), -cos(a));
        const QPointF side(dir.y(), -dir.x());
        QPolygonF head;
        head << end + dir * 3.0 << end + side * 2.4 << end - side * 2.4;
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(head);
        p.end();
        icon.addPixmap(pm, mode == 0 ? QIcon::Normal : QIcon::Active);
    }
    return icon;
}

// A stage's mark in the list: a letter in a rounded square, in the accent.
QIcon StageIcon(int stage)
{
    QPixmap pm(18, 18);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(RsC(0x3b, 0x23, 0x61));
    p.drawRoundedRect(QRectF(0.5, 0.5, 17, 17), 4, 4);
    p.setPen(RsC(0xe9, 0xd5, 0xff));
    QFont f(L("Segoe UI"));
    f.setPixelSize(11);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRectF(0, 0, 18, 18), Qt::AlignCenter, QString(QChar(kStageLetters[stage])));
    p.end();
    return QIcon(pm);
}

// The combo box's arrow: a style sheet can only name a file for it, so one is drawn into the temp folder.
QString ChevronFile()
{
    const QString dir = QDir::tempPath() + L("/reshade_tool");
    QDir().mkpath(dir);
    const QString path = dir + (RsThemeStock() ? L("/chevron_s2fm.png") : L("/chevron.png"));
    QImage img(20, 20, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(GlyphPen(RsC(0xa7, 0x9f, 0xc4), 2.4));
    p.drawLine(QPointF(5, 8), QPointF(10, 13));
    p.drawLine(QPointF(10, 13), QPointF(15, 8));
    p.end();
    img.save(path, "PNG");
    return path;
}

// The effect list's collection arrows, closed (pointing right) and open (down), small.
QString BranchFile(bool open)
{
    const QString dir = QDir::tempPath() + L("/reshade_tool");
    QDir().mkpath(dir);
    const QString path = dir + (open ? L("/branch_open") : L("/branch_closed"))
                         + (RsThemeStock() ? L("_s2fm.png") : L(".png"));
    QImage img(12, 12, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(GlyphPen(RsC(0xa7, 0x9f, 0xc4), 1.6));
    if (open) {
        p.drawLine(QPointF(2.5, 4.5), QPointF(6, 8));
        p.drawLine(QPointF(6, 8), QPointF(9.5, 4.5));
    } else {
        p.drawLine(QPointF(4.5, 2.5), QPointF(8, 6));
        p.drawLine(QPointF(8, 6), QPointF(4.5, 9.5));
    }
    p.end();
    img.save(path, "PNG");
    return path;
}

QString CheckFile()
{
    const QString dir = QDir::tempPath() + L("/reshade_tool");
    QDir().mkpath(dir);
    const QString path = dir + (RsThemeStock() ? L("/check_s2fm.png") : L("/check.png"));
    QImage img(14, 14, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(RsC(0x7c, 0x3a, 0xed));
    p.drawRoundedRect(QRectF(0.5, 0.5, 13, 13), 3.5, 3.5);
    p.setPen(GlyphPen(RsC(0xff, 0xff, 0xff), 1.9));
    p.drawLine(QPointF(3.5, 7.2), QPointF(6.0, 9.8));
    p.drawLine(QPointF(6.0, 9.8), QPointF(10.6, 4.2));
    p.end();
    img.save(path, "PNG");
    return path;
}

QString StyleSheet()
{
    QString s = L(
        "#RsMain { background: #120f1c; }"
        "QWidget { font-family: 'Segoe UI'; font-size: 9pt; color: #ede9fe; }"
        "QDialog, QMessageBox { background: #151122; }"
        "QMenuBar { background: #191426; color: #ede9fe; border-bottom: 1px solid #2b213f; padding: 2px 4px; }"
        "QMenuBar::item { background: transparent; padding: 4px 10px; border-radius: 4px; }"
        "QMenuBar::item:selected { background: #3b2361; color: #ffffff; }"
        "QMenu { background: #1a1526; color: #ede9fe; border: 1px solid #3b2d54; padding: 4px; }"
        "QMenu::item { padding: 6px 30px 6px 24px; border-radius: 4px; }"
        "QMenu::item:selected { background: #3b2361; color: #ffffff; }"
        "QMenu::item:disabled { color: #63587c; }"
        "QMenu::separator { height: 1px; background: #3b2d54; margin: 4px 8px; }"
        "QFrame#header { background: #191426; border: none; border-bottom: 1px solid #2b213f; }"
        "QLabel#title { color: #ffffff; font-size: 15pt; font-weight: 600; }"
        "QLabel#subtitle { color: #a79fc4; }"
        "QLabel#headerLabel { color: #a79fc4; }"
        "QWidget#side { background: #0c0a13; border-right: 1px solid #2b213f; }"
        "QFrame#sideCard { background: #141020; border: 1px solid #2b213f; border-radius: 8px; }"
        "QLabel#sideTitle { color: #c084fc; font-size: 8pt; font-weight: 600; padding-left: 2px; }"
        "QLabel#sideCount { color: #6f658c; font-size: 8pt; }"
        "QSplitter#sideSplit::handle { background: transparent; height: 8px; }"
        "QPushButton#addEffect { background: #3b2361; border-color: #7c3aed; color: #ffffff; padding: 5px 12px; }"
        "QPushButton#addEffect:hover { background: #4c2882; }"
        "QPushButton#addEffect:disabled { background: #1e182c; border-color: #2b213f; color: #5a5075; }"
        "QPushButton#newPreset { padding: 5px 12px; }"
        "QLineEdit#search { background: #1e182c; border: 1px solid #3b2d54; border-radius: 14px;"
        " padding: 5px 12px; color: #ede9fe; selection-background-color: #4c2882; }"
        "QLineEdit#search:focus { border-color: #a855f7; }"
        "QTreeWidget#techniques, QTreeWidget#effects { background: transparent; border: none; outline: 0; }"
        "QTreeWidget#techniques::item { color: #ede9fe; padding: 6px 4px; }"
        "QTreeWidget#techniques::item:hover { background: #211a33; }"
        "QTreeWidget#techniques::item:selected { background: #2b2040; color: #ffffff; }"
        "QTreeWidget#techniques::branch { background: transparent; }"
        "QTreeWidget#effects::item { padding: 4px 2px; }"
        "QTreeWidget#effects::item:hover { background: #211a33; }"
        "QTreeWidget#effects::item:selected { background: #2b2040; color: #ffffff; }"
        "QTreeWidget#effects::branch { background: transparent; }"
        "QTreeWidget#effects::branch:has-children:closed { image: url(%3); }"
        "QTreeWidget#effects::branch:has-children:open { image: url(%4); }"
        "QScrollArea#pageScroll { background: #120f1c; border: none; }"
        "QWidget#page { background: #120f1c; }"
        "QLabel#pageTitle { color: #ffffff; font-size: 13pt; font-weight: 600; }"
        "QLabel#pageNote { color: #a79fc4; }"
        "QLabel#chip { color: #c4b5fd; background: #2e2342; border-radius: 7px; padding: 1px 8px; font-size: 7pt; }"
        "QLabel#chipDepth { color: #d4c48f; background: #3a3526; border-radius: 7px; padding: 1px 8px; font-size: 7pt; }"
        "QFrame#card { background: #151122; border: 1px solid #2b213f; border-radius: 8px; }"
        "QToolButton#cardTitle { color: #c084fc; font-size: 8pt; font-weight: 600; background: transparent;"
        " border: none; padding: 2px 4px; text-align: left; }"
        "QToolButton#cardTitle:hover { color: #e9d5ff; }"
        "QFrame#row { background: transparent; border: none; border-radius: 6px; }"
        "QFrame#row:hover { background: #211a33; }"
        "QLabel#rowName { color: #ede9fe; }"
        "QLabel#rowTip { color: #9d93be; font-size: 8pt; }"
        "QLabel#problem { color: #fb7185; }"
        "QSlider { background: transparent; min-height: 18px; }"
        "QSlider::groove:horizontal { height: 4px; background: #2b213f; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #7c3aed; border-radius: 2px; }"
        "QSlider::add-page:horizontal { background: #2b213f; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #ede9fe; border: 2px solid #7c3aed; width: 10px; height: 10px;"
        " margin: -6px 0px; border-radius: 7px; }"
        "QSlider::handle:horizontal:hover { background: #ffffff; border-color: #c084fc; }"
        "QSpinBox, QDoubleSpinBox, QLineEdit { background: #1e182c; border: 1px solid #3b2d54; border-radius: 5px;"
        " padding: 3px 6px; color: #ede9fe; selection-background-color: #4c2882; }"
        "QSpinBox:hover, QDoubleSpinBox:hover, QLineEdit:hover { border-color: #63478c; }"
        "QSpinBox:focus, QDoubleSpinBox:focus, QLineEdit:focus { border-color: #a855f7; }"
        "QComboBox { background: #1e182c; border: 1px solid #3b2d54; border-radius: 5px; padding: 4px 8px;"
        " color: #ede9fe; }"
        "QComboBox:hover { border-color: #63478c; }"
        "QComboBox:focus, QComboBox:on { border-color: #a855f7; }"
        "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 24px;"
        " border: none; }"
        "QComboBox::down-arrow { image: url(%1); width: 10px; height: 10px; }"
        "QComboBox QAbstractItemView { background: #1a1526; border: 1px solid #3b2d54; color: #ede9fe;"
        " selection-background-color: #3b2361; selection-color: #ffffff; outline: 0; }"
        "QToolButton#reset { background: transparent; border: none; border-radius: 5px; padding: 3px; }"
        "QToolButton#reset:hover { background: #2b2040; }"
        "QPushButton { background: #221b33; color: #ede9fe; border: 1px solid #483466; border-radius: 5px;"
        " padding: 5px 14px; }"
        "QPushButton:hover { background: #2b2040; border-color: #63478c; }"
        "QPushButton:pressed { background: #4c2882; }"
        "QPushButton#swatch { padding: 0px; min-width: 0px; border-radius: 4px; }"
        "QCheckBox::indicator, QTreeWidget::indicator { width: 14px; height: 14px; border-radius: 4px;"
        " border: 1px solid #483466; background: #1e182c; }"
        "QTreeWidget::indicator:checked { image: url(%2); border: none; background: transparent; }"
        "QStatusBar { background: #0c0a13; border-top: 1px solid #2b213f; }"
        "QStatusBar::item { border: none; }"
        "QStatusBar QLabel { color: #9d93be; padding: 0px 6px; }"
        "QLabel#statusLink { color: #c084fc; }"
        "QSplitter::handle { background: #2b213f; }"
        "QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: #36294d; border-radius: 3px; min-height: 30px; }"
        "QScrollBar::handle:vertical:hover { background: #63478c; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }");
    return RsThemeSheet(s.arg(ChevronFile(), CheckFile(), BranchFile(false), BranchFile(true)));
}

// Valve's greys when S2FM's palette is Valve's own (neutral), the purple otherwise; state.ini's [GENERAL] Theme
// (purple or s2fm) over that.
bool ThemeIsStock()
{
    const QString state = QCoreApplication::applicationDirPath() + L("/s2fm_reshade/state.ini");
    const QString chosen = QSettings(state, QSettings::IniFormat).value(L("GENERAL/Theme")).toString().toLower();
    const QColor w = QApplication::palette().color(QPalette::Active, QPalette::Window);
    const bool neutral = abs(w.red() - w.green()) <= 6 && abs(w.green() - w.blue()) <= 6;
    const bool stock = chosen == L("s2fm") ? true : chosen == L("purple") ? false : neutral;
    RsToolLog("theme: S2FM's window colour %02x%02x%02x, state.ini \"%s\": %s", w.red(), w.green(), w.blue(),
              chosen.toUtf8().constData(), stock ? "Valve's greys" : "purple");
    return stock;
}

QLabel* Label(const QString& text, const char* name, QWidget* parent)
{
    QLabel* l = new QLabel(text, parent);
    l->setObjectName(L(name));
    return l;
}

int Decimals(float step, float lo, float hi)
{
    float s = step > 0.0f ? step : (hi - lo) / 1000.0f;
    if (s <= 0.0f) s = 0.001f;
    int d = (int)ceil(-log10(s));
    return d < 0 ? 0 : (d > 5 ? 5 : d);
}

}   // namespace

// --- The list.

RsTechniqueTree::RsTechniqueTree(QWidget* parent) : QTreeWidget(parent)
{
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove);
}

void RsTechniqueTree::dropEvent(QDropEvent* event)
{
    QTreeWidgetItem* dragged = currentItem();
    QTreeWidgetItem* target = itemAt(event->pos());
    event->setDropAction(Qt::IgnoreAction);
    event->accept();
    if (!dragged || !dragged->parent() || !target) return;
    const int technique = dragged->data(0, Qt::UserRole).toInt();
    if (!target->parent()) {
        emit dropped(technique, target->data(0, Qt::UserRole).toInt(), -1);
        return;
    }
    const int stage = target->parent()->data(0, Qt::UserRole).toInt();
    int before = target->data(0, Qt::UserRole).toInt();
    if (dropIndicatorPosition() == QAbstractItemView::BelowItem) {
        QTreeWidgetItem* parent = target->parent();
        const int next = parent->indexOfChild(target) + 1;
        before = next < parent->childCount() ? parent->child(next)->data(0, Qt::UserRole).toInt() : -1;
    }
    emit dropped(technique, stage, before);
}

// --- The window.

RsMainWindow::RsMainWindow()
    : m_tree(0), m_presets(0), m_effectsOn(0), m_subtitle(0), m_search(0), m_effects(0), m_effectsCount(0),
      m_presetCount(0), m_addEffect(0), m_pageScroll(0),
      m_statusCompiled(0), m_statusSize(0), m_statusCost(0), m_statusMessage(0), m_poll(0), m_stageCombo(0),
      m_pageTechnique(-1), m_building(false), m_lastTechniques(-1), m_lastLoading(-1), m_rebuildQueued(false),
      m_rebuildList(false), m_rebuildPage(false), m_rtaoOn(0), m_rtaoRays(0), m_statusRtao(0), m_rtaoGet(0),
      m_rtaoSet(0)
{
    m_stageMs[0] = m_stageMs[1] = m_stageMs[2] = 0.0f;
    setObjectName(L("RsMain"));
    setWindowTitle(L("ReShade"));
    RsThemeStock() = ThemeIsStock();
    setWindowIcon(QIcon(QCoreApplication::applicationDirPath() + L(kIconFromExe)));
    // Only this window is styled: the game's own style sheet and palette stay as they are.
    setStyleSheet(StyleSheet());
    resize(1120, 780);

    QMenu* preset = menuBar()->addMenu(L("&Preset"));
    preset->addAction(L("&New..."), this, SLOT(onNewPreset()), QKeySequence(L("Ctrl+N")));
    preset->addAction(L("&Open..."), this, SLOT(onOpenPreset()), QKeySequence(L("Ctrl+O")));
    preset->addAction(L("&Save"), this, SLOT(onSave()), QKeySequence(L("Ctrl+S")));
    preset->addAction(L("Save &As..."), this, SLOT(onSaveAs()));
    preset->addSeparator();
    preset->addAction(L("&Reload effects"), this, SLOT(onReload()), QKeySequence(L("F5")));
    preset->addSeparator();
    preset->addAction(L("&Close"), this, SLOT(close()), QKeySequence::Close);
    QMenu* help = menuBar()->addMenu(L("&Help"));
    help->addAction(L("Compiler &messages..."), this, SLOT(onShowErrors()));
    help->addSeparator();
    help->addAction(L("&About ReShade"), this, SLOT(onAbout()));

    QWidget* root = new QWidget(this);
    QVBoxLayout* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // The header: the icon, the name, the preset and the switch.
    QFrame* header = new QFrame(root);
    header->setObjectName(L("header"));
    QHBoxLayout* h = new QHBoxLayout(header);
    h->setContentsMargins(18, 12, 18, 12);
    h->setSpacing(12);
    QLabel* icon = new QLabel(header);
    icon->setPixmap(windowIcon().pixmap(36, 36));
    icon->setFixedSize(36, 36);
    h->addWidget(icon);
    QVBoxLayout* names = new QVBoxLayout();
    names->setSpacing(0);
    names->addWidget(Label(L("ReShade"), "title", header));
    m_subtitle = Label(L("Effects inside S2FM's frame, on its own depth"), "subtitle", header);
    names->addWidget(m_subtitle);
    h->addLayout(names);
    h->addStretch(1);
    h->addWidget(Label(L("Preset"), "headerLabel", header));
    m_presets = new QComboBox(header);
    m_presets->setMinimumWidth(240);
    m_presets->setToolTip(L("The presets beside ReShade.ini; Preset > Open for another"));
    h->addWidget(m_presets);
    QPushButton* newPreset = new QPushButton(L("New"), header);
    newPreset->setObjectName(L("newPreset"));
    newPreset->setToolTip(L("A new, empty preset: add effects to it from the list on the left, then Save (Ctrl+N)"));
    h->addWidget(newPreset);
    QPushButton* reload = new QPushButton(L("Reload"), header);
    reload->setToolTip(L("Compile the effects again (F5)"));
    h->addWidget(reload);
    h->addSpacing(8);
    h->addWidget(Label(L("Effects"), "headerLabel", header));
    m_effectsOn = new RsSwitch(header);
    m_effectsOn->setToolTip(L("Every effect off and on, to compare"));
    h->addWidget(m_effectsOn);
    // RTAO, when the game's dxgi.dll is the RTAO package's: its switch and its rays. Its look is the camera's.
    if (HMODULE dxgi = GetModuleHandleA("dxgi.dll")) {
        m_rtaoGet = (void*)GetProcAddress(dxgi, "S2fmRtao_Get");
        m_rtaoSet = (void*)GetProcAddress(dxgi, "S2fmRtao_Set");
    }
    if (m_rtaoGet && m_rtaoSet) {
        h->addSpacing(8);
        h->addWidget(Label(L("RTAO"), "headerLabel", header));
        m_rtaoOn = new RsSwitch(header);
        m_rtaoOn->setToolTip(L("Ray traced ambient occlusion in S2FM's viewports and renders. How dark and how far it "
                               "reaches is the camera's: SSAO Radius, Bias and Strength"));
        h->addWidget(m_rtaoOn);
        m_rtaoRays = new QComboBox(header);
        const int rays[] = { 1, 2, 4, 8, 16 };
        for (int r : rays) m_rtaoRays->addItem(L("%1 rays").arg(r), r);
        m_rtaoRays->setToolTip(L("Rays a pixel each frame: more is smoother at once and costs more; S2FM's refinement "
                                 "averages the frames either way"));
        h->addWidget(m_rtaoRays);
        connect(m_rtaoOn, SIGNAL(toggled(bool)), this, SLOT(onRtaoOn(bool)));
        connect(m_rtaoRays, SIGNAL(activated(int)), this, SLOT(onRtaoRays(int)));
    }
    rootLayout->addWidget(header);

    QSplitter* split = new QSplitter(Qt::Horizontal, root);
    split->setHandleWidth(1);
    QWidget* side = new QWidget(split);
    side->setObjectName(L("side"));
    QVBoxLayout* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(10, 10, 10, 10);
    sideLayout->setSpacing(0);
    QSplitter* sideSplit = new QSplitter(Qt::Vertical, side);
    sideSplit->setObjectName(L("sideSplit"));
    sideSplit->setChildrenCollapsible(false);
    sideLayout->addWidget(sideSplit);

    // The preset's card.
    QFrame* presetCard = new QFrame(sideSplit);
    presetCard->setObjectName(L("sideCard"));
    QVBoxLayout* pc = new QVBoxLayout(presetCard);
    pc->setContentsMargins(10, 10, 6, 6);
    pc->setSpacing(6);
    QHBoxLayout* pcTitle = new QHBoxLayout();
    pcTitle->addWidget(Label(L("IN THIS PRESET"), "sideTitle", presetCard));
    pcTitle->addStretch(1);
    m_presetCount = Label(QString(), "sideCount", presetCard);
    pcTitle->addWidget(m_presetCount);
    pcTitle->addSpacing(6);
    pc->addLayout(pcTitle);
    m_tree = new RsTechniqueTree(presetCard);
    m_tree->setObjectName(L("techniques"));
    m_tree->setColumnCount(2);
    m_tree->setHeaderHidden(true);
    m_tree->header()->setResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(false);
    m_tree->setIndentation(14);
    m_tree->setRootIsDecorated(false);
    m_tree->setIconSize(QSize(18, 18));
    m_tree->setFocusPolicy(Qt::NoFocus);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setToolTip(L("Drag a technique to another stage, or within its stage to run it earlier or later"));
    pc->addWidget(m_tree, 1);

    // The effects folder's card.
    QFrame* addCard = new QFrame(sideSplit);
    addCard->setObjectName(L("sideCard"));
    QVBoxLayout* ac = new QVBoxLayout(addCard);
    ac->setContentsMargins(10, 10, 10, 10);
    ac->setSpacing(8);
    QHBoxLayout* acTitle = new QHBoxLayout();
    acTitle->addWidget(Label(L("ADD AN EFFECT"), "sideTitle", addCard));
    acTitle->addStretch(1);
    m_effectsCount = Label(QString(), "sideCount", addCard);
    acTitle->addWidget(m_effectsCount);
    ac->addLayout(acTitle);
    m_search = new QLineEdit(addCard);
    m_search->setObjectName(L("search"));
    m_search->setPlaceholderText(L("Search by effect or collection  (Ctrl+F)"));
    ac->addWidget(m_search);
    m_effects = new QTreeWidget(addCard);
    m_effects->setObjectName(L("effects"));
    m_effects->setColumnCount(2);
    m_effects->setHeaderHidden(true);
    m_effects->header()->setResizeMode(0, QHeaderView::Stretch);
    m_effects->header()->setResizeMode(1, QHeaderView::ResizeToContents);
    m_effects->header()->setStretchLastSection(false);
    m_effects->setIndentation(12);
    m_effects->setRootIsDecorated(true);
    m_effects->setAnimated(true);
    m_effects->setFocusPolicy(Qt::NoFocus);
    m_effects->setToolTip(L("Double-click an effect, or select it and Add, to put its techniques in the preset"));
    ac->addWidget(m_effects, 1);
    m_addEffect = new QPushButton(L("Add to the preset"), addCard);
    m_addEffect->setObjectName(L("addEffect"));
    m_addEffect->setEnabled(false);
    ac->addWidget(m_addEffect);
    sideSplit->addWidget(presetCard);
    sideSplit->addWidget(addCard);
    sideSplit->setStretchFactor(0, 3);
    sideSplit->setStretchFactor(1, 2);
    split->addWidget(side);

    m_pageScroll = new QScrollArea(split);
    m_pageScroll->setObjectName(L("pageScroll"));
    m_pageScroll->setWidgetResizable(true);
    m_pageScroll->setFrameShape(QFrame::NoFrame);
    m_pageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    split->addWidget(m_pageScroll);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes(QList<int>() << 350 << 770);
    rootLayout->addWidget(split, 1);
    setCentralWidget(root);

    m_statusCompiled = new QLabel(statusBar());
    m_statusCompiled->setObjectName(L("statusLink"));
    m_statusCompiled->setCursor(Qt::PointingHandCursor);
    m_statusCompiled->setToolTip(L("The compiler's messages"));
    m_statusCompiled->installEventFilter(this);
    m_statusSize = new QLabel(statusBar());
    m_statusCost = new QLabel(statusBar());
    m_statusMessage = new QLabel(statusBar());
    statusBar()->addWidget(m_statusCompiled);
    statusBar()->addWidget(m_statusSize);
    statusBar()->addWidget(m_statusCost, 1);
    statusBar()->addPermanentWidget(m_statusMessage);
    if (m_rtaoGet) {
        m_statusRtao = new QLabel(statusBar());
        statusBar()->addPermanentWidget(m_statusRtao);
    }
    statusBar()->setSizeGripEnabled(true);

    connect(m_presets, SIGNAL(activated(int)), this, SLOT(onPresetChosen(int)));
    connect(reload, SIGNAL(clicked()), this, SLOT(onReload()));
    connect(m_effectsOn, SIGNAL(toggled(bool)), this, SLOT(onEffectsOn(bool)));
    connect(m_tree, SIGNAL(itemSelectionChanged()), this, SLOT(onSelection()));
    connect(m_tree, SIGNAL(itemChanged(QTreeWidgetItem*, int)), this, SLOT(onItemChanged(QTreeWidgetItem*, int)));
    connect(m_tree, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(onTreeMenu(const QPoint&)));
    connect(m_tree, SIGNAL(dropped(int, int, int)), this, SLOT(onDropped(int, int, int)));
    connect(m_search, SIGNAL(textChanged(const QString&)), this, SLOT(onSearch(const QString&)));
    connect(m_effects, SIGNAL(itemDoubleClicked(QTreeWidgetItem*, int)), this, SLOT(onAddEffect(QTreeWidgetItem*, int)));
    connect(m_effects, SIGNAL(itemSelectionChanged()), this, SLOT(onEffectSelection()));
    connect(m_addEffect, SIGNAL(clicked()), this, SLOT(onAddSelected()));
    connect(newPreset, SIGNAL(clicked()), this, SLOT(onNewPreset()));
    new QShortcut(QKeySequence::Find, m_search, SLOT(setFocus()));

    const RsCore& core = RsCoreApi();
    if (!core.ok) {
        m_statusMessage->setText(U(core.why));
        RsToolLog("window without the core: %s", core.why);
    } else {
        m_effectsOn->setCheckedNow(core.effectsOn(-1) != 0);
        fillPresets();
        fillEffects();
    }
    buildList();
    buildPage();
    pollRtao();
    m_poll = new QTimer(this);
    connect(m_poll, SIGNAL(timeout()), this, SLOT(onPoll()));
    m_poll->start(kPollMs);
}

void RsMainWindow::closeEvent(QCloseEvent* event)
{
    // The tool lives until the tools quit; its window only goes out of sight.
    event->ignore();
    hide();
}

bool RsMainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_statusCompiled && event->type() == QEvent::MouseButtonRelease) {
        onShowErrors();
        return true;
    }
    // A wheel over a control that does not have the focus scrolls the page instead of changing it.
    if (event->type() == QEvent::Wheel) {
        QWidget* w = qobject_cast<QWidget*>(watched);
        if (w && !w->hasFocus() && (qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QComboBox*>(w)
                                    || qobject_cast<QSlider*>(w))) {
            event->ignore();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void RsMainWindow::fillPresets()
{
    const RsCore& core = RsCoreApi();
    std::vector<char> b(1 << 16);
    core.listPresets(&b[0], (int)b.size());
    m_presetPaths = QString::fromUtf8(&b[0]).split(QChar('\n'), QString::SkipEmptyParts);
    RsStatus st;
    core.status(&st);
    const QString current = QString::fromUtf8(st.preset);
    if (!current.isEmpty() && !m_presetPaths.contains(current, Qt::CaseInsensitive)) m_presetPaths.append(current);
    m_presets->blockSignals(true);
    m_presets->clear();
    m_presets->addItem(L("(none: effects off)"));
    int pick = 0;
    for (int i = 0; i < m_presetPaths.size(); ++i) {
        m_presets->addItem(QFileInfo(m_presetPaths[i]).fileName(), m_presetPaths[i]);
        m_presets->setItemData(i + 1, m_presetPaths[i], Qt::ToolTipRole);
        if (!current.isEmpty() && m_presetPaths[i].compare(current, Qt::CaseInsensitive) == 0) pick = i + 1;
    }
    m_presets->setCurrentIndex(pick);
    m_presets->blockSignals(false);
}

void RsMainWindow::fillEffects()
{
    std::vector<char> b(1 << 20);
    RsCoreApi().listEffects(&b[0], (int)b.size());
    // "file<TAB>collection" a line (a line with no collection from an older core).
    m_allEffects.clear();
    const QStringList lines = QString::fromUtf8(&b[0]).split(QChar('\n'), QString::SkipEmptyParts);
    for (int i = 0; i < lines.size(); ++i) {
        const QStringList parts = lines[i].split(QChar('\t'));
        Effect e;
        e.file = parts[0];
        e.collection = parts.size() > 1 && !parts[1].isEmpty() ? parts[1] : L("Effects");
        m_allEffects.push_back(e);
    }
    onSearch(m_search->text());
}

namespace {

// An effect's name as shown: its file without ".fx".
QString EffectTitle(const QString& file)
{
    return file.endsWith(L(".fx"), Qt::CaseInsensitive) ? file.left(file.size() - 3) : file;
}

}   // namespace

void RsMainWindow::onSearch(const QString& text)
{
    // Grouped by collection, the collections in name order (the package's own first, the loose files last), each
    // closed until a search opens those with a match.
    const QString wanted = text.trimmed();
    QMap<QString, QList<int> > groups;
    for (int i = 0; i < (int)m_allEffects.size(); ++i) {
        const Effect& e = m_allEffects[i];
        if (!wanted.isEmpty() && !e.file.contains(wanted, Qt::CaseInsensitive)
            && !e.collection.contains(wanted, Qt::CaseInsensitive))
            continue;
        groups[e.collection].append(i);
    }
    QStringList order = groups.keys();
    qSort(order.begin(), order.end(), [](const QString& a, const QString& b) {
        const int ra = a == L("S2FM") ? 0 : a == L("Shaders") || a == L("Effects") ? 2 : 1;
        const int rb = b == L("S2FM") ? 0 : b == L("Shaders") || b == L("Effects") ? 2 : 1;
        if (ra != rb) return ra < rb;
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    m_effects->setUpdatesEnabled(false);
    m_effects->clear();
    int shown = 0;
    for (int g = 0; g < order.size(); ++g) {
        const QList<int>& members = groups[order[g]];
        QTreeWidgetItem* group = new QTreeWidgetItem(m_effects);
        group->setText(0, order[g] == L("Shaders") || order[g] == L("Effects") ? L("Other effects") : order[g]);
        group->setText(1, QString::number(members.size()));
        group->setFlags(Qt::ItemIsEnabled);
        QFont f = group->font(0);
        f.setBold(true);
        group->setFont(0, f);
        group->setForeground(0, QBrush(RsC(0xc4, 0xb5, 0xfd)));
        group->setForeground(1, QBrush(RsC(0x6f, 0x65, 0x8c)));
        group->setSizeHint(0, QSize(0, 26));
        for (int k = 0; k < members.size(); ++k) {
            const Effect& e = m_allEffects[members[k]];
            QTreeWidgetItem* it = new QTreeWidgetItem(group);
            it->setText(0, EffectTitle(e.file));
            it->setData(0, Qt::UserRole, e.file);
            it->setToolTip(0, e.file + U("  \xC2\xB7  ") + e.collection);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            ++shown;
        }
        group->setExpanded(!wanted.isEmpty());
    }
    m_effects->setUpdatesEnabled(true);
    m_addEffect->setEnabled(false);
    markAdded();
    if (m_allEffects.empty())
        m_effectsCount->setText(L("none found"));
    else if (wanted.isEmpty())
        m_effectsCount->setText(L("%1 in %2 collections").arg(m_allEffects.size()).arg(order.size()));
    else
        m_effectsCount->setText(L("%1 found").arg(shown));
}

void RsMainWindow::markAdded()
{
    const RsCore& core = RsCoreApi();
    m_inPreset.clear();
    const int n = core.ok ? core.techniqueCount() : 0;
    int on = 0;
    for (int i = 0; i < n; ++i) {
        RsTechniqueInfo ti;
        if (!core.techniqueInfo(i, &ti)) continue;
        if (ti.enabled) ++on;
        const QString file = QFileInfo(U(ti.effect)).fileName();
        if (!m_inPreset.contains(file, Qt::CaseInsensitive)) m_inPreset.append(file);
    }
    if (m_presetCount)
        m_presetCount->setText(n ? L("%1 of %2 on").arg(on).arg(n) : QString());
    if (!m_effects) return;
    for (int g = 0; g < m_effects->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = m_effects->topLevelItem(g);
        for (int c = 0; c < group->childCount(); ++c) {
            QTreeWidgetItem* it = group->child(c);
            const bool in = m_inPreset.contains(it->data(0, Qt::UserRole).toString(), Qt::CaseInsensitive);
            it->setText(1, in ? U("in preset") : QString());
            it->setForeground(0, QBrush(in ? RsC(0x7a, 0x70, 0x95) : RsC(0xed, 0xe9, 0xfe)));
            it->setForeground(1, QBrush(RsC(0xa8, 0x55, 0xf7)));
        }
    }
    onEffectSelection();
}

void RsMainWindow::onEffectSelection()
{
    QList<QTreeWidgetItem*> sel = m_effects->selectedItems();
    const bool effect = !sel.isEmpty() && sel[0]->parent();
    const bool in = effect && m_inPreset.contains(sel[0]->data(0, Qt::UserRole).toString(), Qt::CaseInsensitive);
    m_addEffect->setEnabled(effect && !in);
    m_addEffect->setText(!effect ? L("Add to the preset")
                         : in ? L("%1 is in the preset").arg(sel[0]->text(0))
                              : L("Add %1 to the preset").arg(sel[0]->text(0)));
}

void RsMainWindow::onAddSelected()
{
    QList<QTreeWidgetItem*> sel = m_effects->selectedItems();
    if (!sel.isEmpty()) onAddEffect(sel[0], 0);
}

QString RsMainWindow::stageTitle(int stage, float ms) const
{
    QString t = U(kStageNames[stage]);
    if (ms > 0.005f) t += U("   \xC2\xB7  %1 ms").arg(ms, 0, 'f', 2);
    return t;
}

int RsMainWindow::selectedTechnique() const
{
    QList<QTreeWidgetItem*> sel = m_tree->selectedItems();
    if (sel.isEmpty() || !sel[0]->parent()) return -1;
    return sel[0]->data(0, Qt::UserRole).toInt();
}

void RsMainWindow::selectTechnique(const QString& name)
{
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = m_tree->topLevelItem(g);
        for (int c = 0; c < group->childCount(); ++c)
            if (group->child(c)->data(0, Qt::UserRole + 1).toString() == name) {
                m_tree->setCurrentItem(group->child(c));
                return;
            }
    }
}

void RsMainWindow::buildList()
{
    const RsCore& core = RsCoreApi();
    m_building = true;
    m_tree->clear();
    QTreeWidgetItem* groups[3];
    for (int s = 0; s < 3; ++s) {
        groups[s] = new QTreeWidgetItem(m_tree);
        groups[s]->setText(0, stageTitle(s, m_stageMs[s]));
        groups[s]->setIcon(0, StageIcon(s));
        groups[s]->setToolTip(0, U(kStageNotes[s]));
        groups[s]->setData(0, Qt::UserRole, s);
        groups[s]->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
        groups[s]->setFirstColumnSpanned(true);
        QFont f = groups[s]->font(0);
        f.setBold(true);
        f.setPointSizeF(8.5);
        groups[s]->setFont(0, f);
        groups[s]->setForeground(0, QBrush(RsC(0xc0, 0x84, 0xfc)));
        groups[s]->setSizeHint(0, QSize(0, 30));
    }
    const int n = core.ok ? core.techniqueCount() : 0;
    for (int i = 0; i < n; ++i) {
        RsTechniqueInfo ti;
        if (!core.techniqueInfo(i, &ti)) continue;
        QTreeWidgetItem* it = new QTreeWidgetItem(groups[ti.stage < 0 || ti.stage > 2 ? 2 : ti.stage]);
        it->setText(0, U(ti.name));
        it->setToolTip(0, U(ti.effect) + (ti.readsDepth ? U(" \xC2\xB7 reads the depth") : QString()));
        it->setText(1, ti.overridden ? U("set \xC2\xB7 %1").arg(QChar(kStageLetters[ti.stage])) : L("auto"));
        it->setToolTip(1, L("The rules put it at %1").arg(QChar(kStageLetters[ti.autoStage])));
        it->setForeground(1, QBrush(ti.overridden ? RsC(0xc4, 0xb5, 0xfd) : RsC(0x6f, 0x65, 0x8c)));
        it->setData(0, Qt::UserRole, i);
        it->setData(0, Qt::UserRole + 1, U(ti.name));
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled);
        it->setCheckState(0, ti.enabled ? Qt::Checked : Qt::Unchecked);
        if (!ti.enabled) it->setForeground(0, QBrush(RsC(0x7a, 0x70, 0x95)));
    }
    for (int s = 0; s < 3; ++s) {
        if (groups[s]->childCount() == 0) {
            QTreeWidgetItem* empty = new QTreeWidgetItem(groups[s]);
            empty->setText(0, L("nothing runs here"));
            empty->setForeground(0, QBrush(RsC(0x5a, 0x50, 0x75)));
            empty->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
            empty->setData(0, Qt::UserRole, -1);
        }
    }
    m_tree->expandAll();
    m_building = false;
    markAdded();
    if (!m_selectedName.isEmpty()) selectTechnique(m_selectedName);
    // Nothing chosen yet: the first technique that runs, so its settings are there to see.
    if (m_tree->selectedItems().isEmpty()) {
        for (int g = 0; g < m_tree->topLevelItemCount() && m_tree->selectedItems().isEmpty(); ++g) {
            QTreeWidgetItem* group = m_tree->topLevelItem(g);
            for (int c = 0; c < group->childCount(); ++c)
                if (group->child(c)->data(0, Qt::UserRole).toInt() >= 0 && group->child(c)->checkState(0) == Qt::Checked) {
                    m_tree->setCurrentItem(group->child(c));
                    break;
                }
        }
    }
}

void RsMainWindow::pollRtao()
{
    if (!m_rtaoGet) return;
    S2fmRtaoState s;
    if (!((S2fmRtao_Get_Fn)m_rtaoGet)(&s) || s.apiVersion != S2FM_RTAO_API_VERSION) return;
    if ((s.enabled != 0) != m_rtaoOn->isChecked()) m_rtaoOn->setCheckedNow(s.enabled != 0);
    const int at = m_rtaoRays->findData(s.rays);
    if (at >= 0 && at != m_rtaoRays->currentIndex()) {
        m_rtaoRays->blockSignals(true);
        m_rtaoRays->setCurrentIndex(at);
        m_rtaoRays->blockSignals(false);
    }
    m_rtaoOn->setEnabled(!s.unavailable);
    m_rtaoRays->setEnabled(!s.unavailable && s.enabled);
    if (s.running)
        m_statusRtao->setText(U("RTAO  %1k triangles \xC2\xB7 %2 MB \xC2\xB7 %3 fps")
                                  .arg((s.triangles + 500) / 1000).arg(s.megabytes, 0, 'f', 0)
                                  .arg(s.framesPerSecond, 0, 'f', 0));
    else
        m_statusRtao->setText(L("RTAO: ") + U(s.why));
}

void RsMainWindow::onRtaoOn(bool on)
{
    if (!m_rtaoSet) return;
    ((S2fmRtao_Set_Fn)m_rtaoSet)(on ? 1 : 0, m_rtaoRays->itemData(m_rtaoRays->currentIndex()).toInt());
    RsToolLog("RTAO %s", on ? "on" : "off");
}

void RsMainWindow::onRtaoRays(int index)
{
    if (!m_rtaoSet) return;
    ((S2fmRtao_Set_Fn)m_rtaoSet)(m_rtaoOn->isChecked() ? 1 : 0, m_rtaoRays->itemData(index).toInt());
    RsToolLog("RTAO rays %d", m_rtaoRays->itemData(index).toInt());
}

void RsMainWindow::onPoll()
{
    pollRtao();
    const RsCore& core = RsCoreApi();
    if (!core.ok) return;
    RsStatus st;
    if (!core.status(&st)) return;
    const QString preset = QString::fromUtf8(st.preset);
    bool costChanged = false;
    for (int s = 0; s < 3; ++s) {
        if (fabs(st.stageMs[s] - m_stageMs[s]) > 0.01f) costChanged = true;
        m_stageMs[s] = st.stageMs[s];
    }
    if (st.techniques != m_lastTechniques || st.loading != m_lastLoading || preset != m_lastPreset) {
        m_lastTechniques = st.techniques;
        m_lastLoading = st.loading;
        if (preset != m_lastPreset) { m_lastPreset = preset; fillPresets(); }
        buildList();
        buildPage();
    } else if (costChanged) {
        for (int s = 0; s < 3 && s < m_tree->topLevelItemCount(); ++s)
            m_tree->topLevelItem(s)->setText(0, stageTitle(s, m_stageMs[s]));
    }
    m_subtitle->setText(preset.isEmpty() ? L("No preset: the effects are off")
                        : st.loading ? QFileInfo(preset).fileName() + U("  \xC2\xB7  compiling the effects...")
                        : QFileInfo(preset).fileName() + U("  \xC2\xB7  %1 technique%2").arg(st.techniques)
                                                                .arg(st.techniques == 1 ? L("") : L("s")));
    m_statusCompiled->setText(st.loading ? L("compiling...")
                              : L("%1 technique%2, %3 effect%4 failed").arg(st.techniques)
                                    .arg(st.techniques == 1 ? L("") : L("s")).arg(st.failed).arg(st.failed == 1 ? L("") : L("s")));
    m_statusSize->setText(st.running ? U("%1 \xC3\x97 %2").arg(st.width).arg(st.height) : QString());
    const double frame = (st.stageMs[0] + st.stageMs[1]) * kRenderSamples + st.stageMs[2];
    m_statusCost->setText(st.running && frame > 0.0
        ? U("A %1 \xC2\xB7 B %2 \xC2\xB7 C %3 ms   |   a render at %4 samples: about +%5 ms a frame")
              .arg(st.stageMs[0], 0, 'f', 2).arg(st.stageMs[1], 0, 'f', 2).arg(st.stageMs[2], 0, 'f', 2)
              .arg(kRenderSamples).arg(frame, 0, 'f', 0)
        : QString());
    m_statusMessage->setText(U(st.message));
    if ((st.effectsOn != 0) != m_effectsOn->isChecked()) m_effectsOn->setCheckedNow(st.effectsOn != 0);
}

void RsMainWindow::onPresetChosen(int index)
{
    const QString path = index > 0 ? m_presets->itemData(index).toString() : QString();
    m_selectedName.clear();
    RsCoreApi().loadPreset(path.toUtf8().constData());
    RsToolLog("preset chosen: %s", path.toUtf8().constData());
}

void RsMainWindow::onNewPreset()
{
    const QString start = QCoreApplication::applicationDirPath() + L("/NewPreset.ini");
    const QString path = QFileDialog::getSaveFileName(this, L("A new ReShade preset"), start, L("Presets (*.ini)"));
    if (path.isEmpty()) return;
    if (!newPreset(path)) QMessageBox::warning(this, L("ReShade"), L("The preset could not be made."));
}

bool RsMainWindow::newPreset(const QString& path)
{
    // An empty preset on disk (it names no technique, so no effect runs and nothing compiles), then chosen: the
    // effects added from the list go into it, and Save writes them.
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write("Techniques=\r\nTechniqueSorting=\r\n");
    f.close();
    m_selectedName.clear();
    RsCoreApi().loadPreset(QDir::toNativeSeparators(path).toUtf8().constData());
    fillPresets();
    m_statusMessage->setText(L("new preset ") + QFileInfo(path).fileName() + L(": add effects from the list, then Save"));
    RsToolLog("new preset: %s", QDir::toNativeSeparators(path).toUtf8().constData());
    return true;
}

void RsMainWindow::onOpenPreset()
{
    const QString start = QCoreApplication::applicationDirPath();
    const QString path = QFileDialog::getOpenFileName(this, L("Open a ReShade preset"), start, L("Presets (*.ini)"));
    if (path.isEmpty()) return;
    m_selectedName.clear();
    RsCoreApi().loadPreset(QDir::toNativeSeparators(path).toUtf8().constData());
}

void RsMainWindow::onSave()
{
    const RsCore& core = RsCoreApi();
    RsStatus st;
    core.status(&st);
    if (!st.preset[0]) { onSaveAs(); return; }
    if (st.loading) { QMessageBox::information(this, L("ReShade"), L("The effects are still compiling: save once they are done.")); return; }
    if (!core.savePreset(0)) QMessageBox::warning(this, L("ReShade"), L("The preset could not be saved."));
    else m_statusMessage->setText(L("saved ") + QFileInfo(U(st.preset)).fileName());
}

void RsMainWindow::onSaveAs()
{
    const QString start = QCoreApplication::applicationDirPath();
    const QString path = QFileDialog::getSaveFileName(this, L("Save the preset as"), start, L("Presets (*.ini)"));
    if (path.isEmpty()) return;
    const QByteArray p = QDir::toNativeSeparators(path).toUtf8();
    RsStatus st;
    RsCoreApi().status(&st);
    if (st.loading) { QMessageBox::information(this, L("ReShade"), L("The effects are still compiling: save once they are done.")); return; }
    if (!RsCoreApi().savePreset(p.constData())) QMessageBox::warning(this, L("ReShade"), L("The preset could not be saved."));
    else m_statusMessage->setText(L("saved ") + QFileInfo(path).fileName());
}

void RsMainWindow::onReload()
{
    RsCoreApi().reload();
    fillEffects();
}

void RsMainWindow::onEffectsOn(bool on)
{
    RsCoreApi().effectsOn(on ? 1 : 0);
}

void RsMainWindow::onSelection()
{
    if (m_building) return;
    QList<QTreeWidgetItem*> sel = m_tree->selectedItems();
    const int t = selectedTechnique();
    m_selectedName = t >= 0 && !sel.isEmpty() ? sel[0]->data(0, Qt::UserRole + 1).toString() : QString();
    buildPage();
}

void RsMainWindow::onItemChanged(QTreeWidgetItem* item, int column)
{
    if (m_building || !item || !item->parent() || column != 0) return;
    const int t = item->data(0, Qt::UserRole).toInt();
    if (t < 0) return;
    RsCoreApi().setTechniqueEnabled(t, item->checkState(0) == Qt::Checked ? 1 : 0);
    // Not now: Qt is inside the item's setData, and goes on with the item after this returns. Rebuilding the list
    // deleted it under Qt, and S2FM crashed.
    rebuildLater(true, false);
}

void RsMainWindow::rebuildLater(bool list, bool page)
{
    m_rebuildList = m_rebuildList || list;
    m_rebuildPage = m_rebuildPage || page;
    if (m_rebuildQueued) return;
    m_rebuildQueued = true;
    QTimer::singleShot(0, this, SLOT(onRebuild()));
}

void RsMainWindow::onRebuild()
{
    const bool list = m_rebuildList, page = m_rebuildPage;
    m_rebuildQueued = m_rebuildList = m_rebuildPage = false;
    if (list) buildList();
    if (page) buildPage();
}

bool RsMainWindow::testCheck(int technique, bool on)
{
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = m_tree->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem* item = group->child(i);
            if (item->data(0, Qt::UserRole).toInt() != technique) continue;
            item->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
            return true;
        }
    }
    return false;
}

// For the live tests, on Qt's thread: 1 when the technique was in
// the list and its tick was set.
extern "C" __declspec(dllexport) int RsTool_TestCheck(int technique, int on)
{
    if (!g_window || !OnQtThread()) return 0;
    return g_window->testCheck(technique, on != 0) ? 1 : 0;
}

// For the live tests, on Qt's thread: the RTAO switch set as a click sets it (on 0 or 1;
// -1 leaves it), and the RTAO text of the status bar. 0 when the window has no RTAO controls.
extern "C" __declspec(dllexport) int RsTool_TestRtao(int on, char* status, int size)
{
    if (!g_window || !OnQtThread()) return 0;
    return g_window->testRtao(on, status, size) ? 1 : 0;
}

// For the live tests, on Qt's thread: a new preset made at the path as New makes it.
extern "C" __declspec(dllexport) int RsTool_TestNewPreset(const char* path)
{
    if (!g_window || !OnQtThread() || !path) return 0;
    return g_window->newPreset(QString::fromUtf8(path)) ? 1 : 0;
}

// For the live tests, on Qt's thread: the window drawn into a PNG at the path.
extern "C" __declspec(dllexport) int RsTool_TestGrab(const char* path)
{
    if (!g_window || !OnQtThread() || !path) return 0;
    return g_window->testGrab(QString::fromUtf8(path)) ? 1 : 0;
}

// For the live tests, on Qt's thread: the search typed into the effect list (null leaves it), and its state.
extern "C" __declspec(dllexport) int RsTool_TestEffects(const char* search, char* out, int size)
{
    if (!g_window || !OnQtThread()) return 0;
    const QByteArray t = g_window->testEffects(search ? QString::fromUtf8(search) : QString()).toUtf8();
    if (out && size > 0) strncpy_s(out, size, t.constData(), _TRUNCATE);
    return 1;
}

// For the live tests, on Qt's thread: the look set (-1 as decided at the start, 0 purple, 1 Valve's greys) and
// the window styled and built again; the look now (0 or 1).
extern "C" __declspec(dllexport) int RsTool_TestTheme(int look)
{
    if (!g_window || !OnQtThread()) return -1;
    return g_window->testTheme(look);
}

int RsMainWindow::testTheme(int look)
{
    RsThemeStock() = look < 0 ? ThemeIsStock() : look == 1;
    setStyleSheet(StyleSheet());
    buildList();
    buildPage();
    onSearch(m_search->text());
    foreach (QWidget* w, findChildren<QWidget*>()) w->update();
    return RsThemeStock() ? 1 : 0;
}

bool RsMainWindow::testGrab(const QString& path)
{
    show();
    raise();
    return QPixmap::grabWidget(this).save(path, "PNG");
}

QString RsMainWindow::testEffects(const QString& search)
{
    if (!search.isNull()) m_search->setText(search);
    int collections = m_effects->topLevelItemCount(), shown = 0, marked = 0;
    for (int g = 0; g < collections; ++g) {
        QTreeWidgetItem* group = m_effects->topLevelItem(g);
        shown += group->childCount();
        for (int c = 0; c < group->childCount(); ++c)
            if (!group->child(c)->text(1).isEmpty()) ++marked;
    }
    return L("effects %1, collections %2, in the preset %3, shown %4, marked %5")
        .arg(m_allEffects.size()).arg(collections).arg(m_inPreset.size()).arg(shown).arg(marked);
}

bool RsMainWindow::testRtao(int on, char* status, int size)
{
    if (!m_rtaoOn) return false;
    if (on == 0 || on == 1) m_rtaoOn->setChecked(on == 1);
    pollRtao();
    if (status && size > 0) {
        const QByteArray t = m_statusRtao->text().toUtf8();
        strncpy_s(status, size, t.constData(), _TRUNCATE);
    }
    return true;
}

void RsMainWindow::onTreeMenu(const QPoint& at)
{
    QTreeWidgetItem* item = m_tree->itemAt(at);
    if (!item || !item->parent()) return;
    const int t = item->data(0, Qt::UserRole).toInt();
    RsTechniqueInfo ti;
    if (t < 0 || !RsCoreApi().techniqueInfo(t, &ti)) return;
    QMenu menu(this);
    QAction* autoStage = menu.addAction(L("Stage: automatic (%1)").arg(QChar(kStageLetters[ti.autoStage])));
    QAction* stages[3];
    for (int s = 0; s < 3; ++s) stages[s] = menu.addAction(L("Stage ") + U(kStageNames[s]));
    menu.addSeparator();
    QAction* up = menu.addAction(L("Run earlier"));
    QAction* down = menu.addAction(L("Run later"));
    QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(at));
    if (!chosen) return;
    const RsCore& core = RsCoreApi();
    if (chosen == autoStage) core.setTechniqueStage(t, -1);
    for (int s = 0; s < 3; ++s) if (chosen == stages[s]) core.setTechniqueStage(t, s);
    if (chosen == up && t > 0) core.moveTechnique(t, t - 1);
    if (chosen == down) core.moveTechnique(t, t + 1);
    buildList();
}

void RsMainWindow::onDropped(int technique, int stage, int before)
{
    const RsCore& core = RsCoreApi();
    RsTechniqueInfo ti;
    if (!core.techniqueInfo(technique, &ti)) return;
    if (stage != ti.stage) core.setTechniqueStage(technique, stage == ti.autoStage ? -1 : stage);
    if (before >= 0 && before != technique) core.moveTechnique(technique, technique < before ? before - 1 : before);
    // Not now: the drop is still being handled by the list's own event.
    rebuildLater(true, false);
}

void RsMainWindow::onAddEffect(QTreeWidgetItem* item, int)
{
    if (!item || !item->parent()) return;
    const QString file = item->data(0, Qt::UserRole).toString();
    if (m_inPreset.contains(file, Qt::CaseInsensitive)) {
        m_statusMessage->setText(L("%1 is in the preset already").arg(item->text(0)));
        return;
    }
    RsStatus st;
    RsCoreApi().status(&st);
    if (!st.preset[0]) {
        m_statusMessage->setText(L("Choose a preset first, or make one with New"));
        return;
    }
    const QByteArray f = file.toUtf8();
    const int n = RsCoreApi().addEffect(f.constData());
    m_statusMessage->setText(n > 0 ? L("%1: %2 techniques added, compiling").arg(item->text(0)).arg(n)
                                   : L("%1: no technique found").arg(item->text(0)));
}

// --- The selected technique's page.

QFrame* RsMainWindow::card(const QString& title, QWidget* parent, QVBoxLayout** body, bool closed)
{
    QFrame* c = new QFrame(parent);
    c->setObjectName(L("card"));
    QVBoxLayout* cv = new QVBoxLayout(c);
    cv->setContentsMargins(8, 8, 8, 8);
    cv->setSpacing(0);
    QWidget* content = new QWidget(c);
    QVBoxLayout* inner = new QVBoxLayout(content);
    inner->setContentsMargins(0, 2, 0, 0);
    inner->setSpacing(0);
    if (!title.isEmpty()) {
        QToolButton* t = new QToolButton(c);
        t->setObjectName(L("cardTitle"));
        t->setText(title.toUpper());
        t->setCheckable(true);
        t->setChecked(!closed);
        t->setArrowType(closed ? Qt::RightArrow : Qt::DownArrow);
        t->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        t->setCursor(Qt::PointingHandCursor);
        t->setProperty("content", qVariantFromValue((void*)content));
        connect(t, SIGNAL(clicked()), this, SLOT(onToggleCard()));
        cv->addWidget(t);
        content->setVisible(!closed);
    }
    cv->addWidget(content);
    *body = inner;
    return c;
}

QFrame* RsMainWindow::row(const QString& name, const QString& tip, QWidget* control, QWidget* parent, QWidget* reset)
{
    QFrame* frame = new QFrame(parent);
    frame->setObjectName(L("row"));
    frame->setAttribute(Qt::WA_Hover);
    QHBoxLayout* h = new QHBoxLayout(frame);
    h->setContentsMargins(10, 7, 6, 7);
    h->setSpacing(16);
    QVBoxLayout* text = new QVBoxLayout();
    text->setSpacing(2);
    QLabel* n = Label(name, "rowName", frame);
    n->setWordWrap(true);
    text->addWidget(n);
    if (!tip.isEmpty()) {
        QLabel* t = Label(tip, "rowTip", frame);
        t->setWordWrap(true);
        text->addWidget(t);
    }
    h->addLayout(text, 1);
    control->setParent(frame);
    control->setFixedWidth(kControlWidth);
    h->addWidget(control);
    if (reset) {
        reset->setParent(frame);
        h->addWidget(reset);
    } else {
        h->addSpacing(28);
    }
    return frame;
}

void RsMainWindow::onToggleCard()
{
    QToolButton* t = qobject_cast<QToolButton*>(sender());
    if (!t) return;
    QWidget* content = (QWidget*)t->property("content").value<void*>();
    if (content) content->setVisible(t->isChecked());
    t->setArrowType(t->isChecked() ? Qt::DownArrow : Qt::RightArrow);
}

void RsMainWindow::buildPage()
{
    const RsCore& core = RsCoreApi();
    m_settings.clear();
    m_definitions.clear();
    m_stageCombo = 0;
    QWidget* page = new QWidget;
    page->setObjectName(L("page"));
    QVBoxLayout* v = new QVBoxLayout(page);
    v->setContentsMargins(26, 20, 26, 24);
    v->setSpacing(14);
    const int t = selectedTechnique();
    m_pageTechnique = t;
    RsTechniqueInfo ti;
    if (!core.ok || t < 0 || !core.techniqueInfo(t, &ti)) {
        RsStatus st;
        const bool any = core.ok && core.status(&st) && st.preset[0];
        v->addWidget(Label(core.ok ? (any ? (core.techniqueCount() == 0 && !st.loading ? L("An empty preset")
                                                                                       : L("Choose a technique"))
                                          : L("No preset"))
                                   : L("The core is not running"),
                           "pageTitle", page));
        const bool none = any && core.techniqueCount() == 0 && !st.loading;
        QLabel* note = Label(!core.ok ? U(core.why)
                             : none ? L("This preset has no effects yet. Add some from the list on the left (double-click "
                                        "an effect, or select it and Add), then save it with Preset > Save.")
                             : any ? L("Its settings come up here. Drag a technique to another stage to run it there.")
                                   : L("Pick a preset above, or open one from the Preset menu: until then no effect runs."),
                             "pageNote", page);
        note->setWordWrap(true);
        v->addWidget(note);
        v->addStretch(1);
        m_pageScroll->setWidget(page);
        return;
    }

    // The title, with the effect file and chips.
    QHBoxLayout* titleLine = new QHBoxLayout();
    titleLine->setSpacing(10);
    titleLine->addWidget(Label(U(ti.name), "pageTitle", page));
    QLabel* stageChip = Label(L("stage %1").arg(QChar(kStageLetters[ti.stage])), "chip", page);
    titleLine->addWidget(stageChip);
    if (ti.readsDepth) {
        QLabel* depth = Label(L("reads the depth"), "chipDepth", page);
        depth->setToolTip(L("Given the scene's own linear depth"));
        titleLine->addWidget(depth);
    }
    titleLine->addStretch(1);
    v->addLayout(titleLine);
    v->addWidget(Label(U(ti.effect), "pageNote", page));

    // Where it runs.
    QVBoxLayout* body = 0;
    QFrame* where = card(L("Runs at"), page, &body, false);
    m_stageCombo = new QComboBox;
    m_stageCombo->addItem(L("Automatic  (%1)").arg(U(kStageNames[ti.autoStage])));
    for (int s = 0; s < 3; ++s) m_stageCombo->addItem(U(kStageNames[s]));
    m_stageCombo->setCurrentIndex(ti.overridden ? ti.stage + 1 : 0);
    m_stageCombo->installEventFilter(this);
    connect(m_stageCombo, SIGNAL(activated(int)), this, SLOT(onStageCombo(int)));
    body->addWidget(row(L("Stage"), U(kStageNotes[ti.stage]), m_stageCombo, where, 0));
    v->addWidget(where);

    std::vector<char> err(1 << 16);
    if (core.effectErrors(t, &err[0], (int)err.size()) > 0) {
        QFrame* msg = card(L("Compiler"), page, &body, false);
        QPushButton* show = new QPushButton(L("Show messages"));
        connect(show, SIGNAL(clicked()), this, SLOT(onShowErrors()));
        QWidget* holder = new QWidget;
        QHBoxLayout* hl = new QHBoxLayout(holder);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->addStretch(1);
        hl->addWidget(show);
        body->addWidget(row(L("The compiler had something to say about this effect"),
                            L("Warnings, mostly; the effect runs unless it failed."), holder, msg, 0));
        v->addWidget(msg);
    }

    // The settings, in ui_category cards, in the effect's order.
    const int n = core.uniformCount(t);
    QMap<QString, QVBoxLayout*> bodies;
    QMap<QString, QFrame*> cards;
    for (int u = 0; u < n; ++u) {
        Setting s;
        memset(&s.info, 0, sizeof(s.info));
        s.u = u;
        s.slider = 0;
        s.sw = 0;
        s.combo = 0;
        s.color = 0;
        if (!core.uniformInfo(t, u, &s.info)) continue;
        const QString cat = U(s.info.category).isEmpty() ? L("Settings") : U(s.info.category);
        if (!cards.contains(cat)) {
            QVBoxLayout* b = 0;
            cards[cat] = card(cat, page, &b, s.info.categoryClosed != 0);
            bodies[cat] = b;
            v->addWidget(cards[cat]);
        }
        const int index = (int)m_settings.size();
        QWidget* control = new QWidget;
        QHBoxLayout* cl = new QHBoxLayout(control);
        cl->setContentsMargins(0, 0, 0, 0);
        cl->setSpacing(8);
        const bool isFloat = s.info.baseType == 3;
        if (s.info.baseType == 0 && s.info.components == 1) {
            cl->addStretch(1);
            s.sw = new RsSwitch(control);
            s.sw->setProperty("setting", index);
            connect(s.sw, SIGNAL(toggled(bool)), this, SLOT(onSwitch(bool)));
            cl->addWidget(s.sw);
        } else if (s.info.itemCount > 0 && s.info.components == 1) {
            s.combo = new QComboBox(control);
            const char* p = s.info.items;
            for (int k = 0; k < s.info.itemCount && p < s.info.items + sizeof(s.info.items); ++k) {
                s.combo->addItem(U(p));
                p += strlen(p) + 1;
            }
            s.combo->setProperty("setting", index);
            s.combo->setFocusPolicy(Qt::StrongFocus);
            s.combo->installEventFilter(this);
            connect(s.combo, SIGNAL(activated(int)), this, SLOT(onCombo(int)));
            cl->addWidget(s.combo, 1);
        } else {
            const bool ranged = s.info.uiMax > s.info.uiMin;
            const bool colour = !strcmp(s.info.uiType, "color") && s.info.components >= 3;
            if (colour) {
                s.color = new QPushButton(control);
                s.color->setObjectName(L("swatch"));
                s.color->setFixedSize(30, 24);
                s.color->setProperty("setting", index);
                s.color->setToolTip(L("Choose the colour"));
                connect(s.color, SIGNAL(clicked()), this, SLOT(onColor()));
                cl->addWidget(s.color);
            }
            if (s.info.components == 1 && ranged && strcmp(s.info.uiType, "input")) {
                s.slider = new QSlider(Qt::Horizontal, control);
                s.slider->setRange(0, 1000);
                s.slider->setProperty("setting", index);
                s.slider->setFocusPolicy(Qt::StrongFocus);
                s.slider->installEventFilter(this);
                connect(s.slider, SIGNAL(valueChanged(int)), this, SLOT(onSlider(int)));
                cl->addWidget(s.slider, 1);
            } else {
                cl->addStretch(1);
            }
            for (int c = 0; c < s.info.components; ++c) {
                QAbstractSpinBox* box;
                if (isFloat) {
                    QDoubleSpinBox* d = new QDoubleSpinBox(control);
                    d->setDecimals(Decimals(s.info.uiStep, s.info.uiMin, s.info.uiMax));
                    d->setRange(ranged ? s.info.uiMin : -1e6, ranged ? s.info.uiMax : 1e6);
                    d->setSingleStep(s.info.uiStep > 0 ? s.info.uiStep : (ranged ? (s.info.uiMax - s.info.uiMin) / 100.0 : 0.01));
                    if (s.info.units[0]) d->setSuffix(U(s.info.units));
                    connect(d, SIGNAL(valueChanged(double)), this, SLOT(onSpin()));
                    box = d;
                } else {
                    QSpinBox* i = new QSpinBox(control);
                    i->setRange(ranged ? (int)s.info.uiMin : -1000000, ranged ? (int)s.info.uiMax : 1000000);
                    i->setSingleStep(s.info.uiStep >= 1 ? (int)s.info.uiStep : 1);
                    if (s.info.units[0]) i->setSuffix(U(s.info.units));
                    connect(i, SIGNAL(valueChanged(int)), this, SLOT(onSpin()));
                    box = i;
                }
                box->setProperty("setting", index);
                box->setButtonSymbols(QAbstractSpinBox::NoButtons);
                box->setKeyboardTracking(false);
                box->setAlignment(Qt::AlignRight);
                box->setFixedWidth(s.info.components > 1 ? 62 : 72);
                box->setFocusPolicy(Qt::StrongFocus);
                box->installEventFilter(this);
                s.spins.push_back(box);
                cl->addWidget(box);
            }
        }
        QToolButton* reset = new QToolButton;
        reset->setObjectName(L("reset"));
        reset->setIcon(ResetIcon());
        reset->setIconSize(QSize(16, 16));
        reset->setAutoRaise(true);
        reset->setCursor(Qt::PointingHandCursor);
        reset->setToolTip(L("Back to the effect's default"));
        reset->setProperty("setting", index);
        connect(reset, SIGNAL(clicked()), this, SLOT(onReset()));
        bodies[cat]->addWidget(row(U(s.info.label), U(s.info.tooltip), control, cards[cat], reset));
        m_settings.push_back(s);
        readValues(index);
    }
    if (n == 0) v->addWidget(Label(L("This effect has no settings."), "pageNote", page));

    // Its preprocessor definitions: changing one compiles the effect again.
    const int d = core.definitionCount(t);
    if (d > 0) {
        QFrame* defs = card(L("Preprocessor definitions"), page, &body, true);
        for (int k = 0; k < d; ++k) {
            char name[256] = "", value[512] = "";
            core.definition(t, k, name, sizeof(name), value, sizeof(value));
            QLineEdit* edit = new QLineEdit(U(value));
            edit->setProperty("name", U(name));
            connect(edit, SIGNAL(editingFinished()), this, SLOT(onDefinitionEdited()));
            m_definitions.push_back(edit);
            body->addWidget(row(U(name), QString(), edit, defs, 0));
        }
        v->addWidget(defs);
    }
    v->addStretch(1);
    m_pageScroll->setWidget(page);
}

void RsMainWindow::readValues(int si)
{
    if (si < 0 || si >= (int)m_settings.size()) return;
    Setting& s = m_settings[(size_t)si];
    float v[16] = {};
    if (!RsCoreApi().getUniform(m_pageTechnique, s.u, v, s.info.components)) return;
    m_building = true;
    if (s.sw) s.sw->setCheckedNow(v[0] != 0.0f);
    if (s.combo) s.combo->setCurrentIndex((int)v[0]);
    for (size_t c = 0; c < s.spins.size(); ++c) {
        if (QDoubleSpinBox* d = qobject_cast<QDoubleSpinBox*>(s.spins[c])) d->setValue(v[c]);
        else if (QSpinBox* i = qobject_cast<QSpinBox*>(s.spins[c])) i->setValue((int)floor(v[c] + 0.5f));
    }
    if (s.slider && s.info.uiMax > s.info.uiMin)
        s.slider->setValue((int)((v[0] - s.info.uiMin) / (s.info.uiMax - s.info.uiMin) * 1000.0f + 0.5f));
    if (s.color) {
        QColor c;
        c.setRgbF(qBound(0.0, (double)v[0], 1.0), qBound(0.0, (double)v[1], 1.0), qBound(0.0, (double)v[2], 1.0));
        s.color->setStyleSheet(L("background: %1; border: 1px solid #483466;").arg(c.name()));
    }
    m_building = false;
}

void RsMainWindow::sendValues(int si)
{
    if (si < 0 || si >= (int)m_settings.size()) return;
    Setting& s = m_settings[(size_t)si];
    float v[16] = {};
    if (s.sw) v[0] = s.sw->isChecked() ? 1.0f : 0.0f;
    if (s.combo) v[0] = (float)s.combo->currentIndex();
    for (size_t c = 0; c < s.spins.size(); ++c) {
        if (QDoubleSpinBox* d = qobject_cast<QDoubleSpinBox*>(s.spins[c])) v[c] = (float)d->value();
        else if (QSpinBox* i = qobject_cast<QSpinBox*>(s.spins[c])) v[c] = (float)i->value();
    }
    RsCoreApi().setUniform(m_pageTechnique, s.u, v, s.info.components);
    readValues(si);
}

void RsMainWindow::onStageCombo(int index)
{
    if (m_pageTechnique < 0) return;
    RsCoreApi().setTechniqueStage(m_pageTechnique, index - 1);
    // Not now: the combo is on the page, and Qt goes on with it after this returns.
    rebuildLater(true, true);
}

void RsMainWindow::onSlider(int value)
{
    if (m_building) return;
    const int si = sender()->property("setting").toInt();
    Setting& s = m_settings[(size_t)si];
    const double v = s.info.uiMin + (s.info.uiMax - s.info.uiMin) * value / 1000.0;
    m_building = true;
    if (!s.spins.empty()) {
        if (QDoubleSpinBox* d = qobject_cast<QDoubleSpinBox*>(s.spins[0])) d->setValue(v);
        else if (QSpinBox* i = qobject_cast<QSpinBox*>(s.spins[0])) i->setValue((int)floor(v + 0.5));
    }
    m_building = false;
    sendValues(si);
}

void RsMainWindow::onSpin()
{
    if (m_building) return;
    sendValues(sender()->property("setting").toInt());
}

void RsMainWindow::onSwitch(bool)
{
    if (m_building) return;
    sendValues(sender()->property("setting").toInt());
}

void RsMainWindow::onCombo(int)
{
    if (m_building) return;
    sendValues(sender()->property("setting").toInt());
}

void RsMainWindow::onColor()
{
    const int si = sender()->property("setting").toInt();
    Setting& s = m_settings[(size_t)si];
    float v[16] = {};
    RsCoreApi().getUniform(m_pageTechnique, s.u, v, s.info.components);
    QColor start;
    start.setRgbF(qBound(0.0, (double)v[0], 1.0), qBound(0.0, (double)v[1], 1.0), qBound(0.0, (double)v[2], 1.0));
    const QColor c = QColorDialog::getColor(start, this, U(s.info.label));
    if (!c.isValid()) return;
    m_building = true;
    const double rgb[3] = { c.redF(), c.greenF(), c.blueF() };
    for (size_t k = 0; k < s.spins.size() && k < 3; ++k)
        if (QDoubleSpinBox* d = qobject_cast<QDoubleSpinBox*>(s.spins[k])) d->setValue(rgb[k]);
    m_building = false;
    sendValues(si);
}

void RsMainWindow::onReset()
{
    const int si = sender()->property("setting").toInt();
    RsCoreApi().resetUniform(m_pageTechnique, m_settings[(size_t)si].u);
    readValues(si);
}

void RsMainWindow::onDefinitionEdited()
{
    QLineEdit* edit = qobject_cast<QLineEdit*>(sender());
    if (!edit || m_pageTechnique < 0 || !edit->isModified()) return;
    edit->setModified(false);
    const QByteArray name = edit->property("name").toString().toUtf8();
    const QByteArray value = edit->text().toUtf8();
    RsCoreApi().setDefinition(m_pageTechnique, name.constData(), value.constData());
    m_statusMessage->setText(L("%1 = %2: compiling").arg(U(name.constData())).arg(U(value.constData())));
}

void RsMainWindow::onShowErrors()
{
    std::vector<char> b(1 << 20);
    RsCoreApi().errors(&b[0], (int)b.size());
    QDialog dialog(this);
    dialog.setWindowTitle(L("Compiler messages"));
    dialog.resize(900, 560);
    QVBoxLayout* l = new QVBoxLayout(&dialog);
    QPlainTextEdit* text = new QPlainTextEdit(&dialog);
    text->setReadOnly(true);
    text->setPlainText(b[0] ? U(&b[0]) : L("Every effect compiled without a word."));
    l->addWidget(text);
    dialog.exec();
}

void RsMainWindow::onAbout()
{
    QMessageBox::about(this, L("About ReShade"),
                       L("<b>ReShade</b> 6.8.0 inside S2FM's frame.<br><br>"
                         "A runs after the opaque surfaces, B on every refinement sample after S2FM's post, "
                         "C once on the finished picture. Effects get the scene's own linear depth.<br><br>"
                         "ReShade is by crosire (BSD 3-Clause). Core interface version %1.").arg(RS_CORE_API_VERSION));
}

// --- What the tool system asks of the window (rs_window.h).

void RsWindowShow()
{
    if (!OnQtThread()) { RsToolLog("Show off Qt's thread: ignored"); return; }
    if (!g_window) g_window = new RsMainWindow;
    g_window->show();
    g_window->raise();
    g_window->activateWindow();
}

void RsWindowHide()
{
    if (g_window && OnQtThread()) g_window->hide();
}

void RsWindowFlush()
{
}

void RsWindowShutdown()
{
    if (!g_window || !OnQtThread()) return;
    delete g_window;
    g_window = 0;
}

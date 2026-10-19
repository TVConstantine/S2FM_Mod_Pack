// The ReShade tool's window: a header with the icon, the name,
// the preset (with New) and the effects switch; down the left two cards, one above the other: the preset's
// techniques grouped by the stage they run at (A after the opaque surfaces, B every refinement sample after S2FM's
// post, C on the finished picture), each group with what it costs, and the effects folder to add from, grouped by
// collection (qUINT, SweetFX, ...), the effects already in the preset marked; on the right the selected technique's page:
// where it runs, then its effect's settings in cards by ui_category, a row each (its name and tooltip on the
// left, its control and a reset on the right), then its preprocessor definitions. A status bar with what
// compiled, the picture's size and what a render costs.
//
// Everything goes through the core's C API (rs_bridge.h); an edit reaches the viewport at once. The preset on
// disk is written only by Save and Save As.
#pragma once

#include <QtGui/QMainWindow>
#include <QtGui/QTreeWidget>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <vector>

#include "core/rs_core_api.h"

class QAbstractSpinBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QTimer;
class QToolButton;
class QVBoxLayout;
class QWidget;
class RsSwitch;

// The technique list: dropping a technique on a stage's group, or between techniques, moves it there.
class RsTechniqueTree : public QTreeWidget
{
    Q_OBJECT
public:
    explicit RsTechniqueTree(QWidget* parent);
signals:
    // technique: its number; stage: the group it was dropped in; before: the technique it goes before, or -1.
    void dropped(int technique, int stage, int before);
protected:
    void dropEvent(QDropEvent* event);
};

class RsMainWindow : public QMainWindow
{
    Q_OBJECT
public:
    RsMainWindow();
    // For the live tests: a technique's tick set as a click sets it. False when it is not in the list.
    bool testCheck(int technique, bool on);
    // For the live tests: the RTAO switch set as a click sets it (on -1 leaves it), and the RTAO status text.
    bool testRtao(int on, char* status, int size);
    // For the live tests: a new preset made at that path as New makes it; the window drawn into a PNG.
    bool newPreset(const QString& path);
    bool testGrab(const QString& path);
    // For the live tests: the look set (-1 as decided at the start, 0 purple, 1 Valve's greys), the window restyled.
    int testTheme(int look);
    // For the live tests: the effect list's state, "effects N, collections N, in the preset N, shown N".
    QString testEffects(const QString& search);

protected:
    void closeEvent(QCloseEvent* event);
    bool eventFilter(QObject* watched, QEvent* event);

private slots:
    void onPoll();
    void onPresetChosen(int index);
    void onNewPreset();
    void onOpenPreset();
    void onSave();
    void onSaveAs();
    void onReload();
    void onEffectsOn(bool on);
    void onSelection();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onTreeMenu(const QPoint& at);
    void onDropped(int technique, int stage, int before);
    void onStageCombo(int index);
    void onSearch(const QString& text);
    void onAddEffect(QTreeWidgetItem* item, int column);
    void onAddSelected();
    void onEffectSelection();
    void onSlider(int value);
    void onSpin();
    void onSwitch(bool on);
    void onCombo(int index);
    void onColor();
    void onReset();
    void onDefinitionEdited();
    void onToggleCard();
    void onShowErrors();
    void onAbout();
    void onRebuild();
    void onRtaoOn(bool on);
    void onRtaoRays(int index);

private:
    struct Setting
    {
        int u;
        RsUniformInfo info;
        QSlider* slider;
        std::vector<QAbstractSpinBox*> spins;
        RsSwitch* sw;
        QComboBox* combo;
        QPushButton* color;
    };

    void buildList();
    // The list and/or the page rebuilt once the event loop has its turn (see onItemChanged).
    void rebuildLater(bool list, bool page);
    void buildPage();
    QFrame* card(const QString& title, QWidget* parent, QVBoxLayout** body, bool closed);
    QFrame* row(const QString& name, const QString& tip, QWidget* control, QWidget* parent, QWidget* reset);
    void readValues(int s);
    void sendValues(int s);
    void fillPresets();
    void fillEffects();
    // The effects already in the preset marked in the effect list, and the cards' counts.
    void markAdded();
    int selectedTechnique() const;
    void selectTechnique(const QString& name);
    QString stageTitle(int stage, float ms) const;
    // RTAO's switch, rays and state, when the game's dxgi.dll has RTAO (rtao/rtao_api.h).
    void pollRtao();

    RsTechniqueTree* m_tree;
    QComboBox* m_presets;
    RsSwitch* m_effectsOn;
    QLabel* m_subtitle;
    QLineEdit* m_search;
    QTreeWidget* m_effects;
    QLabel* m_effectsCount;
    QLabel* m_presetCount;
    QPushButton* m_addEffect;
    QScrollArea* m_pageScroll;
    QLabel* m_statusCompiled;
    QLabel* m_statusSize;
    QLabel* m_statusCost;
    QLabel* m_statusMessage;
    QTimer* m_poll;
    QStringList m_presetPaths;
    struct Effect
    {
        QString file, collection;
    };
    std::vector<Effect> m_allEffects;
    QStringList m_inPreset;   // the effect files the preset's techniques come from
    std::vector<Setting> m_settings;
    std::vector<QLineEdit*> m_definitions;
    QComboBox* m_stageCombo;
    QString m_selectedName;
    int m_pageTechnique;
    bool m_building;
    bool m_rebuildQueued, m_rebuildList, m_rebuildPage;
    // What the list was built from, so the poll rebuilds it only when it changed.
    int m_lastTechniques;
    int m_lastLoading;
    QString m_lastPreset;
    float m_stageMs[3];
    RsSwitch* m_rtaoOn;
    QComboBox* m_rtaoRays;
    QLabel* m_statusRtao;
    void* m_rtaoGet;
    void* m_rtaoSet;
};

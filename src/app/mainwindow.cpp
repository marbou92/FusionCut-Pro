#include "mainwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QFont>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QStatusBar>

#include <fc/version.h>

namespace fc {

namespace {

// FusionCut Pro design tokens (Module 2.3 design system).
constexpr unsigned int kWindowBg = 0x1E1E1E; // charcoal
constexpr unsigned int kPanelBg = 0x252525;  // panel background
constexpr unsigned int kButtonBg = 0x2E2E2E;
constexpr unsigned int kText = 0xE8E8E8;
constexpr unsigned int kAccent = 0x00A8FF; // FusionCut accent

// Adds a menu action with an optional shortcut. Written explicitly because
// the QWidget::addAction(text, shortcut, receiver, functor) overload only
// exists from Qt 6.3; Qt 5.15 (our floor) has no such convenience overload.
QAction *addMenuAction(QMenu *menu, const QString &text,
                       const QKeySequence &shortcut = QKeySequence()) {
    QAction *action = menu->addAction(text);
    if (!shortcut.isEmpty()) {
        action->setShortcut(shortcut);
    }
    return action;
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(tr("FusionCut Pro"));
    resize(1280, 720);
    applyDarkTheme();
    buildMenus();
    buildCentralPlaceholder();
    buildStatusBar();
}

void MainWindow::applyDarkTheme() {
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(kWindowBg));
    pal.setColor(QPalette::WindowText, QColor(kText));
    pal.setColor(QPalette::Base, QColor(kPanelBg));
    pal.setColor(QPalette::AlternateBase, QColor(kWindowBg));
    pal.setColor(QPalette::Text, QColor(kText));
    pal.setColor(QPalette::Button, QColor(kButtonBg));
    pal.setColor(QPalette::ButtonText, QColor(kText));
    pal.setColor(QPalette::ToolTipBase, QColor(kPanelBg));
    pal.setColor(QPalette::ToolTipText, QColor(kText));
    pal.setColor(QPalette::Highlight, QColor(kAccent));
    pal.setColor(QPalette::HighlightedText, QColor(0x101010));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(0x777777));
    setPalette(pal);
}

void MainWindow::buildMenus() {
    // ---- File ----
    QMenu *file = menuBar()->addMenu(tr("&File"));
    addMenuAction(file, tr("&New Project..."), QKeySequence::New);
    addMenuAction(file, tr("&Open Project..."), QKeySequence::Open);
    addMenuAction(file, tr("&Save Project"), QKeySequence::Save);
    file->addSeparator();
    addMenuAction(file, tr("&Import Media..."), QKeySequence(tr("Ctrl+I")));
    addMenuAction(file, tr("Import &Image Sequence..."));
    file->addSeparator();
    addMenuAction(file, tr("&Export Media..."), QKeySequence(tr("Ctrl+M")));
    file->addSeparator();
    QAction *quitAction = addMenuAction(file, tr("E&xit"), QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    // ---- Edit ----
    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    addMenuAction(edit, tr("&Undo"), QKeySequence::Undo);
    addMenuAction(edit, tr("&Redo"), QKeySequence::Redo);
    edit->addSeparator();
    addMenuAction(edit, tr("Cu&t"), QKeySequence::Cut);
    addMenuAction(edit, tr("&Copy"), QKeySequence::Copy);
    addMenuAction(edit, tr("&Paste"), QKeySequence::Paste);
    edit->addSeparator();
    addMenuAction(edit, tr("&Keyboard Shortcuts..."));

    // ---- Clip ----
    QMenu *clip = menuBar()->addMenu(tr("&Clip"));
    addMenuAction(clip, tr("Split at Playhead"), QKeySequence(tr("C")));
    addMenuAction(clip, tr("&Speed / Duration..."), QKeySequence(tr("Ctrl+R")));
    addMenuAction(clip, tr("&Reverse Clip"));
    addMenuAction(clip, tr("&Freeze Frame..."));
    clip->addSeparator();
    addMenuAction(clip, tr("Link / Unlink &Audio-Video"), QKeySequence(tr("Ctrl+L")));
    addMenuAction(clip, tr("&Group Clips"), QKeySequence(tr("Ctrl+G")));
    addMenuAction(clip, tr("U&ngroup Clips"), QKeySequence(tr("Ctrl+Shift+G")));

    // ---- Sequence ----
    QMenu *sequence = menuBar()->addMenu(tr("Se&quence"));
    addMenuAction(sequence, tr("Add &Video Track"));
    addMenuAction(sequence, tr("Add &Audio Track"));
    sequence->addSeparator();
    addMenuAction(sequence, tr("&Render In to Out"), QKeySequence(tr("Enter")));

    // ---- Effects ----
    QMenu *effects = menuBar()->addMenu(tr("&Effects"));
    addMenuAction(effects, tr("Apply &Default Transition"), QKeySequence(tr("Ctrl+D")));
    addMenuAction(effects, tr("&Remove All Effects from Clip"));

    // ---- View ----
    QMenu *view = menuBar()->addMenu(tr("&View"));
    addMenuAction(view, tr("Zoom &In"), QKeySequence(tr("+")));
    addMenuAction(view, tr("Zoom &Out"), QKeySequence(tr("-")));
    addMenuAction(view, tr("&Fit Timeline"));
    view->addSeparator();
    addMenuAction(view, tr("Toggle &Safe Margins"));

    // ---- Window (dual-mode workspace switcher) ----
    QMenu *window = menuBar()->addMenu(tr("&Window"));
    QActionGroup *modes = new QActionGroup(this);
    QAction *proMode = modes->addAction(tr("&Pro Mode"));
    QAction *quickMode = modes->addAction(tr("&Quick Mode"));
    proMode->setCheckable(true);
    quickMode->setCheckable(true);
    proMode->setChecked(true);
    connect(proMode, &QAction::triggered, this, [this] {
        statusBar()->showMessage(tr("Workspace: Pro Mode (Premiere-style panel layout)"));
    });
    connect(quickMode, &QAction::triggered, this, [this] {
        statusBar()->showMessage(tr("Workspace: Quick Mode (CapCut-style streamlined layout)"));
    });
    window->addAction(proMode);
    window->addAction(quickMode);

    // ---- Help ----
    QMenu *help = menuBar()->addMenu(tr("&Help"));
    QAction *aboutAction = help->addAction(tr("&About FusionCut Pro"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About FusionCut Pro"),
                           tr("<b>FusionCut Pro %1</b><br/>Pre-alpha engineering shell."
                              "<br/><br/>Core engine: timecode, LRU frame cache, memory pool."
                              "<br/>License: GPL-3.0-or-later"
                              "<br/><a href=\"https://github.com/marbou92/FusionCut-Pro\">"
                              "github.com/marbou92/FusionCut-Pro</a>")
                               .arg(FC_VERSION_STRING));
    });
}

void MainWindow::buildCentralPlaceholder() {
    auto *placeholder = new QLabel(this);
    placeholder->setAlignment(Qt::AlignCenter);
    QFont font = placeholder->font();
    font.setPointSize(font.pointSize() + 1);
    placeholder->setFont(font);
    placeholder->setText(tr("FusionCut Pro v%1 - Pre-Alpha Shell"
                            "\n\n"
                            "Shipped in this build: build system, CI, portable pipeline,\n"
                            "core engine primitives (timecode, LRU frame cache, memory pool)."
                            "\n\n"
                            "The dual-mode workspace (Pro Mode / Quick Mode) docks in Milestone 3.")
                             .arg(FC_VERSION_STRING));
    setCentralWidget(placeholder);
}

void MainWindow::buildStatusBar() {
    statusBar()->showMessage(tr("Workspace: Pro Mode"));
    auto *budget = new QLabel(tr("1 GB RAM target - Qt 5.15 - C++17 - Windows 7+"), this);
    statusBar()->addPermanentWidget(budget);
}

} // namespace fc

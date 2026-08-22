#include "mainwindow.h"

#include <QActionGroup>
#include <QApplication>
#include <QFont>
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
    file->addAction(tr("&New Project..."), QKeySequence::New, this, [] {});
    file->addAction(tr("&Open Project..."), QKeySequence::Open, this, [] {});
    file->addAction(tr("&Save Project"), QKeySequence::Save, this, [] {});
    file->addSeparator();
    file->addAction(tr("&Import Media..."), QKeySequence(tr("Ctrl+I")), this, [] {});
    file->addAction(tr("Import &Image Sequence..."), this, [] {});
    file->addSeparator();
    file->addAction(tr("&Export Media..."), QKeySequence(tr("Ctrl+M")), this, [] {});
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, qApp, &QApplication::quit);

    // ---- Edit ----
    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("&Undo"), QKeySequence::Undo, this, [] {});
    edit->addAction(tr("&Redo"), QKeySequence::Redo, this, [] {});
    edit->addSeparator();
    edit->addAction(tr("Cu&t"), QKeySequence::Cut, this, [] {});
    edit->addAction(tr("&Copy"), QKeySequence::Copy, this, [] {});
    edit->addAction(tr("&Paste"), QKeySequence::Paste, this, [] {});
    edit->addSeparator();
    edit->addAction(tr("&Keyboard Shortcuts..."), this, [] {});

    // ---- Clip ----
    QMenu *clip = menuBar()->addMenu(tr("&Clip"));
    clip->addAction(tr("Split at Playhead\tC"), this, [] {});
    clip->addAction(tr("&Speed / Duration..."), this, [] {});
    clip->addAction(tr("&Reverse Clip"), this, [] {});
    clip->addAction(tr("&Freeze Frame..."), this, [] {});
    clip->addSeparator();
    clip->addAction(tr("Link / Unlink &Audio-Video"), this, [] {});
    clip->addAction(tr("&Group Clips"), this, [] {});
    clip->addAction(tr("U&ngroup Clips"), this, [] {});

    // ---- Sequence ----
    QMenu *sequence = menuBar()->addMenu(tr("Se&quence"));
    sequence->addAction(tr("Add &Video Track"), this, [] {});
    sequence->addAction(tr("Add &Audio Track"), this, [] {});
    sequence->addSeparator();
    sequence->addAction(tr("&Render In to Out"), this, [] {});

    // ---- Effects ----
    QMenu *effects = menuBar()->addMenu(tr("&Effects"));
    effects->addAction(tr("Apply &Default Transition\tCtrl+D"), this, [] {});
    effects->addAction(tr("&Remove All Effects from Clip"), this, [] {});

    // ---- View ----
    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Zoom &In\t+"), this, [] {});
    view->addAction(tr("Zoom &Out\t-"), this, [] {});
    view->addAction(tr("&Fit Timeline"), this, [] {});
    view->addSeparator();
    view->addAction(tr("Toggle &Safe Margins"), this, [] {});

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
    help->addAction(tr("&About FusionCut Pro"), this, [this] {
        QMessageBox::about(
            this, tr("About FusionCut Pro"),
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
    placeholder->setText(
        tr("FusionCut Pro v%1 - Pre-Alpha Shell"
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

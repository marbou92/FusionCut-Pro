#include "quick_mode_view.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace {
const char *kAccentStyle = "QPushButton { background: #00A8FF; color: #101010; font-weight: bold; "
                           "border-radius: 4px; padding: 6px 18px; }"
                           "QPushButton:hover { background: #33B9FF; }";

struct ToolGroup {
    const char *const *labels;
    int count;
    const char *tooltip;
};

QPushButton *flatToolButton(const QString &text, const QString &tooltip, const char *color,
                            QWidget *parent) {
    auto *button = new QPushButton(text, parent);
    button->setFlat(true);
    button->setToolTip(tooltip);
    button->setStyleSheet(QString("QPushButton { color: %1; padding: 6px 10px; }"
                                  "QPushButton:hover { color: #00A8FF; }")
                              .arg(color));
    return button;
}
} // namespace

QuickModeView::QuickModeView(QWidget *parent) : QWidget(parent) {
    canvas_ = new PreviewCanvas(this);
    playButton_ = new QPushButton(tr("Play"), this);
    playButton_->setMinimumWidth(90);
    position_ = new QSlider(Qt::Horizontal, this);
    position_->setRange(0, 1000);
    position_->setEnabled(false); // full scrubbing ships with the M4 timeline

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(10);
    root->addWidget(buildTopBar());
    root->addWidget(canvas_, 1);

    auto *transport = new QHBoxLayout();
    transport->addWidget(playButton_);
    transport->addWidget(position_, 1);
    root->addLayout(transport);
    root->addWidget(buildToolbar());

    connect(playButton_, &QPushButton::clicked, this,
            [this] { emit playToggled(playButton_->text() == tr("Play")); });
}

QWidget *QuickModeView::buildTopBar() {
    auto *bar = new QWidget(this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *import = new QPushButton(tr("Import"), bar);
    import->setStyleSheet(kAccentStyle);

    aspectBox_ = new QComboBox(bar);
    aspectBox_->addItem(tr("16:9"));
    aspectBox_->addItem(tr("9:16"));
    aspectBox_->addItem(tr("1:1"));
    aspectBox_->addItem(tr("4:3"));
    aspectBox_->addItem(tr("Custom"));
    connect(aspectBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                const double aspects[] = {16.0 / 9.0, 9.0 / 16.0, 1.0, 4.0 / 3.0, 0.0};
                if (aspects[index] > 0.0) {
                    canvas_->setAspectHint(aspects[index]);
                }
            });

    layout->addWidget(import);
    layout->addStretch(1);
    layout->addWidget(new QLabel(tr("Aspect:"), bar));
    layout->addWidget(aspectBox_);
    return bar;
}

QWidget *QuickModeView::buildToolbar() {
    auto *bar = new QWidget(this);
    bar->setStyleSheet("QWidget { background: #252525; border-radius: 6px; }");
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(8);

    // Module 2.2 main toolbar (shown when no clip is selected).
    static const char *kTools[] = {"Add Media", "Text",        "Stickers",
                                   "Effects",   "Transitions", "Filters"};
    for (const char *tool : kTools) {
        layout->addWidget(
            flatToolButton(tr(tool), tr("Coming in milestones M4-M6"), "#E8E8E8", bar));
    }
    layout->addStretch(1);

    // Quick Actions (Module 2.2): AI one-click features.
    static const char *kActions[] = {"Auto-Captions", "Auto-Enhance", "Smart Crop", "Templates"};
    for (const char *action : kActions) {
        layout->addWidget(flatToolButton(tr(action), tr("Coming in milestone M7"), "#9BB8C9", bar));
    }
    return bar;
}

void QuickModeView::setPlaying(bool playing) {
    playButton_->setText(playing ? tr("Pause") : tr("Play"));
}

#include "effects_panel.h"

#include <QMap>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

struct EffectEntry {
    const char *category;
    const char *name;
};

// Seed list from the Module 7 spec (full 50+ effects ship with M5).
const EffectEntry kEffects[] = {
    {"Video Effects", "Gaussian Blur"},
    {"Video Effects", "Directional Blur"},
    {"Video Effects", "Radial Blur"},
    {"Video Effects", "Sharpen"},
    {"Video Effects", "Noise Reduction"},
    {"Video Effects", "Vignette"},
    {"Video Effects", "Film Grain"},
    {"Video Effects", "Chromatic Aberration"},
    {"Video Effects", "Lens Distortion Correction"},
    {"Video Effects", "Pixelate / Mosaic"},
    {"Video Effects", "Mirror"},
    {"Video Effects", "Wave"},
    {"Video Effects", "Glow"},
    {"Video Effects", "Drop Shadow"},
    {"Video Effects", "Bevel Edges"},
    {"Style Effects", "VHS"},
    {"Style Effects", "Glitch"},
    {"Style Effects", "RGB Split"},
    {"Style Effects", "Scanlines (CRT)"},
    {"Style Effects", "Halftone"},
    {"Style Effects", "Posterize"},
    {"Style Effects", "Threshold"},
    {"Style Effects", "Edge Detect"},
    {"Style Effects", "Emboss"},
    {"Style Effects", "Oil Paint"},
    {"Time Effects", "Time Warp"},
    {"Time Effects", "Echo / Trail"},
    {"Time Effects", "Frame Blending"},
    {"Time Effects", "Optical Flow"},
    {"Generators", "Gradient"},
    {"Generators", "Solid Color"},
    {"Generators", "Color Bars"},
    {"Generators", "Countdown Timer"},
    {"Generators", "Timecode Generator"},
    {"Audio Effects", "3-Band EQ"},
    {"Audio Effects", "10-Band EQ"},
    {"Audio Effects", "Compressor"},
    {"Audio Effects", "Reverb"},
    {"Audio Effects", "Echo"},
    {"Audio Effects", "Denoise"},
    {"Audio Effects", "Normalize"},
    {"Transitions", "Cross Dissolve"},
    {"Transitions", "Dip to Black"},
    {"Transitions", "Dip to White"},
    {"Transitions", "Push"},
    {"Transitions", "Slide"},
    {"Transitions", "Split"},
    {"Transitions", "Clock Wipe"},
    {"Transitions", "Radial Wipe"},
    {"Transitions", "Checkerboard Wipe"},
    {"Transitions", "Zoom"},
    {"Transitions", "Glitch"},
    {"Transitions", "Cube Rotate"},
    {"Transitions", "Flip"},
    {"Transitions", "Blur"},
    {"Presets", "Cinematic Teal & Orange"},
    {"Presets", "Vintage 1970s"},
    {"Presets", "Vintage 1980s"},
    {"Presets", "Sepia"},
    {"Presets", "Black & White"},
    {"Presets", "HDR Simulation"},
    {"Presets", "Noir"},
    {"Presets", "Cyberpunk"},
};

} // namespace

EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent) {
    auto *tree = new QTreeWidget(this);
    tree->setHeaderLabel(tr("Effects & Presets"));
    tree->setDragDropMode(QAbstractItemView::DragOnly);
    tree->setColumnCount(1);

    QMap<QString, QTreeWidgetItem *> categories;
    for (const EffectEntry &entry : kEffects) {
        const QString category = QString::fromUtf8(entry.category);
        if (!categories.contains(category)) {
            categories[category] = new QTreeWidgetItem(tree, QStringList(category));
            categories[category]->setFlags(Qt::ItemIsEnabled);
        }
        auto *leaf =
            new QTreeWidgetItem(categories[category], QStringList(QString::fromUtf8(entry.name)));
        leaf->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
        leaf->setToolTip(0, tr("Drag onto a timeline clip (timeline editing ships in M4)"));
    }
    tree->expandAll();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(tree);
}

#include "text_panel.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextObject>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kDefaultSize = 64;

QColor toQColor(uint32_t rgba) {
    return QColor(fc::textRed(rgba), fc::textGreen(rgba), fc::textBlue(rgba), fc::textAlpha(rgba));
}

uint32_t fromQColor(const QColor &c) {
    if (!c.isValid()) {
        return 0xFFFFFFFFu;
    }
    return fc::textRgba(static_cast<uint8_t>(c.red()), static_cast<uint8_t>(c.green()),
                        static_cast<uint8_t>(c.blue()), static_cast<uint8_t>(c.alpha()));
}

Qt::Alignment toQtAlignment(fc::TextAlign align) {
    switch (align) {
    case fc::TextAlign::Left:
        return Qt::AlignLeft;
    case fc::TextAlign::Center:
        return Qt::AlignHCenter;
    case fc::TextAlign::Right:
        return Qt::AlignRight;
    }
    return Qt::AlignLeft;
}

fc::TextAlign fromQtAlignment(Qt::Alignment a) {
    if (a & Qt::AlignHCenter) {
        return fc::TextAlign::Center;
    }
    if (a & Qt::AlignRight) {
        return fc::TextAlign::Right;
    }
    return fc::TextAlign::Left;
}

// Applies a character format the word-processor way: QWidgetTextControl
// merges into the cursor - a SELECTION takes the format directly, no
// selection updates the typing format (QTextEdit::mergeCurrentCharFormat
// delegates to exactly that).
void mergeFormatOn(QTextEdit *editor, const QTextCharFormat &fmt) {
    editor->mergeCurrentCharFormat(fmt);
}

// Qt 5.15's QTextCharFormat has no setFontPixelSize (Qt 6 API); the
// FontPixelSize TEXT PROPERTY is the 5.15 mechanism.
void setFormatPixelSize(QTextCharFormat &fmt, int pixels) {
    fmt.setProperty(QTextFormat::FontPixelSize, pixels);
}

int formatPixelSize(const QTextCharFormat &fmt, int fallback) {
    const QVariant v = fmt.property(QTextFormat::FontPixelSize);
    const int pixels = v.isValid() ? v.toInt() : 0;
    return pixels > 0 ? pixels : fallback;
}

} // namespace

TextPanel::TextPanel(QWidget *parent) : QWidget(parent) {
    buildUi();
    setClip(-1, nullptr);
}

void TextPanel::buildUi() {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    info_ = new QLabel(this);
    info_->setWordWrap(true);
    layout->addWidget(info_);

    addButton_ = new QPushButton(tr("Add Text Clip"), this);
    addButton_->setToolTip(
        tr("Creates a 4 s text clip at the playhead on a new text track (Ctrl+T)"));
    connect(addButton_, &QPushButton::clicked, this, [this] { emit addTextClipRequested(); });
    layout->addWidget(addButton_);

    editor_ = new QTextEdit(this);
    editor_->setAcceptRichText(true);
    editor_->setMinimumHeight(120);
    editor_->setPlaceholderText(tr("Type the title text..."));
    connect(editor_, &QTextEdit::textChanged, this, [this] {
        if (!loading_) {
            pushDoc();
        }
    });
    layout->addWidget(editor_, 1);

    // ---- Style row: family / size / B / I / U / color ----
    auto *styleRow = new QWidget(this);
    auto *styleLayout = new QHBoxLayout(styleRow);
    styleLayout->setContentsMargins(0, 0, 0, 0);
    styleLayout->setSpacing(4);

    family_ = new QFontComboBox(styleRow);
    family_->setEditable(false);
    family_->setToolTip(tr("Font family of the selection"));
    family_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(family_, &QFontComboBox::currentFontChanged, this, [this](const QFont &font) {
        if (loading_) {
            return;
        }
        QTextCharFormat fmt;
        fmt.setFontFamily(font.family());
        mergeFormatOn(editor_, fmt);
        pushDoc();
    });
    styleLayout->addWidget(family_, 1);

    size_ = new QSpinBox(styleRow);
    size_->setRange(fc::kTextMinSize, fc::kTextMaxSize);
    size_->setValue(kDefaultSize);
    size_->setToolTip(tr("Pixel size of the selection"));
    connect(size_, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this,
            [this](int value) {
                if (loading_) {
                    return;
                }
                QTextCharFormat fmt;
                setFormatPixelSize(fmt, value);
                mergeFormatOn(editor_, fmt);
                pushDoc();
            });
    styleLayout->addWidget(size_);

    bold_ = new QToolButton(styleRow);
    bold_->setText(tr("B"));
    bold_->setToolTip(tr("Bold (selection)"));
    bold_->setCheckable(true);
    QFont boldFont = bold_->font();
    boldFont.setBold(true);
    bold_->setFont(boldFont);
    connect(bold_, &QToolButton::toggled, this, [this](bool on) {
        if (loading_) {
            return;
        }
        QTextCharFormat fmt;
        fmt.setFontWeight(on ? QFont::Bold : QFont::Normal);
        mergeFormatOn(editor_, fmt);
        pushDoc();
    });
    styleLayout->addWidget(bold_);

    italic_ = new QToolButton(styleRow);
    italic_->setText(tr("I"));
    italic_->setToolTip(tr("Italic (selection)"));
    italic_->setCheckable(true);
    QFont italicFont = italic_->font();
    italicFont.setItalic(true);
    italic_->setFont(italicFont);
    connect(italic_, &QToolButton::toggled, this, [this](bool on) {
        if (loading_) {
            return;
        }
        QTextCharFormat fmt;
        fmt.setFontItalic(on);
        mergeFormatOn(editor_, fmt);
        pushDoc();
    });
    styleLayout->addWidget(italic_);

    underline_ = new QToolButton(styleRow);
    underline_->setText(tr("U"));
    underline_->setToolTip(tr("Underline (selection)"));
    underline_->setCheckable(true);
    QFont underlineFont = underline_->font();
    underlineFont.setUnderline(true);
    underline_->setFont(underlineFont);
    connect(underline_, &QToolButton::toggled, this, [this](bool on) {
        if (loading_) {
            return;
        }
        QTextCharFormat fmt;
        fmt.setFontUnderline(on);
        mergeFormatOn(editor_, fmt);
        pushDoc();
    });
    styleLayout->addWidget(underline_);

    color_ = new QPushButton(tr("A"), styleRow);
    color_->setToolTip(tr("Text color (selection)"));
    connect(color_, &QPushButton::clicked, this, [this] {
        if (loading_) {
            return;
        }
        const QColor initial =
            toQColor(doc_.runs.empty() ? 0xFFFFFFFFu : doc_.runs.front().style.colorRgba);
        const QColor picked = QColorDialog::getColor(initial, this, tr("Text Color"));
        if (!picked.isValid()) {
            return;
        }
        QTextCharFormat fmt;
        fmt.setForeground(picked);
        mergeFormatOn(editor_, fmt);
        pushDoc();
    });
    styleLayout->addWidget(color_);

    layout->addWidget(styleRow);

    // ---- Alignment row ----
    auto *alignRow = new QWidget(this);
    auto *alignLayout = new QHBoxLayout(alignRow);
    alignLayout->setContentsMargins(0, 0, 0, 0);
    alignLayout->setSpacing(4);

    auto *alignLabel = new QLabel(tr("Align:"), alignRow);
    alignLayout->addWidget(alignLabel);

    alignLeft_ = new QToolButton(alignRow);
    alignLeft_->setText(tr("Left"));
    alignLeft_->setCheckable(true);
    connect(alignLeft_, &QToolButton::clicked, this, [this] {
        doc_.align = fc::TextAlign::Left;
        applyAlign();
        pushDoc();
    });
    alignLayout->addWidget(alignLeft_);

    alignCenter_ = new QToolButton(alignRow);
    alignCenter_->setText(tr("Center"));
    alignCenter_->setCheckable(true);
    connect(alignCenter_, &QToolButton::clicked, this, [this] {
        doc_.align = fc::TextAlign::Center;
        applyAlign();
        pushDoc();
    });
    alignLayout->addWidget(alignCenter_);

    alignRight_ = new QToolButton(alignRow);
    alignRight_->setText(tr("Right"));
    alignRight_->setCheckable(true);
    connect(alignRight_, &QToolButton::clicked, this, [this] {
        doc_.align = fc::TextAlign::Right;
        applyAlign();
        pushDoc();
    });
    alignLayout->addWidget(alignRight_);

    alignLayout->addStretch(1);
    layout->addWidget(alignRow);

    // ---- Position row: anchor X/Y + wrap ----
    auto *posRow = new QWidget(this);
    auto *posForm = new QHBoxLayout(posRow);
    posForm->setContentsMargins(0, 0, 0, 0);
    posForm->setSpacing(4);

    auto *xLabel = new QLabel(tr("X:"), posRow);
    posForm->addWidget(xLabel);
    anchorX_ = new QDoubleSpinBox(posRow);
    anchorX_->setRange(0.0, 1.0);
    anchorX_->setSingleStep(0.05);
    anchorX_->setValue(0.5);
    anchorX_->setToolTip(tr("Horizontal center of the text block (fraction of the frame width)"));
    connect(anchorX_, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
            this, [this](double) {
                if (!loading_) {
                    pushDoc();
                }
            });
    posForm->addWidget(anchorX_);

    auto *yLabel = new QLabel(tr("Y:"), posRow);
    posForm->addWidget(yLabel);
    anchorY_ = new QDoubleSpinBox(posRow);
    anchorY_->setRange(0.0, 1.0);
    anchorY_->setSingleStep(0.05);
    anchorY_->setValue(0.5);
    anchorY_->setToolTip(tr("Vertical center of the text block (fraction of the frame height)"));
    connect(anchorY_, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
            this, [this](double) {
                if (!loading_) {
                    pushDoc();
                }
            });
    posForm->addWidget(anchorY_);

    auto *wrapLabel = new QLabel(tr("Width:"), posRow);
    posForm->addWidget(wrapLabel);
    wrap_ = new QDoubleSpinBox(posRow);
    wrap_->setRange(0.05, 1.0);
    wrap_->setSingleStep(0.05);
    wrap_->setValue(0.8);
    wrap_->setToolTip(tr("Maximum line width before wrapping (fraction of the frame width)"));
    connect(wrap_, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
            this, [this](double) {
                if (!loading_) {
                    pushDoc();
                }
            });
    posForm->addWidget(wrap_);

    posForm->addStretch(1);
    layout->addWidget(posRow);

    // ---- Background row ----
    auto *bgRow = new QWidget(this);
    auto *bgLayout = new QHBoxLayout(bgRow);
    bgLayout->setContentsMargins(0, 0, 0, 0);
    bgLayout->setSpacing(4);

    background_ = new QCheckBox(tr("Background"), bgRow);
    connect(background_, &QCheckBox::toggled, this, [this](bool) {
        if (!loading_) {
            pushDoc();
        }
    });
    bgLayout->addWidget(background_);

    bgColor_ = new QPushButton(tr("Color..."), bgRow);
    connect(bgColor_, &QPushButton::clicked, this, [this] {
        if (loading_) {
            return;
        }
        const QColor initial = toQColor(doc_.box.backgroundRgba);
        const QColor picked = QColorDialog::getColor(initial, this, tr("Background Color"));
        if (!picked.isValid()) {
            return;
        }
        doc_.box.backgroundRgba = fromQColor(picked);
        pushDoc();
    });
    bgLayout->addWidget(bgColor_);
    bgLayout->addStretch(1);
    layout->addWidget(bgRow);

    editor_->setTabChangesFocus(true);
}

void TextPanel::applyAlign() {
    QTextCursor cursor = editor_->textCursor();
    cursor.select(QTextCursor::Document);
    QTextBlockFormat blockFormat;
    blockFormat.setAlignment(toQtAlignment(doc_.align));
    cursor.mergeBlockFormat(blockFormat);
}

fc::TextDocument TextPanel::documentFromEditor() const {
    fc::TextDocument doc = doc_; // box + align as loaded/edited via the spins
    doc.align = doc_.align;
    doc.box.anchorX = anchorX_->value();
    doc.box.anchorY = anchorY_->value();
    doc.box.wrap = wrap_->value();
    doc.box.background = background_->isChecked();
    // doc.box.backgroundRgba lives in doc_ (set via the color dialog).

    doc.runs.clear();
    const QTextDocument *tdoc = editor_->document();
    for (QTextBlock block = tdoc->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.text().isEmpty()) {
                continue;
            }
            const QTextCharFormat fmt = fragment.charFormat();
            fc::TextRun run;
            run.text = fragment.text().toUtf8().toStdString();
            run.style.family = fmt.fontFamily().toStdString();
            run.style.size = formatPixelSize(fmt, kDefaultSize);
            run.style.bold = fmt.fontWeight() > QFont::Normal;
            run.style.italic = fmt.fontItalic();
            run.style.underline = fmt.fontUnderline();
            run.style.colorRgba = fromQColor(fmt.foreground().color());
            doc.runs.push_back(std::move(run));
        }
        if (block.next().isValid()) {
            // Paragraph separator: a hard newline inside the last run (or
            // a tiny newline-only run when the block had no fragments).
            if (!doc.runs.empty()) {
                doc.runs.back().text += "\n";
            } else {
                fc::TextRun sep;
                sep.text = "\n";
                doc.runs.push_back(std::move(sep));
            }
        }
    }
    return doc;
}

void TextPanel::loadIntoEditor(const fc::TextDocument &doc) {
    loading_ = true;

    editor_->clear();
    doc_ = doc;

    // Default font of the editor matches the first run (or the panel
    // default) so typing with no explicit format looks right.
    fc::TextStyle base;
    if (!doc.runs.empty()) {
        base = doc.runs.front().style;
    } else {
        base.size = kDefaultSize;
    }
    QFont baseFont;
    if (!base.family.empty()) {
        baseFont.setFamily(QString::fromStdString(base.family));
    }
    baseFont.setPixelSize(base.size);
    baseFont.setBold(base.bold);
    baseFont.setItalic(base.italic);
    baseFont.setUnderline(base.underline);
    editor_->setFont(baseFont);

    QTextCursor cursor(editor_->document());
    cursor.movePosition(QTextCursor::Start);
    QTextCharFormat baseFormat;
    baseFormat.setFontFamily(baseFont.family());
    setFormatPixelSize(baseFormat, base.size);
    baseFormat.setFontWeight(base.bold ? QFont::Bold : QFont::Normal);
    baseFormat.setFontItalic(base.italic);
    baseFormat.setFontUnderline(base.underline);
    baseFormat.setForeground(toQColor(base.colorRgba));
    cursor.setCharFormat(baseFormat);
    editor_->setCurrentCharFormat(baseFormat);

    for (const fc::TextRun &run : doc.runs) {
        QTextCharFormat fmt;
        if (!run.style.family.empty()) {
            fmt.setFontFamily(QString::fromStdString(run.style.family));
        }
        setFormatPixelSize(fmt, run.style.size);
        fmt.setFontWeight(run.style.bold ? QFont::Bold : QFont::Normal);
        fmt.setFontItalic(run.style.italic);
        fmt.setFontUnderline(run.style.underline);
        fmt.setForeground(toQColor(run.style.colorRgba));
        cursor.insertText(QString::fromStdString(run.text), fmt);
    }

    editor_->document()->clearUndoRedoStacks();
    applyAlign();
    anchorX_->setValue(doc.box.anchorX);
    anchorY_->setValue(doc.box.anchorY);
    wrap_->setValue(doc.box.wrap);
    background_->setChecked(doc.box.background);
    size_->setValue(base.size);
    family_->setCurrentFont(baseFont);
    bold_->setChecked(base.bold);
    italic_->setChecked(base.italic);
    underline_->setChecked(base.underline);
    alignLeft_->setChecked(doc.align == fc::TextAlign::Left);
    alignCenter_->setChecked(doc.align == fc::TextAlign::Center);
    alignRight_->setChecked(doc.align == fc::TextAlign::Right);

    loading_ = false;
}

void TextPanel::pushDoc() {
    if (clipId_ <= 0) {
        return;
    }
    doc_ = documentFromEditor();
    fc::normalizeTextDocument(doc_);
    emit textEdited(clipId_, doc_);
}

void TextPanel::refreshInfo() {
    if (clipId_ <= 0) {
        info_->setText(tr("No text clip selected - add one or click a T clip in the timeline."));
        addButton_->setVisible(true);
        editor_->setEnabled(false);
        family_->setEnabled(false);
        size_->setEnabled(false);
        bold_->setEnabled(false);
        italic_->setEnabled(false);
        underline_->setEnabled(false);
        color_->setEnabled(false);
        alignLeft_->setEnabled(false);
        alignCenter_->setEnabled(false);
        alignRight_->setEnabled(false);
        anchorX_->setEnabled(false);
        anchorY_->setEnabled(false);
        wrap_->setEnabled(false);
        background_->setEnabled(false);
        bgColor_->setEnabled(false);
        return;
    }
    info_->setText(tr("Editing text clip %1").arg(clipId_));
    addButton_->setVisible(true);
    editor_->setEnabled(true);
    family_->setEnabled(true);
    size_->setEnabled(true);
    bold_->setEnabled(true);
    italic_->setEnabled(true);
    underline_->setEnabled(true);
    color_->setEnabled(true);
    alignLeft_->setEnabled(true);
    alignCenter_->setEnabled(true);
    alignRight_->setEnabled(true);
    anchorX_->setEnabled(true);
    anchorY_->setEnabled(true);
    wrap_->setEnabled(true);
    background_->setEnabled(true);
    bgColor_->setEnabled(true);
}

void TextPanel::setClip(int64_t clipId, const fc::TextDocument *doc) {
    clipId_ = doc ? clipId : -1;
    if (clipId_ > 0 && doc) {
        loadIntoEditor(*doc);
    } else {
        loading_ = true;
        editor_->clear();
        doc_ = fc::TextDocument();
        loading_ = false;
    }
    refreshInfo();
}

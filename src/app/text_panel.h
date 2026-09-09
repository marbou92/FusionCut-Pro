#pragma once

#include <QWidget>

#include <QVector>

#include "system_fonts.h"
#include "text.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTextEdit;
class QToolButton;

// Pro Mode left panel : the rich-text editor for the
// SELECTED text clip. The content area is a QTextEdit (Qt's fragment
// model maps 1:1 onto fc::TextRun), so style controls apply to the
// SELECTION exactly like a word processor: family / size / bold /
// italic / underline / color merge into the selected characters' formats
// (with no selection, they set the format text typed next gets).
//
// Below the editor sit the box controls (alignment, anchor X/Y, wrap
// width, background + color) that place the block in the frame, and
// the ANIMATION controls (entrance/exit kind + duration in frames,
// direction) that play the clip in and out.
//
// At the bottom sits the EMOJI FONT picker - a machine-wide preference
// (not part of any document): the combo lists the emoji-capable fonts
// discovered on this PC (Segoe UI Emoji, Noto Color Emoji, Apple Color
// Emoji, ...) plus "System default"; picking one renders every text
// clip's emoji from THAT font's color bitmaps, so switching fonts
// switches the emoji artwork (Microsoft, Apple, Google...). Emitted as
// emojiFontPicked; MainWindow loads the font and re-renders.
//
// Every edit emits the clip's FULL new document (textEdited) -
// MainWindow writes it into the model and re-renders the program
// monitor instantly from the cached video frame. The emitter keeps its
// working copy (MainWindow only pushes back on selection changes),
// mirroring the Effect Controls / Color panel protocol.
class TextPanel : public QWidget {
    Q_OBJECT

public:
    explicit TextPanel(QWidget *parent = nullptr);

    // Shows the text document of the clip with this id (clipId < 0 or a
    // null/non-text clip clears the panel).
    void setClip(int64_t clipId, const fc::TextDocument *doc);

    // Populates the emoji-font combo (list of discovered fonts + a
    // leading "System default" entry) and selects `currentPath`
    // ("" = the System default row).
    void setEmojiFonts(const QVector<fc::SystemEmojiFont> &fonts, const QString &currentPath);

signals:
    // The Add button (no text clip selected yet).
    void addTextClipRequested();
    // The edited document for clipId (content, styles, box).
    void textEdited(int64_t clipId, const fc::TextDocument &doc);
    // The user picked an emoji font ("" = system default / none).
    void emojiFontPicked(const QString &path);

private:
    void buildUi();
    // QTextEdit + box controls -> fc::TextDocument (no emission).
    fc::TextDocument documentFromEditor() const;
    // doc -> QTextEdit + box controls (signals suppressed while loading).
    void loadIntoEditor(const fc::TextDocument &doc);
    void pushDoc();     // documentFromEditor + emit textEdited
    void applyAlign();  // write align_ into every block's format
    void refreshInfo(); // the header label

    int64_t clipId_ = -1;
    fc::TextDocument doc_; // working copy (box fields are read directly)
    bool loading_ = false; // suppress textChanged -> pushDoc feedback

    QLabel *info_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QTextEdit *editor_ = nullptr;
    QFontComboBox *family_ = nullptr;
    QSpinBox *size_ = nullptr;
    QToolButton *bold_ = nullptr;
    QToolButton *italic_ = nullptr;
    QToolButton *underline_ = nullptr;
    QPushButton *color_ = nullptr;
    QToolButton *alignLeft_ = nullptr;
    QToolButton *alignCenter_ = nullptr;
    QToolButton *alignRight_ = nullptr;
    QDoubleSpinBox *anchorX_ = nullptr;
    QDoubleSpinBox *anchorY_ = nullptr;
    QDoubleSpinBox *wrap_ = nullptr;
    QCheckBox *background_ = nullptr;
    QPushButton *bgColor_ = nullptr;
    QComboBox *animIn_ = nullptr;
    QSpinBox *animInFrames_ = nullptr;
    QComboBox *animOut_ = nullptr;
    QSpinBox *animOutFrames_ = nullptr;
    QComboBox *animDir_ = nullptr;
    QComboBox *emojiFont_ = nullptr;
};

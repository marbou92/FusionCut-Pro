#pragma once

#include <QWidget>

#include "text.h"

class QCheckBox;
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
// width, background + color) that place the block in the frame.
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

signals:
    // The Add button (no text clip selected yet).
    void addTextClipRequested();
    // The edited document for clipId (content, styles, box).
    void textEdited(int64_t clipId, const fc::TextDocument &doc);

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
};

#pragma once

#include <QWidget>

#include <QVector>

#include "system_fonts.h"
#include "text.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QImage;
class QLabel;
class QPushButton;
class QSpinBox;
class QTextEdit;
class QTimer;
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
//
// Live preview (suggestion #43): a compact letterboxed render of the
// layer sits in the panel. Any editor change re-emits previewRequested
// (throttled to 300 ms); MainWindow renders the layer and feeds the
// result back through setPreviewImage. Works with no clip selected too -
// the panel only requests and displays.
//
// Direction hint (suggestion #41): the editor's plain text is scanned
// (throttled to 250 ms) for RTL / complex-script content; a subtle label
// next to the alignment controls shows Auto (LTR) / Auto (RTL) / Mixed
// plus a "shaped text detected" note. Detection only - forcing a
// paragraph direction would need a model field (v1 documents this).
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

public slots:
    // The coordinator's render of the requested preview layer (a null
    // image falls back to the idle placeholder).
    void setPreviewImage(const QImage &image);

signals:
    // The Add button (no text clip selected yet).
    void addTextClipRequested();
    // The edited document for clipId (content, styles, box).
    void textEdited(int64_t clipId, const fc::TextDocument &doc);
    // The user picked an emoji font ("" = system default / none).
    void emojiFontPicked(const QString &path);
    // The (throttled) editor content to preview - emitted with or
    // without a selected clip; the receiver decides what to render.
    void previewRequested(const fc::TextDocument &doc);

private:
    void buildUi();
    void buildAnimChips();
    void buildPreview();
    // QTextEdit + box controls -> fc::TextDocument (no emission).
    fc::TextDocument documentFromEditor() const;
    // doc -> QTextEdit + box controls (signals suppressed while loading).
    void loadIntoEditor(const fc::TextDocument &doc);
    void pushDoc();          // documentFromEditor + emit textEdited
    void applyAlign();       // write align_ into every block's format
    void refreshInfo();      // the header label
    void updatePxReadout();  // #42: the size spinbox's display-scale readout
    void scanDirection();    // #41: RTL / complex-script scan of the editor
    void refreshAnimChips(); // #44: light the chip whose preset matches
    void applyAnimPreset(fc::TextAnimKind kind, int64_t frames); // #44: set the controls

    int64_t clipId_ = -1;
    fc::TextDocument doc_; // working copy (box fields are read directly)
    bool loading_ = false; // suppress textChanged -> pushDoc feedback

    QLabel *info_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QTextEdit *editor_ = nullptr;
    QFontComboBox *family_ = nullptr;
    QSpinBox *size_ = nullptr;
    QLabel *pxReadout_ = nullptr;
    QToolButton *bold_ = nullptr;
    QToolButton *italic_ = nullptr;
    QToolButton *underline_ = nullptr;
    QPushButton *color_ = nullptr;
    QToolButton *alignLeft_ = nullptr;
    QToolButton *alignCenter_ = nullptr;
    QToolButton *alignRight_ = nullptr;
    QLabel *dirHint_ = nullptr;    // "Auto (LTR)" / "Auto (RTL)" / "Mixed"
    QLabel *shapedHint_ = nullptr; // "shaped text detected..." (complex content)
    QDoubleSpinBox *anchorX_ = nullptr;
    QDoubleSpinBox *anchorY_ = nullptr;
    QDoubleSpinBox *wrap_ = nullptr;
    QCheckBox *background_ = nullptr;
    QPushButton *bgColor_ = nullptr;
    QToolButton *chipNone_ = nullptr;       // #44 preset chips (mirror the
    QToolButton *chipFade_ = nullptr;       //  animation fields that actually
    QToolButton *chipTypewriter_ = nullptr; // exist in TextDocument)
    QToolButton *chipSlide_ = nullptr;
    QWidget *previewHolder_ = nullptr;
    QLabel *previewLabel_ = nullptr;
    QTimer *dirTimer_ = nullptr;     // #41 throttle (250 ms)
    QTimer *previewTimer_ = nullptr; // #43 throttle (300 ms)
    QComboBox *animIn_ = nullptr;
    QSpinBox *animInFrames_ = nullptr;
    QComboBox *animOut_ = nullptr;
    QSpinBox *animOutFrames_ = nullptr;
    QComboBox *animDir_ = nullptr;
    QComboBox *emojiFont_ = nullptr;
};

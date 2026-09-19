#pragma once

#include <QPoint>
#include <QWidget>

#include <QTimer>

class QLabel;
class QLineEdit;
class QPropertyAnimation;
class QPushButton;
class QSlider;
class QToolButton;

// Program-monitor transport: play/pause, frame step, scrub, volume and a
// click-to-edit timecode readout (display-side drop-frame numbering via
// fc::timecode_display - the model stays in frames).
class TransportBar : public QWidget {
    Q_OBJECT

public:
    explicit TransportBar(QWidget *parent = nullptr);

    // Configures the range/timecode rate for the loaded clip.
    void setMedia(double durationSeconds, double fps);

    // Position in seconds (slider follows without emitting seekRequested).
    void setPosition(double seconds);

    // Reflects external play-state changes (e.g. pause at end of file).
    void setPlaying(bool playing);

signals:
    void playToggled(bool playing);
    void seekRequested(double seconds);
    void stepRequested(int frames); // +1 / -1
    // Preview volume (#27): 0..100 percent; the percent->dB mapping and
    // the audio hookup are wave-2 wiring.
    void volumeChanged(int percent);
    // Preview mute (#27): the button's own checked state.
    void muteToggled(bool muted);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onSliderMoved(int value);
    void onPlayClicked();

private:
    void refreshTimecode();
    void beginTimecodeEdit();
    void commitTimecodeEdit();
    void cancelTimecodeEdit();
    void shakeTimecodeEditor();
    void repositionStepFlash();
    void flashStepFrame();

    QPushButton *playButton_ = nullptr;
    QPushButton *stepBack_ = nullptr;
    QPushButton *stepFwd_ = nullptr;
    QSlider *position_ = nullptr;
    QLabel *timecode_ = nullptr;
    QLineEdit *timecodeEditor_ = nullptr;
    QPropertyAnimation *timecodeShake_ = nullptr;
    bool editing_ = false;
    QPoint timecodeEditorHome_;

    QToolButton *muteButton_ = nullptr;
    QSlider *volume_ = nullptr;

    QLabel *stepFlash_ = nullptr;
    QTimer stepFlashTimer_;

    double duration_ = 0.0;
    double fps_ = 24.0;
    double pos_ = 0.0;
    bool playing_ = false;
};

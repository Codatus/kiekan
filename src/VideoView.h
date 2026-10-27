// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QOpenGLWidget>
#include <QTimer>
#include <QWidget>

struct mpv_handle;
struct mpv_render_context;

class QLabel;
class QSlider;
class QToolButton;
class QVBoxLayout;

// Zeichenfläche für libmpv (OpenGL-Render-API). Funktioniert unter X11,
// Wayland und Windows gleichermaßen.
class MpvWidget : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit MpvWidget(QWidget *parent = nullptr);
    ~MpvWidget() override;

    bool isValid() const { return m_mpv != nullptr; }
    void command(const QStringList &args);
    void setProperty(const char *name, const QVariant &value);
    QVariant property(const char *name) const;
    // Aktuelles Videobild als QImage (für Kopieren/Speichern).
    QImage grabFrame();

Q_SIGNALS:
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void pausedChanged(bool paused);
    void videoSizeChanged(const QSize &size);
    void fileLoaded();
    void errorOccurred(const QString &message);
    void navigateRequested(int step);
    void doubleClicked();
    void contextMenuRequested(const QPoint &globalPos);
    void mouseMoved();

protected:
    void initializeGL() override;
    void paintGL() override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void handleEvents();
    static void onWakeup(void *ctx);
    static void onUpdate(void *ctx);

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_render = nullptr;
    int m_wheelAccum = 0;
};

// Videoanzeige mit Bedienleiste (Abspielen/Pause, Position, Lautstärke).
class VideoView : public QWidget
{
    Q_OBJECT

public:
    explicit VideoView(QWidget *parent = nullptr);

    bool open(const QString &path);
    void stop();
    void togglePause();
    void seekRelative(double seconds);
    void toggleMute();
    // Vollbild: Bedienleiste liegt über dem Bild und erscheint nur bei
    // Mausbewegung; sonst steht sie fest unter dem Video.
    void setFullscreenMode(bool on);

    QImage currentFrame() { return m_mpv->grabFrame(); }
    QSize videoSize() const { return m_videoSize; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    bool isValid() const { return m_mpv->isValid(); }

Q_SIGNALS:
    void stateChanged();       // Position, Dauer, Größe – für die Statuszeile
    void errorOccurred(const QString &message);
    void navigateRequested(int step);
    void doubleClicked();
    void contextMenuRequested(const QPoint &globalPos);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateTime();
    void revealControls();
    void placeOverlay();

    MpvWidget *m_mpv;
    QVBoxLayout *m_layout = nullptr;
    QTimer m_hideTimer;
    bool m_overlay = false;
    bool m_hoverControls = false;
    QWidget *m_controls;
    QToolButton *m_play;
    QToolButton *m_mute;
    QSlider *m_seek;
    QSlider *m_volume;
    QLabel *m_time;
    bool m_seeking = false;
    double m_position = 0;
    double m_duration = 0;
    QSize m_videoSize;
};

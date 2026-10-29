// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VideoView.h"

#include "Icons.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSettings>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <cmath>
#include <functional>
#include <vector>

namespace {

void *getProcAddress(void *, const char *name)
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    return ctx ? reinterpret_cast<void *>(ctx->getProcAddress(QByteArray(name))) : nullptr;
}

// Virtuelle und Software-Grafiktreiber (VirtualBox/VMware SVGA3D, llvmpipe,
// Microsoft-Basistreiber) zeichnen mpvs Shader-Kette fehlerhaft (fast schwarzes
// Bild). Dort genügt der einfache Darstellungsweg.
bool needsSimpleRendering()
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (!ctx)
        return false;
    const QByteArray renderer = QByteArray(reinterpret_cast<const char *>(ctx->functions()->glGetString(GL_RENDERER))).toLower();
    for (const char *weak : {"svga3d", "llvmpipe", "softpipe", "gdi generic", "microsoft basic render"}) {
        if (renderer.contains(weak))
            return true;
    }
    return false;
}

// Schieberegler, der beim Klick direkt an die Klickstelle springt (wie in
// Videoplayern üblich) statt nur seitenweise weiterzurücken.
class ClickSlider : public QSlider
{
public:
    using QSlider::QSlider;

    // Liefert den Tooltip-Text für einen Wert (z. B. die Zielzeit beim Überfahren).
    std::function<QString(int)> hoverText;

protected:
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (hoverText && !(event->buttons() & Qt::LeftButton)) {
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
            const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
            const int pos = int(event->position().x()) - groove.x() - handle.width() / 2;
            const int v = QStyle::sliderValueFromPosition(minimum(), maximum(), pos, groove.width() - handle.width(), opt.upsideDown);
            QToolTip::showText(event->globalPosition().toPoint(), hoverText(v), this);
        }
        QSlider::mouseMoveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
            if (!handle.contains(event->position().toPoint())) {
                const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
                const int span = groove.width() - handle.width();
                const int pos = int(event->position().x()) - groove.x() - handle.width() / 2;
                setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), pos, span, opt.upsideDown));
            }
        }
        // Der Griff liegt jetzt unter der Maus: normales Ziehen schließt an.
        QSlider::mousePressEvent(event);
    }
};

QString formatTime(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0)
        seconds = 0;
    const int s = int(seconds);
    if (s >= 3600)
        return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

// --- MpvWidget -------------------------------------------------------------

MpvWidget::MpvWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    m_mpv = mpv_create();
    if (!m_mpv)
        return;

    // Nur in das eingebettete Widget zeichnen: ohne diese Vorgabe öffnet mpv ein
    // eigenes Fenster, wenn eine Datei geladen wird, bevor OpenGL bereit ist (Windows).
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "msg-level", "all=no");
    // Kein Netz: Videos sind lokale Dateien. Eine präparierte Datei (z. B. eine
    // als .mp4 getarnte HLS-Wiedergabeliste) darf nichts nachladen; keine
    // mpv-Konfiguration und keine Skripte (ytdl_hook würde youtube-dl starten).
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "load-scripts", "no");
    mpv_set_option_string(m_mpv, "ytdl", "no");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "demuxer-lavf-o", "protocol_whitelist=[file,crypto,data]");
    mpv_set_option_string(m_mpv, "stream-lavf-o", "protocol_whitelist=[file,crypto,data]");
    // Fehlersuche: KIEKAN_MPV_LOG=<datei> schreibt ein ausführliches mpv-Protokoll.
    const QByteArray mpvLog = qgetenv("KIEKAN_MPV_LOG");
    if (!mpvLog.isEmpty()) {
        mpv_set_option_string(m_mpv, "log-file", mpvLog.constData());
        mpv_set_option_string(m_mpv, "msg-level", "all=v");
    }
    // Fehlersuche/Treiberprobleme: KIEKAN_MPV_OPTIONS="name=wert;name=wert"
    const QStringList extra = qEnvironmentVariable("KIEKAN_MPV_OPTIONS").split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &opt : extra) {
        const int eq = opt.indexOf(QLatin1Char('='));
        if (eq > 0)
            mpv_set_option_string(m_mpv, opt.left(eq).trimmed().toUtf8().constData(), opt.mid(eq + 1).trimmed().toUtf8().constData());
    }
    mpv_set_option_string(m_mpv, "hwdec", "auto-safe");
    mpv_set_option_string(m_mpv, "keep-open", "yes");          // letztes Bild stehen lassen
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "audio-client-name", "KiekAn!");
    mpv_set_option_string(m_mpv, "screenshot-format", "png");

    if (mpv_initialize(m_mpv) < 0) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }

    mpv_observe_property(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "dheight", MPV_FORMAT_INT64);
    mpv_set_wakeup_callback(m_mpv, &MpvWidget::onWakeup, this);
}

MpvWidget::~MpvWidget()
{
    if (m_render) {
        makeCurrent();
        mpv_render_context_free(m_render);
        doneCurrent();
    }
    if (m_mpv)
        mpv_terminate_destroy(m_mpv);
}

void MpvWidget::onWakeup(void *ctx)
{
    // Kommt aus einem mpv-Thread: Ereignisse im GUI-Thread abholen.
    auto *self = static_cast<MpvWidget *>(ctx);
    QMetaObject::invokeMethod(self, [self] { self->handleEvents(); }, Qt::QueuedConnection);
}

void MpvWidget::onUpdate(void *ctx)
{
    auto *self = static_cast<MpvWidget *>(ctx);
    QMetaObject::invokeMethod(self, [self] { self->update(); }, Qt::QueuedConnection);
}

void MpvWidget::initializeGL()
{
    if (!m_mpv || m_render)
        return;
    // Felder einzeln setzen: libmpv 2 hat „extra_exts“ entfernt.
    mpv_opengl_init_params gl{};
    gl.get_proc_address = &getProcAddress;
    gl.get_proc_address_ctx = nullptr;
    // Kein MPV_RENDER_PARAM_ADVANCED_CONTROL: das verlangt, auf jeden Update-Rückruf
    // mpv_render_context_update() zu bedienen; sonst hängt der Decoder beim
    // Anlegen von Bildpuffern (direct rendering) – unter Windows sofort.
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (needsSimpleRendering() && !qEnvironmentVariable("KIEKAN_MPV_OPTIONS").contains(QLatin1String("gpu-dumb-mode")))
        mpv_set_property_string(m_mpv, "gpu-dumb-mode", "yes");
    if (mpv_render_context_create(&m_render, m_mpv, params) < 0) {
        m_render = nullptr;
        Q_EMIT errorOccurred(tr("Videoausgabe konnte nicht eingerichtet werden (OpenGL)."));
        return;
    }
    mpv_render_context_set_update_callback(m_render, &MpvWidget::onUpdate, this);
}

void MpvWidget::paintGL()
{
    if (!m_render)
        return;
    const qreal dpr = devicePixelRatioF();
    mpv_opengl_fbo fbo{int(defaultFramebufferObject()), int(width() * dpr), int(height() * dpr), 0};
    int flip = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(m_render, params);
}

void MpvWidget::handleEvents()
{
    while (m_mpv) {
        mpv_event *ev = mpv_wait_event(m_mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;
        switch (ev->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(ev->data);
            const QByteArray name(prop->name);
            if (prop->format == MPV_FORMAT_DOUBLE) {
                const double v = *static_cast<double *>(prop->data);
                if (name == "time-pos")
                    Q_EMIT positionChanged(v);
                else if (name == "duration")
                    Q_EMIT durationChanged(v);
            } else if (prop->format == MPV_FORMAT_FLAG && name == "pause") {
                Q_EMIT pausedChanged(*static_cast<int *>(prop->data) != 0);
            } else if (prop->format == MPV_FORMAT_INT64 && (name == "dwidth" || name == "dheight")) {
                const QVariant w = property("dwidth"), h = property("dheight");
                Q_EMIT videoSizeChanged(QSize(w.toInt(), h.toInt()));
            } else if (prop->format == MPV_FORMAT_NONE && name == "duration") {
                Q_EMIT durationChanged(0);
            }
            break;
        }
        case MPV_EVENT_FILE_LOADED:
            Q_EMIT fileLoaded();
            break;
        case MPV_EVENT_END_FILE: {
            auto *end = static_cast<mpv_event_end_file *>(ev->data);
            if (end->reason == MPV_END_FILE_REASON_ERROR)
                Q_EMIT errorOccurred(QString::fromUtf8(mpv_error_string(end->error)));
            break;
        }
        default:
            break;
        }
    }
}

void MpvWidget::command(const QStringList &args)
{
    if (!m_mpv)
        return;
    std::vector<QByteArray> utf8;
    utf8.reserve(args.size());
    for (const QString &a : args)
        utf8.push_back(a.toUtf8());
    std::vector<const char *> argv;
    for (const QByteArray &a : utf8)
        argv.push_back(a.constData());
    argv.push_back(nullptr);
    mpv_command(m_mpv, argv.data());
}

void MpvWidget::setProperty(const char *name, const QVariant &value)
{
    if (!m_mpv)
        return;
    switch (value.typeId()) {
    case QMetaType::Bool: {
        int flag = value.toBool() ? 1 : 0;
        mpv_set_property(m_mpv, name, MPV_FORMAT_FLAG, &flag);
        break;
    }
    case QMetaType::Int:
    case QMetaType::LongLong: {
        int64_t v = value.toLongLong();
        mpv_set_property(m_mpv, name, MPV_FORMAT_INT64, &v);
        break;
    }
    case QMetaType::Double: {
        double v = value.toDouble();
        mpv_set_property(m_mpv, name, MPV_FORMAT_DOUBLE, &v);
        break;
    }
    default: {
        const QByteArray s = value.toString().toUtf8();
        mpv_set_property_string(m_mpv, name, s.constData());
    }
    }
}

QVariant MpvWidget::property(const char *name) const
{
    if (!m_mpv)
        return {};
    char *s = mpv_get_property_string(m_mpv, name);
    if (!s)
        return {};
    const QString v = QString::fromUtf8(s);
    mpv_free(s);
    return v;
}

QImage MpvWidget::grabFrame()
{
    if (!m_mpv)
        return {};
    mpv_node args[2];
    args[0].format = MPV_FORMAT_STRING;
    args[0].u.string = const_cast<char *>("screenshot-raw");
    args[1].format = MPV_FORMAT_STRING;
    args[1].u.string = const_cast<char *>("video");
    mpv_node_list list{2, args, nullptr};
    mpv_node cmd;
    cmd.format = MPV_FORMAT_NODE_ARRAY;
    cmd.u.list = &list;

    mpv_node result;
    if (mpv_command_node(m_mpv, &cmd, &result) < 0)
        return {};

    int64_t w = 0, h = 0, stride = 0;
    QByteArray format;
    const mpv_byte_array *data = nullptr;
    if (result.format == MPV_FORMAT_NODE_MAP) {
        for (int i = 0; i < result.u.list->num; ++i) {
            const QByteArray key(result.u.list->keys[i]);
            const mpv_node &v = result.u.list->values[i];
            if (key == "w") w = v.u.int64;
            else if (key == "h") h = v.u.int64;
            else if (key == "stride") stride = v.u.int64;
            else if (key == "format") format = v.u.string;
            else if (key == "data" && v.format == MPV_FORMAT_BYTE_ARRAY) data = v.u.ba;
        }
    }

    QImage img;
    // „bgr0“ entspricht im Speicher genau QImage::Format_RGB32 (Little Endian).
    if (data && format == "bgr0" && w > 0 && h > 0 && stride >= w * 4) {
        img = QImage(int(w), int(h), QImage::Format_RGB32);
        const auto *src = static_cast<const uchar *>(data->data);
        for (int y = 0; y < h; ++y)
            memcpy(img.scanLine(y), src + y * stride, size_t(w) * 4);
    }
    mpv_free_node_contents(&result);
    return img;
}

void MpvWidget::wheelEvent(QWheelEvent *event)
{
    m_wheelAccum += event->angleDelta().y();
    while (std::abs(m_wheelAccum) >= 120) {
        const int dir = m_wheelAccum > 0 ? 1 : -1;
        m_wheelAccum -= dir * 120;
        Q_EMIT navigateRequested(-dir);
    }
    event->accept();
}

void MpvWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        Q_EMIT doubleClicked();
}

void MpvWidget::mouseMoveEvent(QMouseEvent *event)
{
    Q_EMIT mouseMoved();
    QOpenGLWidget::mouseMoveEvent(event);
}

void MpvWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton)
        Q_EMIT contextMenuRequested(event->globalPosition().toPoint());
}

// --- VideoView -------------------------------------------------------------

VideoView::VideoView(QWidget *parent)
    : QWidget(parent)
{
    m_mpv = new MpvWidget(this);

    m_play = new QToolButton(this);
    m_play->setAutoRaise(true);
    m_play->setToolTip(tr("Abspielen / Pause (Leertaste)"));
    connect(m_play, &QToolButton::clicked, this, &VideoView::togglePause);

    m_time = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    m_time->setMinimumWidth(m_time->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00 / 00:00:00")));

    auto *seek = new ClickSlider(Qt::Horizontal, this);
    seek->setMouseTracking(true);
    seek->hoverText = [](int v) { return formatTime(v / 10.0); };
    m_seek = seek;
    m_seek->setRange(0, 0);
    connect(m_seek, &QSlider::sliderPressed, this, [this] { m_seeking = true; });
    connect(m_seek, &QSlider::sliderReleased, this, [this] {
        m_seeking = false;
        if (m_overlay)
            m_hideTimer.start();
        m_mpv->command({QStringLiteral("seek"), QString::number(m_seek->value() / 10.0), QStringLiteral("absolute")});
    });
    connect(m_seek, &QSlider::sliderMoved, this, [this](int v) {
        m_mpv->command({QStringLiteral("seek"), QString::number(v / 10.0), QStringLiteral("absolute+keyframes")});
    });

    m_mute = new QToolButton(this);
    m_mute->setAutoRaise(true);
    m_mute->setCheckable(true);
    m_mute->setToolTip(tr("Ton aus (M)"));
    m_mute->setIcon(Icons::get(QStringLiteral("audio-volume-high")));
    connect(m_mute, &QToolButton::toggled, this, [this](bool on) {
        m_mpv->setProperty("mute", on);
        m_mute->setIcon(Icons::get(on ? QStringLiteral("audio-volume-muted") : QStringLiteral("audio-volume-high")));
    });

    m_volume = new ClickSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setMaximumWidth(110);
    m_volume->setToolTip(tr("Lautstärke"));
    m_volume->setValue(QSettings().value(QStringLiteral("video/volume"), 80).toInt());
    m_mpv->setProperty("volume", double(m_volume->value()));
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        m_mpv->setProperty("volume", double(v));
        QSettings().setValue(QStringLiteral("video/volume"), v);
    });

    m_controls = new QWidget(this);
    auto *bar = new QHBoxLayout(m_controls);
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(m_play);
    bar->addWidget(m_time);
    bar->addWidget(m_seek, 1);
    bar->addWidget(m_mute);
    bar->addWidget(m_volume);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_layout->addWidget(m_mpv, 1);
    m_layout->addWidget(m_controls);

    // Vollbild: Leiste nach kurzer Ruhe ausblenden, Mauszeiger ebenso.
    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(2500);
    connect(&m_hideTimer, &QTimer::timeout, this, [this] {
        if (!m_overlay || m_hoverControls || m_seeking)
            return;
        m_controls->hide();
        m_mpv->setCursor(Qt::BlankCursor);
    });
    m_mpv->setMouseTracking(true);
    connect(m_mpv, &MpvWidget::mouseMoved, this, [this] {
        if (m_overlay)
            revealControls();
    });
    m_controls->installEventFilter(this);

    connect(m_mpv, &MpvWidget::positionChanged, this, [this](double s) {
        m_position = s;
        if (!m_seeking)
            m_seek->setValue(int(s * 10));
        updateTime();
    });
    connect(m_mpv, &MpvWidget::durationChanged, this, [this](double s) {
        m_duration = s;
        m_seek->setRange(0, int(s * 10));
        updateTime();
        Q_EMIT stateChanged();
    });
    connect(m_mpv, &MpvWidget::pausedChanged, this, [this](bool paused) {
        m_play->setIcon(Icons::get(paused ? QStringLiteral("media-playback-start")
                                                : QStringLiteral("media-playback-pause")));
    });
    connect(m_mpv, &MpvWidget::videoSizeChanged, this, [this](const QSize &s) {
        m_videoSize = s;
        Q_EMIT stateChanged();
    });
    connect(m_mpv, &MpvWidget::errorOccurred, this, &VideoView::errorOccurred);
    connect(m_mpv, &MpvWidget::navigateRequested, this, &VideoView::navigateRequested);
    connect(m_mpv, &MpvWidget::doubleClicked, this, &VideoView::doubleClicked);
    connect(m_mpv, &MpvWidget::contextMenuRequested, this, &VideoView::contextMenuRequested);

    m_play->setIcon(Icons::get(QStringLiteral("media-playback-pause")));
}

bool VideoView::open(const QString &path)
{
    if (!m_mpv->isValid())
        return false;
    m_position = m_duration = 0;
    m_videoSize = QSize();
    m_seek->setRange(0, 0);
    m_mpv->command({QStringLiteral("loadfile"), path});
    m_mpv->setProperty("pause", false);
    updateTime();
    return true;
}

void VideoView::stop()
{
    m_mpv->command({QStringLiteral("stop")});
    m_position = m_duration = 0;
    m_videoSize = QSize();
}

void VideoView::togglePause()
{
    // Am Ende angekommen: „Abspielen“ beginnt von vorn.
    if (m_duration > 0 && m_position >= m_duration - 0.25) {
        m_mpv->command({QStringLiteral("seek"), QStringLiteral("0"), QStringLiteral("absolute")});
        m_mpv->setProperty("pause", false);
        return;
    }
    m_mpv->command({QStringLiteral("cycle"), QStringLiteral("pause")});
}

void VideoView::seekRelative(double seconds)
{
    m_mpv->command({QStringLiteral("seek"), QString::number(seconds), QStringLiteral("relative")});
}

void VideoView::toggleMute()
{
    m_mute->toggle();
}

void VideoView::setFullscreenMode(bool on)
{
    if (on == m_overlay)
        return;
    m_overlay = on;
    if (on) {
        // Leiste aus dem Layout nehmen und über das Bild legen
        m_layout->removeWidget(m_controls);
        m_controls->setAutoFillBackground(true);
        m_controls->setStyleSheet(QStringLiteral(
            "QWidget { background: rgba(20, 20, 20, 200); color: white; }"
            "QToolButton { background: transparent; }"));
        placeOverlay();
        m_controls->raise();
        revealControls();
    } else {
        m_hideTimer.stop();
        m_controls->setStyleSheet(QString());
        m_controls->setAutoFillBackground(false);
        m_layout->addWidget(m_controls);
        m_controls->show();
        m_mpv->unsetCursor();
    }
}

void VideoView::revealControls()
{
    m_mpv->unsetCursor();
    if (!m_controls->isVisible()) {
        placeOverlay();
        m_controls->show();
        m_controls->raise();
    }
    m_hideTimer.start();
}

void VideoView::placeOverlay()
{
    const int h = m_controls->sizeHint().height() + 8;
    m_controls->setGeometry(0, height() - h, width(), h);
}

void VideoView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_overlay)
        placeOverlay();
}

bool VideoView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_controls) {
        if (event->type() == QEvent::Enter) {
            m_hoverControls = true;
            m_hideTimer.stop();
        } else if (event->type() == QEvent::Leave) {
            m_hoverControls = false;
            if (m_overlay)
                m_hideTimer.start();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void VideoView::updateTime()
{
    m_time->setText(formatTime(m_position) + QStringLiteral(" / ") + formatTime(m_duration));
    Q_EMIT stateChanged();
}

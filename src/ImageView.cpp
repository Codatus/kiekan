// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageView.h"

#include "Accessibility.h"

#include <QAccessible>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kMinZoom = 0.01;
constexpr double kMaxZoom = 32.0;
constexpr double kZoomStep = 1.25;
}

ImageView::ImageView(QWidget *parent)
    : QWidget(parent)
    , m_background(0x2b, 0x2b, 0x2b)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);

    m_smoothTimer.setSingleShot(true);
    m_smoothTimer.setInterval(150);
    connect(&m_smoothTimer, &QTimer::timeout, this, [this] {
        rebuildScaled();
        update();
    });
}

// --- Bild setzen -----------------------------------------------------------

void ImageView::setImage(const QImage &image)
{
    clearTextLayer();
    m_image = image;
    m_selection = QRect();
    m_fit = true;
    m_zoom = fitZoom();
    m_offset = QPointF();
    rebuildScaled();
    Q_EMIT selectionChanged(m_selection);
    Q_EMIT zoomChanged(m_zoom);
    Q_EMIT imageChanged();
    update();
}

void ImageView::replaceImage(const QImage &image)
{
    clearTextLayer();
    m_image = image;
    m_selection = QRect();
    if (m_fit)
        m_zoom = fitZoom();
    clampOffset();
    rebuildScaled();
    Q_EMIT selectionChanged(m_selection);
    Q_EMIT zoomChanged(m_zoom);
    Q_EMIT imageChanged();
    update();
}

void ImageView::setFrame(const QImage &frame)
{
    const bool sizeChanged = frame.size() != m_image.size();
    m_image = frame;
    if (sizeChanged && m_fit)
        m_zoom = fitZoom();
    rebuildScaled();
    update();
}

void ImageView::clear()
{
    clearTextLayer();
    m_image = QImage();
    m_scaled = QImage();
    m_selection = QRect();
    Q_EMIT selectionChanged(m_selection);
    Q_EMIT imageChanged();
    update();
}

void ImageView::setBackground(const QColor &color)
{
    m_background = color;
    update();
}

// --- Zoom ------------------------------------------------------------------

double ImageView::fitZoom() const
{
    if (m_image.isNull() || width() <= 0 || height() <= 0)
        return 1.0;
    const double dpr = devicePixelRatioF();
    const double z = std::min(width() * dpr / m_image.width(),
                              height() * dpr / m_image.height());
    // Kleine Bilder nicht aufblasen – wie in IrfanView.
    return std::min(z, 1.0);
}

void ImageView::setFitMode()
{
    m_fit = true;
    m_zoom = fitZoom();
    m_offset = QPointF();
    invalidateScaled(true);
    Q_EMIT zoomChanged(m_zoom);
    update();
}

void ImageView::setZoom(double zoom)
{
    zoomAt(zoom, QPointF(width() / 2.0, height() / 2.0));
}

void ImageView::zoomIn()
{
    double z = m_zoom * kZoomStep;
    if (m_zoom < 1.0 && z > 1.0)
        z = 1.0;     // 100 % nicht überspringen
    setZoom(z);
}

void ImageView::zoomOut()
{
    double z = m_zoom / kZoomStep;
    if (m_zoom > 1.0 && z < 1.0)
        z = 1.0;
    setZoom(z);
}

void ImageView::zoomAt(double zoom, const QPointF &widgetPos)
{
    if (m_image.isNull())
        return;
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    const QPointF anchor = toImage(widgetPos);

    m_fit = false;
    m_zoom = zoom;
    const double scale = m_zoom / devicePixelRatioF();
    m_offset = widgetPos - anchor * scale;
    clampOffset();

    invalidateScaled(false);
    Q_EMIT zoomChanged(m_zoom);
    update();
}

// --- Geometrie -------------------------------------------------------------

QSizeF ImageView::scaledSize() const
{
    const double scale = m_zoom / devicePixelRatioF();
    return QSizeF(m_image.width() * scale, m_image.height() * scale);
}

QRectF ImageView::imageRect() const
{
    const QSizeF s = scaledSize();
    const double x = s.width() <= width() ? (width() - s.width()) / 2.0 : m_offset.x();
    const double y = s.height() <= height() ? (height() - s.height()) / 2.0 : m_offset.y();
    return QRectF(QPointF(x, y), s);
}

QPointF ImageView::toImage(const QPointF &widgetPos) const
{
    const QRectF r = imageRect();
    if (r.width() <= 0 || r.height() <= 0)
        return QPointF();
    return QPointF((widgetPos.x() - r.x()) * m_image.width() / r.width(),
                   (widgetPos.y() - r.y()) * m_image.height() / r.height());
}

QPointF ImageView::toWidget(const QPointF &imagePos) const
{
    const QRectF r = imageRect();
    return QPointF(r.x() + imagePos.x() * r.width() / m_image.width(),
                   r.y() + imagePos.y() * r.height() / m_image.height());
}

void ImageView::clampOffset()
{
    const QSizeF s = scaledSize();
    m_offset.setX(s.width() > width() ? std::clamp(m_offset.x(), width() - s.width(), 0.0) : 0.0);
    m_offset.setY(s.height() > height() ? std::clamp(m_offset.y(), height() - s.height(), 0.0) : 0.0);
}

bool ImageView::canPan() const
{
    if (m_image.isNull())
        return false;
    const QSizeF s = scaledSize();
    return s.width() > width() + 0.5 || s.height() > height() + 0.5;
}

void ImageView::panBy(int dx, int dy)
{
    m_offset -= QPointF(dx, dy);
    clampOffset();
    update();
}

// --- Geglättete Verkleinerung ---------------------------------------------

void ImageView::invalidateScaled(bool immediate)
{
    if (immediate) {
        m_smoothTimer.stop();
        rebuildScaled();
    } else {
        m_smoothTimer.start();
    }
}

void ImageView::rebuildScaled()
{
    if (m_image.isNull() || m_zoom >= 0.999) {
        m_scaled = QImage();
        m_scaledZoom = 0;
        return;
    }
    const QSize target(std::max(1, int(std::lround(m_image.width() * m_zoom))),
                       std::max(1, int(std::lround(m_image.height() * m_zoom))));
    if (m_scaledZoom == m_zoom && m_scaledKey == m_image.cacheKey() && m_scaled.size() == target)
        return;
    m_scaled = m_image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    m_scaled.setDevicePixelRatio(devicePixelRatioF());
    m_scaledZoom = m_zoom;
    m_scaledKey = m_image.cacheKey();
}

// --- Zeichnen --------------------------------------------------------------

void ImageView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_background);
    if (m_image.isNull())
        return;

    const QRectF target = imageRect();

    if (m_zoom < 0.999) {
        if (!m_scaled.isNull() && m_scaledZoom == m_zoom && m_scaledKey == m_image.cacheKey()) {
            p.drawImage(target.topLeft(), m_scaled);
        } else {
            // Übergangsweise ungeglättet, bis der Timer die glatte Kopie baut.
            p.drawImage(target, m_image);
        }
    } else {
        // Nur den sichtbaren Ausschnitt zeichnen.
        const QRectF visible = target.intersected(QRectF(rect()));
        if (!visible.isEmpty()) {
            const double f = m_image.width() / target.width();
            const QRectF source((visible.x() - target.x()) * f,
                                (visible.y() - target.y()) * f,
                                visible.width() * f,
                                visible.height() * f);
            p.drawImage(visible, m_image, source);
        }
    }

    if (m_textMode && !m_text.isEmpty()) {
        // Erkannte Wörter leicht andeuten, markierte deutlich hervorheben
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        const QColor hint(70, 130, 220, 28);
        QColor marked = palette().color(QPalette::Highlight);
        marked.setAlpha(110);
        for (int i = 0; i < m_text.words.size(); ++i) {
            const QRect &b = m_text.words.at(i).box;
            const QRectF r(toWidget(b.topLeft()), toWidget(b.topLeft() + QPointF(b.width(), b.height())));
            const bool sel = m_textFrom >= 0 && i >= m_textFrom && i <= m_textTo;
            p.setBrush(sel ? marked : hint);
            p.drawRect(r.adjusted(-1, -1, 1, 1));
        }
        // Textcursor, sobald mit der Tastatur gelesen wird
        if (m_caret >= 0 && hasFocus()) {
            const QRect c = characterRect(m_caret);
            p.fillRect(QRect(c.left(), c.top(), 2, c.height()), palette().color(QPalette::Highlight));
        }
    }

    if (!m_selection.isEmpty()) {
        const QRectF sel = QRectF(toWidget(m_selection.topLeft()),
                                  toWidget(m_selection.topLeft() + QPointF(m_selection.width(), m_selection.height())));
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::black, 1));
        p.drawRect(sel);
        p.setPen(QPen(Qt::white, 1, Qt::DashLine));
        p.drawRect(sel);

        const QString label = QStringLiteral("%1 × %2").arg(m_selection.width()).arg(m_selection.height());
        const QRect textRect = p.fontMetrics().boundingRect(label).adjusted(-4, -2, 4, 2);
        QRectF box(sel.topLeft() + QPointF(0, -textRect.height() - 2), textRect.size());
        if (box.top() < 0)
            box.moveTop(sel.top() + 2);
        p.fillRect(box, QColor(0, 0, 0, 170));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, label);
    }
}

void ImageView::resizeEvent(QResizeEvent *)
{
    if (m_fit && !m_image.isNull()) {
        const double z = fitZoom();
        if (z != m_zoom) {
            m_zoom = z;
            invalidateScaled(false);
            Q_EMIT zoomChanged(m_zoom);
        }
    }
    clampOffset();
}

// --- Maus ------------------------------------------------------------------

void ImageView::wheelEvent(QWheelEvent *event)
{
    if (m_image.isNull()) {
        event->ignore();
        return;
    }
    const int delta = event->angleDelta().y();
    if (event->modifiers() & Qt::ControlModifier) {
        const double factor = std::pow(kZoomStep, delta / 120.0);
        zoomAt(m_zoom * factor, event->position());
    } else {
        m_wheelAccum += delta;
        while (std::abs(m_wheelAccum) >= 120) {
            const int dir = m_wheelAccum > 0 ? 1 : -1;
            m_wheelAccum -= dir * 120;
            Q_EMIT navigateRequested(-dir);   // Rad nach unten = nächstes Bild
        }
    }
    event->accept();
}

void ImageView::mousePressEvent(QMouseEvent *event)
{
    if (m_image.isNull()) {
        if (event->button() == Qt::RightButton)
            Q_EMIT contextMenuRequested(event->globalPosition().toPoint());
        return;
    }
    if (event->button() == Qt::LeftButton && m_textMode && !m_text.isEmpty()) {
        const int w = wordAt(toImage(event->position()), true);
        m_textSelecting = true;
        m_textAnchor = w;
        const int hit = wordAt(toImage(event->position()), false);
        m_textFrom = m_textTo = hit;   // Klick ins Leere hebt die Markierung auf
        if (hit < 0)
            m_textAnchor = w;
        else if (m_caret >= 0 || QAccessible::isActive())
            setCaretOffset(offsetAt(event->position().toPoint()));
        notifyTextSelection();
        update();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_selecting = true;
        QPointF pt = toImage(event->position());
        pt.setX(std::clamp(pt.x(), 0.0, double(m_image.width())));
        pt.setY(std::clamp(pt.y(), 0.0, double(m_image.height())));
        m_selectStart = pt;
        if (!m_selection.isEmpty()) {
            m_selection = QRect();
            Q_EMIT selectionChanged(m_selection);
            update();
        }
    } else if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panMoved = false;
        m_panLast = event->position().toPoint();
        m_pressPos = m_panLast;
    }
}

void ImageView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_textMode && !m_text.isEmpty() && !m_panning) {
        const int hit = wordAt(toImage(event->position()), false);
        setCursor(hit >= 0 || m_textSelecting ? Qt::IBeamCursor : Qt::ArrowCursor);
    }
    if (m_textSelecting) {
        const int w = wordAt(toImage(event->position()), true);
        if (w >= 0 && m_textAnchor >= 0) {
            m_textFrom = std::min(m_textAnchor, w);
            m_textTo = std::max(m_textAnchor, w);
            notifyTextSelection();
            update();
        }
        return;
    }
    if (m_selecting) {
        QPointF pt = toImage(event->position());
        pt.setX(std::clamp(pt.x(), 0.0, double(m_image.width())));
        pt.setY(std::clamp(pt.y(), 0.0, double(m_image.height())));
        const QPoint a(int(std::floor(std::min(pt.x(), m_selectStart.x()))),
                       int(std::floor(std::min(pt.y(), m_selectStart.y()))));
        const QPoint b(int(std::ceil(std::max(pt.x(), m_selectStart.x()))),
                       int(std::ceil(std::max(pt.y(), m_selectStart.y()))));
        m_selection = QRect(a, QSize(b.x() - a.x(), b.y() - a.y()));
        Q_EMIT selectionChanged(m_selection);
        update();
    } else if (m_panning) {
        const QPoint pos = event->position().toPoint();
        if ((pos - m_pressPos).manhattanLength() > 4) {
            m_panMoved = true;
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_panMoved && canPan()) {
            m_offset += pos - m_panLast;
            clampOffset();
            update();
        }
        m_panLast = pos;
    }
}

void ImageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_textSelecting) {
        m_textSelecting = false;
        return;
    }
    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        if (m_selection.width() < 2 || m_selection.height() < 2) {
            m_selection = QRect();
            Q_EMIT selectionChanged(m_selection);
            update();
        }
    } else if ((event->button() == Qt::RightButton || event->button() == Qt::MiddleButton) && m_panning) {
        m_panning = false;
        unsetCursor();
        if (!m_panMoved && event->button() == Qt::RightButton)
            Q_EMIT contextMenuRequested(event->globalPosition().toPoint());
    }
}

void ImageView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    // Im Textmodus: Doppelklick markiert das Wort statt Vollbild
    if (m_textMode && !m_text.isEmpty()) {
        const int hit = wordAt(toImage(event->position()), false);
        if (hit >= 0) {
            m_textFrom = m_textTo = m_textAnchor = hit;
            notifyTextSelection();
            update();
        }
        return;
    }
    Q_EMIT doubleClicked();
}

// --- Erkannter Text ----------------------------------------------------------

void ImageView::setTextLayer(const TextLayer &layer)
{
    const QString old = m_textFull;
    m_text = layer;
    m_textFrom = m_textTo = m_textAnchor = -1;
    m_textFull = m_text.text();
    m_wordStart = m_text.wordOffsets();
    m_caret = -1;
    Accessibility::textReplaced(this, old, m_textFull);
    update();
}

void ImageView::clearTextLayer()
{
    const bool had = !m_text.isEmpty();
    const QString old = m_textFull;
    m_text = TextLayer();
    m_textFrom = m_textTo = m_textAnchor = -1;
    m_textSelecting = false;
    m_textFull.clear();
    m_wordStart.clear();
    m_caret = -1;
    if (had) {
        Accessibility::textReplaced(this, old, QString());
        Q_EMIT textLayerCleared();
    }
}

void ImageView::setAccessibleInfo(const QString &name, const QString &description)
{
    // Angesagt wird erst mit Accessibility::present() – der Aufrufer
    // entscheidet, wann (z. B. erst, wenn das Bild geladen ist).
    m_a11yName = name;
    m_a11yDescription = description;
}

void ImageView::notifyTextSelection()
{
    Q_EMIT textSelectionChanged();
    if (QAccessible::isActive()) {
        int start, end;
        selectionOffsets(&start, &end);
        Accessibility::selectionChanged(this, start, end, caretOffset());
    }
}

// --- Textcursor (Tastatur und Bildschirmleser) ------------------------------

int ImageView::wordAtOffset(int offset) const
{
    if (m_wordStart.isEmpty())
        return -1;
    const auto it = std::upper_bound(m_wordStart.begin(), m_wordStart.end(), offset);
    return std::max(0, int(it - m_wordStart.begin()) - 1);
}

void ImageView::selectionOffsets(int *start, int *end) const
{
    if (m_textFrom < 0 || m_wordStart.isEmpty()) {
        *start = *end = caretOffset();
        return;
    }
    *start = m_wordStart.at(m_textFrom);
    *end = m_wordStart.at(m_textTo) + int(m_text.words.at(m_textTo).text.size());
}

void ImageView::setSelectionOffsets(int start, int end)
{
    if (m_text.isEmpty())
        return;
    if (end <= start) {
        m_textFrom = m_textTo = -1;
    } else {
        m_textFrom = wordAtOffset(start);
        m_textTo = wordAtOffset(end - 1);
    }
    notifyTextSelection();
    update();
}

void ImageView::setCaretOffset(int offset)
{
    if (m_textFull.isEmpty())
        return;
    m_caret = std::clamp(offset, 0, int(m_textFull.size()));
    Accessibility::caretMoved(this, m_caret);
    update();
}

QRect ImageView::characterRect(int offset) const
{
    const int w = wordAtOffset(offset);
    if (w < 0)
        return {};
    const QRect &b = m_text.words.at(w).box;
    const QRectF r(toWidget(b.topLeft()), toWidget(b.topLeft() + QPointF(b.width(), b.height())));
    const int len = std::max(1, int(m_text.words.at(w).text.size()));
    // Zwischenraum hinter dem Wort: am Wortende
    const int inWord = std::clamp(offset - m_wordStart.at(w), 0, len);
    const double cw = r.width() / len;
    return QRectF(r.left() + inWord * cw, r.top(), inWord == len ? 1.0 : cw, r.height()).toAlignedRect();
}

int ImageView::offsetAt(const QPoint &widgetPos) const
{
    const int w = wordAt(toImage(widgetPos), false);
    if (w < 0)
        return -1;
    const QRect &b = m_text.words.at(w).box;
    const double x = toImage(widgetPos).x();
    const int len = int(m_text.words.at(w).text.size());
    const int inWord = std::clamp(int((x - b.left()) / std::max(1, b.width()) * len), 0, std::max(0, len - 1));
    return m_wordStart.at(w) + inWord;
}

bool ImageView::caretKey(const QKeyEvent *event) const
{
    if (!m_textMode || m_textFull.isEmpty())
        return false;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Home:
    case Qt::Key_End:
        // Ohne Umschalt nur, wenn die Pfeiltasten den Cursor bewegen sollen
        // (Bildschirmleser), sonst blättern sie wie gewohnt.
        return (event->modifiers() & Qt::ShiftModifier) || m_caretKeys;
    default:
        return false;
    }
}

void ImageView::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    // Nach Qts eigenem Fokusereignis: der Bildschirmleser soll beim Element
    // (Bild bzw. Text) landen, nicht bei der leeren Fläche.
    QTimer::singleShot(0, this, [this] { Accessibility::focusIn(this); });
}

bool ImageView::event(QEvent *event)
{
    // Pos1/Ende sind sonst Tastenkürzel des Fensters (erstes/letztes Bild).
    if (event->type() == QEvent::ShortcutOverride && caretKey(static_cast<QKeyEvent *>(event))) {
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void ImageView::moveCaret(int offset, bool extend)
{
    const int before = caretOffset();
    const int anchorWord = m_textAnchor >= 0 ? m_textAnchor : wordAtOffset(before);
    setCaretOffset(offset);
    if (extend) {
        // Markierung wortweise vom Anker bis zum Cursor
        m_textAnchor = anchorWord;
        const int w = wordAtOffset(m_caret > 0 && m_caret > before ? m_caret - 1 : m_caret);
        m_textFrom = std::min(anchorWord, w);
        m_textTo = std::max(anchorWord, w);
        notifyTextSelection();
    } else if (m_textFrom >= 0) {
        m_textFrom = m_textTo = m_textAnchor = -1;
        notifyTextSelection();
    } else {
        m_textAnchor = -1;
    }
    update();
}

void ImageView::keyPressEvent(QKeyEvent *event)
{
    if (!caretKey(event)) {
        QWidget::keyPressEvent(event);   // weiter ans Fenster (Blättern usw.)
        return;
    }
    const QString &t = m_textFull;
    const int n = int(t.size());
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool extend = event->modifiers() & Qt::ShiftModifier;
    int pos = caretOffset();
    const int lineStart = pos > 0 ? int(t.lastIndexOf(QLatin1Char('\n'), pos - 1)) + 1 : 0;
    int lineEnd = int(t.indexOf(QLatin1Char('\n'), pos));
    if (lineEnd < 0)
        lineEnd = n;
    switch (event->key()) {
    case Qt::Key_Left:
        if (ctrl) {
            const int w = wordAtOffset(pos);
            pos = (pos > m_wordStart.at(w) || w == 0) ? m_wordStart.at(w) : m_wordStart.at(w - 1);
        } else {
            pos = std::max(0, pos - 1);
        }
        break;
    case Qt::Key_Right:
        if (ctrl) {
            const int w = wordAtOffset(pos);
            pos = w + 1 < m_wordStart.size() ? m_wordStart.at(w + 1) : n;
        } else {
            pos = std::min(n, pos + 1);
        }
        break;
    case Qt::Key_Up:
        if (lineStart > 0) {
            const int prevStart = lineStart >= 2 ? int(t.lastIndexOf(QLatin1Char('\n'), lineStart - 2)) + 1 : 0;
            pos = std::min(prevStart + (pos - lineStart), lineStart - 1);
        } else {
            pos = 0;
        }
        break;
    case Qt::Key_Down:
        if (lineEnd < n) {
            const int nextStart = lineEnd + 1;
            int nextEnd = int(t.indexOf(QLatin1Char('\n'), nextStart));
            if (nextEnd < 0)
                nextEnd = n;
            pos = std::min(nextStart + (pos - lineStart), nextEnd);
        } else {
            pos = n;
        }
        break;
    case Qt::Key_Home:
        pos = ctrl ? 0 : lineStart;
        break;
    case Qt::Key_End:
        pos = ctrl ? n : lineEnd;
        break;
    default:
        break;
    }
    moveCaret(pos, extend);
    event->accept();
}

void ImageView::setTextMode(bool on)
{
    m_textMode = on;
    if (!on) {
        m_textFrom = m_textTo = -1;
        unsetCursor();
    } else if (!m_selection.isEmpty()) {
        clearSelection();
    }
    update();
}

QString ImageView::selectedText() const
{
    return m_textFrom < 0 ? QString() : m_text.text(m_textFrom, m_textTo);
}

void ImageView::selectAllText()
{
    if (m_text.isEmpty())
        return;
    m_textFrom = 0;
    m_textTo = int(m_text.words.size()) - 1;
    notifyTextSelection();
    update();
}

// Wort unter einem Punkt (Bildkoordinaten). `nearest`: sonst das nächstgelegene
// – beim Ziehen über Zwischenräume und Zeilenenden.
int ImageView::wordAt(const QPointF &pt, bool nearest) const
{
    int best = -1;
    double bestDist = 0;
    for (int i = 0; i < m_text.words.size(); ++i) {
        const QRectF b = QRectF(m_text.words.at(i).box).adjusted(-2, -2, 2, 2);
        if (b.contains(pt))
            return i;
        if (!nearest)
            continue;
        // Abstand, vertikal stärker gewichtet: erst die Zeile, dann das Wort
        const double dx = pt.x() < b.left() ? b.left() - pt.x() : pt.x() > b.right() ? pt.x() - b.right() : 0;
        const double dy = pt.y() < b.top() ? b.top() - pt.y() : pt.y() > b.bottom() ? pt.y() - b.bottom() : 0;
        const double d = dx + 4 * dy;
        if (best < 0 || d < bestDist) {
            best = i;
            bestDist = d;
        }
    }
    return best;
}

// --- Auswahl ---------------------------------------------------------------

void ImageView::clearSelection()
{
    if (m_selection.isEmpty())
        return;
    m_selection = QRect();
    Q_EMIT selectionChanged(m_selection);
    update();
}

void ImageView::setSelection(const QRect &rect)
{
    m_selection = rect.intersected(m_image.rect());
    Q_EMIT selectionChanged(m_selection);
    update();
}

void ImageView::selectAll()
{
    if (m_image.isNull())
        return;
    m_selection = m_image.rect();
    Q_EMIT selectionChanged(m_selection);
    update();
}

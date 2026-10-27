// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Icons.h"

#include <QApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyle>

#include <QtMath>

#include <cmath>
#include <functional>

namespace {

using Drawer = std::function<void(QPainter &, const QRectF &)>;

// Zeichnet ein einfaches Strich-Icon in der Textfarbe der Oberfläche,
// passend zu hellen und dunklen Farbschemata und jeder Größe.
class GlyphEngine : public QIconEngine
{
public:
    explicit GlyphEngine(Drawer draw) : m_draw(std::move(draw)) {}

    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        QColor c = QApplication::palette().color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active,
                                                 QPalette::WindowText);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const qreal s = std::min(rect.width(), rect.height());
        QPen pen(c, std::max<qreal>(1.2, s / 12.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        const QRectF box(rect.x() + (rect.width() - s) / 2 + s * 0.12, rect.y() + (rect.height() - s) / 2 + s * 0.12,
                         s * 0.76, s * 0.76);
        m_draw(*p, box);
        p->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(), size), mode, state);
        return pm;
    }

    QIconEngine *clone() const override { return new GlyphEngine(m_draw); }

private:
    Drawer m_draw;
};

void magnifier(QPainter &p, const QRectF &b, const QString &inner)
{
    const QRectF lens(b.left(), b.top(), b.width() * 0.68, b.height() * 0.68);
    p.drawEllipse(lens);
    p.drawLine(QPointF(lens.right() - lens.width() * 0.12, lens.bottom() - lens.height() * 0.12), b.bottomRight());
    const QPointF c = lens.center();
    const qreal r = lens.width() * 0.22;
    if (inner == QLatin1String("+") || inner == QLatin1String("-"))
        p.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
    if (inner == QLatin1String("+"))
        p.drawLine(QPointF(c.x(), c.y() - r), QPointF(c.x(), c.y() + r));
    if (inner == QLatin1String("1")) {
        QFont f = p.font();
        f.setPixelSize(int(lens.height() * 0.45));
        f.setBold(true);
        p.setFont(f);
        p.drawText(lens, Qt::AlignCenter, QStringLiteral("1:1"));
    }
}

void rotateArrow(QPainter &p, const QRectF &b, bool clockwise)
{
    // Bogen über drei Viertel des Kreises, endet oben (rechts drehen: oben
    // rechts, links drehen: oben links); dort eine Pfeilspitze in Laufrichtung.
    const QRectF circle = b.adjusted(b.width() * 0.1, b.height() * 0.14, -b.width() * 0.1, -b.height() * 0.06);
    const qreal end = clockwise ? 60 : 120;          // Qt-Winkel, gegen den Uhrzeigersinn ab 3 Uhr
    const qreal span = clockwise ? -270 : 270;
    QPainterPath arc;
    arc.arcMoveTo(circle, end - span);
    arc.arcTo(circle, end - span, span);
    p.drawPath(arc);

    const qreal rad = qDegreesToRadians(end);
    const QPointF tip = arc.currentPosition();
    // Tangente in Bildschirmkoordinaten (y nach unten) in Laufrichtung
    QPointF dir(-std::sin(rad), -std::cos(rad));
    if (clockwise)
        dir = -dir;
    const QPointF perp(-dir.y(), dir.x());
    const qreal a = b.width() * 0.22;
    QPolygonF head{tip + dir * a, tip + perp * a * 0.65, tip - perp * a * 0.65};
    p.save();
    p.setBrush(p.pen().color());
    p.drawPolygon(head);
    p.restore();
}

const QHash<QString, Drawer> &glyphs()
{
    static const QHash<QString, Drawer> map = {
        {QStringLiteral("zoom-in"), [](QPainter &p, const QRectF &b) { magnifier(p, b, QStringLiteral("+")); }},
        {QStringLiteral("zoom-out"), [](QPainter &p, const QRectF &b) { magnifier(p, b, QStringLiteral("-")); }},
        {QStringLiteral("zoom-original"), [](QPainter &p, const QRectF &b) { magnifier(p, b, QStringLiteral("1")); }},
        {QStringLiteral("zoom-fit-best"), [](QPainter &p, const QRectF &b) {
             const qreal k = b.width() * 0.3;
             for (const QPointF &c : {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight()}) {
                 const qreal sx = c.x() == b.left() ? 1 : -1, sy = c.y() == b.top() ? 1 : -1;
                 p.drawLine(c, c + QPointF(k * sx, 0));
                 p.drawLine(c, c + QPointF(0, k * sy));
             }
             p.drawRect(b.adjusted(b.width() * 0.3, b.height() * 0.3, -b.width() * 0.3, -b.height() * 0.3));
         }},
        {QStringLiteral("view-fullscreen"), [](QPainter &p, const QRectF &b) {
             const qreal k = b.width() * 0.32;
             for (const QPointF &c : {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight()}) {
                 const qreal sx = c.x() == b.left() ? 1 : -1, sy = c.y() == b.top() ? 1 : -1;
                 p.drawLine(c, c + QPointF(k * sx, 0));
                 p.drawLine(c, c + QPointF(0, k * sy));
             }
         }},
        {QStringLiteral("transform-crop"), [](QPainter &p, const QRectF &b) {
             const qreal i = b.width() * 0.22;
             p.drawPolyline(QPolygonF{QPointF(b.left() + i, b.top()), QPointF(b.left() + i, b.bottom() - i), QPointF(b.right(), b.bottom() - i)});
             p.drawPolyline(QPolygonF{QPointF(b.left(), b.top() + i), QPointF(b.right() - i, b.top() + i), QPointF(b.right() - i, b.bottom())});
         }},
        {QStringLiteral("object-rotate-right"), [](QPainter &p, const QRectF &b) { rotateArrow(p, b, true); }},
        {QStringLiteral("object-rotate-left"), [](QPainter &p, const QRectF &b) { rotateArrow(p, b, false); }},
    };
    return map;
}

QStyle::StandardPixmap standardFor(const QString &name)
{
    static const QHash<QString, QStyle::StandardPixmap> map = {
        {QStringLiteral("document-open"), QStyle::SP_DialogOpenButton},
        {QStringLiteral("document-save"), QStyle::SP_DialogSaveButton},
        {QStringLiteral("document-save-as"), QStyle::SP_DialogSaveButton},
        {QStringLiteral("go-next"), QStyle::SP_ArrowRight},
        {QStringLiteral("go-previous"), QStyle::SP_ArrowLeft},
        {QStringLiteral("go-first"), QStyle::SP_MediaSkipBackward},
        {QStringLiteral("go-last"), QStyle::SP_MediaSkipForward},
        {QStringLiteral("edit-delete"), QStyle::SP_TrashIcon},
        {QStringLiteral("document-properties"), QStyle::SP_FileDialogInfoView},
        {QStringLiteral("help-contents"), QStyle::SP_DialogHelpButton},
        {QStringLiteral("help-about"), QStyle::SP_MessageBoxInformation},
        {QStringLiteral("application-exit"), QStyle::SP_DialogCloseButton},
        {QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay},
        {QStringLiteral("media-playback-pause"), QStyle::SP_MediaPause},
        {QStringLiteral("audio-volume-high"), QStyle::SP_MediaVolume},
        {QStringLiteral("audio-volume-muted"), QStyle::SP_MediaVolumeMuted},
        {QStringLiteral("edit-undo"), QStyle::SP_ArrowBack},
    };
    return map.value(name, QStyle::SP_CustomBase);
}

} // namespace

namespace Icons {

QIcon get(const QString &name)
{
    if (name.isEmpty())
        return {};
    if (QIcon::hasThemeIcon(name))
        return QIcon::fromTheme(name);
    if (glyphs().contains(name))
        return QIcon(new GlyphEngine(glyphs().value(name)));
    const QStyle::StandardPixmap sp = standardFor(name);
    if (sp != QStyle::SP_CustomBase && qApp)
        return QApplication::style()->standardIcon(sp);
    return {};
}

} // namespace Icons

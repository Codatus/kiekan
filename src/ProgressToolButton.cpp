// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProgressToolButton.h"

#include <QPainter>

#include <algorithm>

void ProgressToolButton::setProgress(int percent)
{
    percent = percent < 0 ? -1 : std::min(percent, 100);
    if (percent == m_progress)
        return;
    m_progress = percent;
    update();
}

void ProgressToolButton::setDone(bool done)
{
    if (done == m_done)
        return;
    m_done = done;
    update();
}

void ProgressToolButton::paintEvent(QPaintEvent *event)
{
    QToolButton::paintEvent(event);
    if (m_progress < 0 && !m_done)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal side = std::min(width(), height()) - 3.0;
    const QRectF ring(QPointF((width() - side) / 2.0, (height() - side) / 2.0), QSizeF(side, side));
    const QColor accent = palette().color(QPalette::Highlight);
    if (m_progress >= 0) {
        QColor track = palette().color(QPalette::Mid);
        track.setAlpha(120);
        p.setPen(QPen(track, 2.5));
        p.drawEllipse(ring);
        p.setPen(QPen(accent, 2.5, Qt::SolidLine, Qt::RoundCap));
        // oben beginnend im Uhrzeigersinn; mindestens ein kleiner Bogen,
        // damit sofort sichtbar ist, dass etwas läuft
        p.drawArc(ring, 90 * 16, -std::max(5, m_progress) * 360 * 16 / 100);
    } else {
        QColor done = accent;
        done.setAlpha(150);
        p.setPen(QPen(done, 1.5));
        p.drawEllipse(ring);
    }
}

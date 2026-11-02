// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PdfExport.h"

#include <QBuffer>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace PdfExport {

namespace {

// Seitengröße: die Auflösung aus der Datei, wenn sie glaubhaft ist; sonst so,
// dass die lange Seite einer A4-Seite entspricht (Fotos von Dokumenten).
double dotsPerInch(const QImage &img)
{
    const double dpi = img.dotsPerMeterX() * 0.0254;
    if (dpi >= 100 && dpi <= 1200)
        return dpi;
    const double a4Long = 297.0 / 25.4;
    return std::clamp(std::max(img.width(), img.height()) / a4Long, 72.0, 1200.0);
}

} // namespace

TextLayer scaled(const TextLayer &layer, const QSize &to)
{
    if (layer.isEmpty() || !layer.size.isValid() || !to.isValid())
        return {};
    if (layer.size == to)
        return layer;
    const double sx = double(to.width()) / layer.size.width();
    const double sy = double(to.height()) / layer.size.height();
    if (std::abs(sx - sy) > 0.02 * std::max(sx, sy))
        return {};   // anders gedreht oder beschnitten
    TextLayer out = layer;
    out.size = to;
    for (TextWord &w : out.words)
        w.box = QRectF(w.box.x() * sx, w.box.y() * sy, w.box.width() * sx, w.box.height() * sy).toAlignedRect();
    return out;
}

QString write(const QString &path, const QList<Page> &pages, const QString &title)
{
    return write(path, int(pages.size()), [&pages](int i) { return pages.at(i); }, title);
}

QString write(const QString &path, int count, const std::function<Page(int)> &makePage, const QString &title,
              const std::function<bool(int)> &keepGoing)
{
    if (count <= 0)
        return QStringLiteral("no pages");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QStringLiteral("cannot write %1: %2").arg(path, file.errorString());

    // Eigene Einheit: 1/1200 Zoll, damit Seiten beliebiger Auflösung passen.
    constexpr int kUnit = 1200;
    QPdfWriter writer(&file);
    writer.setResolution(kUnit);
    writer.setTitle(title);
    writer.setCreator(QStringLiteral("KiekAn!"));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));

    QPainter painter;
    for (int i = 0; i < count; ++i) {
        if (keepGoing && i > 0 && !keepGoing(i - 1))
            return QStringLiteral("cancelled");
        const Page page = makePage(i);
        if (page.image.isNull())
            continue;
        const double dpi = dotsPerInch(page.image);
        const QSizeF inch(page.image.width() / dpi, page.image.height() / dpi);
        writer.setPageSize(QPageSize(QSizeF(inch.width() * 72.0, inch.height() * 72.0), QPageSize::Point,
                                     QString(), QPageSize::ExactMatch));
        if (painter.isActive()) {
            if (!writer.newPage())
                return QStringLiteral("cannot add page %1").arg(i + 1);
        } else if (!painter.begin(&writer)) {
            return QStringLiteral("cannot write PDF");
        }
        const double scale = kUnit / dpi;   // Bildpixel → Einheiten
        painter.drawImage(QRectF(0, 0, page.image.width() * scale, page.image.height() * scale), page.image);

        // Erkannter Text unsichtbar über den Wörtern, auf Wortbreite gestaucht
        if (!page.text.isEmpty()) {
            painter.save();
            painter.setPen(QColor(0, 0, 0, 0));
            QFont font(QStringLiteral("DejaVu Sans"));
            for (const TextWord &w : page.text.words) {
                if (w.text.isEmpty() || w.box.isEmpty())
                    continue;
                const QRectF box(w.box.x() * scale, w.box.y() * scale, w.box.width() * scale, w.box.height() * scale);
                font.setPixelSize(std::max(1, int(box.height() * 0.85)));
                painter.setFont(font);
                const double natural = std::max(1.0, painter.fontMetrics().horizontalAdvance(w.text) * 1.0);
                painter.save();
                painter.translate(box.left(), box.bottom() - box.height() * 0.15);
                painter.scale(box.width() / natural, 1.0);
                painter.drawText(QPointF(0, 0), w.text);
                painter.restore();
            }
            painter.restore();
        }
    }
    if (!painter.isActive())
        return QStringLiteral("no pages");
    painter.end();
    if (!file.commit())
        return QStringLiteral("cannot write %1: %2").arg(path, file.errorString());
    return {};
}

} // namespace PdfExport

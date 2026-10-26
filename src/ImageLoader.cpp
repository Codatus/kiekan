// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageLoader.h"

#include "Media.h"

#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QRunnable>

#ifdef KIEKAN_HAVE_PDF
#include <QPdfDocument>
#endif

#include <atomic>
#include <cmath>

namespace {
std::atomic<bool> s_autoRotate{true};

constexpr int kPdfMaxSide = 4000;

QImage toDisplayFormat(QImage img)
{
    // In ein Format bringen, das der Raster-Painter ohne Umwandlung zeichnet.
    const QImage::Format target = img.hasAlphaChannel()
        ? QImage::Format_ARGB32_Premultiplied
        : QImage::Format_RGB32;
    if (img.format() != target)
        img = img.convertToFormat(target);
    return img;
}

#ifdef KIEKAN_HAVE_PDF
LoadResult decodePdf(const QString &path, int page, double dpi)
{
    LoadResult r;
    QPdfDocument doc;
    doc.load(path);
    if (doc.status() != QPdfDocument::Status::Ready || doc.pageCount() <= 0) {
        r.error = QObject::tr("PDF kann nicht geöffnet werden");
        return r;
    }
    r.pageCount = doc.pageCount();
    page = std::clamp(page, 0, r.pageCount - 1);
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    const QSizeF points = doc.pagePointSize(page);
#else
    const QSizeF points = doc.pageSize(page);
#endif
    // Punkte (1/72 Zoll) in Pixel bei fester Auflösung, nach oben begrenzt.
    double scale = dpi / 72.0;
    const double longest = std::max(points.width(), points.height()) * scale;
    if (longest > kPdfMaxSide)
        scale *= kPdfMaxSide / longest;
    const QSize size(std::max(1, int(std::lround(points.width() * scale))),
                     std::max(1, int(std::lround(points.height() * scale))));

    const QImage rendered = doc.render(page, size);
    if (rendered.isNull()) {
        r.error = QObject::tr("PDF-Seite kann nicht gerendert werden");
        return r;
    }
    // PDF-Seiten sind Papier: auf weißen Grund legen.
    QImage page_(size, QImage::Format_RGB32);
    page_.fill(Qt::white);
    QPainter p(&page_);
    p.drawImage(0, 0, rendered);
    p.end();
    // Auflösung vermerken – die Texterkennung schätzt daran die Schriftgröße.
    page_.setDotsPerMeterX(int(std::lround(dpi / 0.0254)));
    page_.setDotsPerMeterY(int(std::lround(dpi / 0.0254)));
    r.image = page_;
    return r;
}
#endif

} // namespace

void ImageLoader::setAutoRotate(bool on)
{
    s_autoRotate = on;
}

ImageLoader::ImageLoader(QObject *parent)
    : QObject(parent)
{
    // Mehr als drei parallele Dekodierungen bringen beim Blättern nichts,
    // kosten aber Speicher und bremsen das aktuell angeforderte Bild.
    m_pool.setMaxThreadCount(3);
}

ImageLoader::~ImageLoader()
{
    m_pool.clear();
    m_pool.waitForDone();
}

QString ImageLoader::key(const QString &path, int page)
{
    return page == 0 ? path : path + QLatin1Char('\x1f') + QString::number(page);
}

LoadResult ImageLoader::decode(const QString &path, int page, double pdfDpi, bool fullQuality)
{
#ifdef KIEKAN_HAVE_PDF
    if (Media::isPdf(path))
        return decodePdf(path, page, pdfDpi);
#endif

    LoadResult r;
    QImageReader reader(path);
    reader.setAutoTransform(s_autoRotate);   // EXIF-Ausrichtung beachten
    if (fullQuality)
        reader.setQuality(100);              // RAW: voll entwickeln
    // Format zuerst nach Dateiendung wählen (Qt fällt bei Bedarf auf den
    // Inhalt zurück). Wichtig für Kamera-RAW: CR2, NEF, DNG … sind intern
    // TIFF-Dateien, und der TIFF-Leser würde nur deren Mini-Vorschau zeigen.

    const int count = reader.imageCount();
    if (reader.supportsAnimation() && count != 1) {
        // count ist 0, wenn das Format die Anzahl nicht vorab kennt.
        r.animated = true;
    } else if (count > 1) {
        r.pageCount = count;   // mehrseitiges TIFF
        page = std::clamp(page, 0, count - 1);
        if (page > 0)
            reader.jumpToImage(page);
    }

    const QImage img = reader.read();
    if (img.isNull()) {
        r.error = reader.errorString();
        return r;
    }
    if (r.animated && !reader.canRead())
        r.animated = false;    // doch nur ein Bild
    r.rawPreview = img.text(QStringLiteral("kiekan-raw")) == QLatin1String("preview");
    const QStringList size = img.text(QStringLiteral("kiekan-raw-size")).split(QLatin1Char('x'));
    if (size.size() == 2)
        r.rawFullSize = QSize(size.at(0).toInt(), size.at(1).toInt());
    r.image = toDisplayFormat(img);
    return r;
}

void ImageLoader::request(const QString &path, int page)
{
    const QString k = key(path, page);
    if (path.isEmpty() || m_cache.contains(k) || m_pending.contains(k))
        return;
    m_pending.insert(k);
    if (!m_wanted.isEmpty())
        m_wanted.insert(k);

    // Der Destruktor wartet auf alle laufenden Aufgaben, daher bleibt `this`
    // während der Dekodierung gültig. Das Ergebnis wird über die
    // Ereignisschleife zurückgereicht und verfällt, falls das Objekt bis
    // dahin zerstört wurde.
    m_pool.start(QRunnable::create([this, path, page] {
        LoadResult r = decode(path, page);
        QMetaObject::invokeMethod(this, [this, path, page, r] {
            finish(path, page, r);
        }, Qt::QueuedConnection);
    }));
}

void ImageLoader::finish(const QString &path, int page, const LoadResult &result)
{
    const QString k = key(path, page);
    m_pending.remove(k);
    if (!result.image.isNull() && (m_wanted.isEmpty() || m_wanted.contains(k)))
        m_cache.insert(k, result);
    Q_EMIT loaded(path, page, result);
}

void ImageLoader::retainOnly(const QList<QPair<QString, int>> &keep)
{
    m_wanted.clear();
    for (const auto &p : keep)
        m_wanted.insert(key(p.first, p.second));
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (m_wanted.contains(it.key()))
            ++it;
        else
            it = m_cache.erase(it);
    }
}

void ImageLoader::remove(const QString &path)
{
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it.key() == path || it.key().startsWith(path + QLatin1Char('\x1f')))
            it = m_cache.erase(it);
        else
            ++it;
    }
}

void ImageLoader::clear()
{
    m_cache.clear();
}

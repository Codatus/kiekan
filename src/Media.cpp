// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Media.h"

#include <QFileInfo>
#include <QImageReader>

#ifdef KIEKAN_HAVE_PDF
#include <QPdfDocument>
#endif

namespace Media {

namespace {

const QStringList &videoSuffixes()
{
    static const QStringList list = {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mkv"), QStringLiteral("webm"),
        QStringLiteral("avi"), QStringLiteral("mov"), QStringLiteral("wmv"), QStringLiteral("flv"),
        QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("3gp"), QStringLiteral("ogv"),
        QStringLiteral("ts"), QStringLiteral("mts"), QStringLiteral("m2ts"), QStringLiteral("vob"),
    };
    return list;
}

} // namespace

bool isVideo(const QString &path)
{
#ifdef KIEKAN_HAVE_MPV
    return videoSuffixes().contains(QFileInfo(path).suffix().toLower());
#else
    Q_UNUSED(path);
    return false;
#endif
}

bool isPdf(const QString &path)
{
    return path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
}

QStringList imageNameFilters()
{
    QStringList filters;
    const auto formats = QImageReader::supportedImageFormats();
    for (const QByteArray &fmt : formats)
        filters << QStringLiteral("*.") + QString::fromLatin1(fmt).toLower();
#ifdef KIEKAN_HAVE_PDF
    filters << QStringLiteral("*.pdf");
#endif
    filters.removeDuplicates();
    return filters;
}

QStringList allNameFilters()
{
    QStringList filters = imageNameFilters();
#ifdef KIEKAN_HAVE_MPV
    for (const QString &s : videoSuffixes())
        filters << QStringLiteral("*.") + s;
#endif
    return filters;
}

int pdfPageCount(const QString &path)
{
#ifdef KIEKAN_HAVE_PDF
    QPdfDocument doc;
    doc.load(path);
    return doc.status() == QPdfDocument::Status::Ready ? doc.pageCount() : 0;
#else
    Q_UNUSED(path);
    return 0;
#endif
}

QString pdfPageText(const QString &path, int page)
{
#ifdef KIEKAN_HAVE_PDF
    QPdfDocument doc;
    doc.load(path);
    if (doc.status() != QPdfDocument::Status::Ready || page < 0 || page >= doc.pageCount())
        return {};
    return doc.getAllText(page).text();
#else
    Q_UNUSED(path);
    Q_UNUSED(page);
    return {};
#endif
}

} // namespace Media

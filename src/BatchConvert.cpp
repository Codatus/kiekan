// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BatchConvert.h"

#include "EditOps.h"
#include "ImageLoader.h"
#include "Media.h"
#include "Metadata.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

namespace BatchConvert {

QByteArray writerFormat(const QString &pathOrSuffix)
{
    QString s = QFileInfo(pathOrSuffix).suffix().toLower();
    if (s.isEmpty())
        s = pathOrSuffix.toLower();
    if (s == QLatin1String("jpg") || s == QLatin1String("jpeg"))
        return "jpeg";
    if (s == QLatin1String("tif") || s == QLatin1String("tiff"))
        return "tiff";
    if (s == QLatin1String("png") || s == QLatin1String("webp") || s == QLatin1String("bmp"))
        return s.toLatin1();
    return {};
}

QStringList writableFormats()
{
    const auto supported = QImageWriter::supportedImageFormats();
    QStringList out;
    for (const QString &s : {QStringLiteral("jpg"), QStringLiteral("png"), QStringLiteral("webp"),
                             QStringLiteral("tif"), QStringLiteral("bmp")})
        if (supported.contains(writerFormat(s)))
            out << s;
    return out;
}

QList<Item> plan(const QStringList &inputs, const Options &options, const QStringList &outputNames)
{
    QList<Item> items;
    QSet<QString> produced;
    for (int i = 0; i < inputs.size(); ++i) {
        const QString &in = inputs.at(i);
        if (Media::isVideo(in))
            continue;
        int pages = 1;
        if (Media::isPdf(in)) {
            pages = std::max(1, Media::pdfPageCount(in));
        } else {
            QImageReader probe(in);
            if (!probe.supportsAnimation() && probe.imageCount() > 1)
                pages = probe.imageCount();
        }
        const QString name = outputNames.value(i).isEmpty() ? QFileInfo(in).completeBaseName() : outputNames.at(i);
        for (int page = 0; page < pages; ++page) {
            QString base = name;
            if (pages > 1)
                base += QStringLiteral("_page-%1").arg(page + 1);
            QString out = QDir(options.outputDir).filePath(base + QLatin1Char('.') + options.format);
            if (produced.contains(out))
                out = QDir(options.outputDir).filePath(base + QLatin1Char('_') + QFileInfo(in).suffix().toLower()
                                                       + QLatin1Char('.') + options.format);
            produced.insert(out);
            items.append({in, page, out});
        }
    }
    return items;
}

QString writeImage(const QImage &input, const QString &output, int quality, const QString &metadataSource)
{
    const QByteArray format = writerFormat(output);
    if (format.isEmpty() || !QImageWriter::supportedImageFormats().contains(format))
        return QStringLiteral("unsupported output format: %1").arg(QFileInfo(output).suffix());

    QImage img = input;
    if (img.hasAlphaChannel() && (format == "jpeg" || format == "bmp")) {
        // Formate ohne Transparenz: auf weißen Grund legen statt schwarz.
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, img);
        p.end();
        img = flat;
    }

    Metadata::Snapshot meta;
    if (!metadataSource.isEmpty() && !Media::isPdf(metadataSource) && format != "bmp")
        meta = Metadata::Snapshot::capture(metadataSource);

    QSaveFile file(output);
    if (!file.open(QIODevice::WriteOnly))
        return QStringLiteral("cannot write %1: %2").arg(output, file.errorString());
    QImageWriter writer(&file, format);
    if (format == "jpeg" || format == "webp")
        writer.setQuality(std::clamp(quality, 1, 100));
    if (format == "png")
        writer.setCompression(1);
    if (!writer.write(img) || !file.commit())
        return QStringLiteral("cannot write %1: %2").arg(output, writer.errorString());
    meta.applyTo(output, img.size(), true);
    return {};
}

Outcome convert(const Item &item, const Options &options)
{
    if (QFileInfo::exists(item.output) && !options.overwrite)
        return {Outcome::Skipped, QStringLiteral("exists"), {}};
    if (QFileInfo(item.output).canonicalFilePath() == QFileInfo(item.input).canonicalFilePath())
        return {Outcome::Skipped, QStringLiteral("would replace its own source"), {}};
    if (!QDir().mkpath(QFileInfo(item.output).absolutePath()))
        return {Outcome::Failed, QStringLiteral("cannot create %1").arg(QFileInfo(item.output).absolutePath()), {}};

    const LoadResult r = ImageLoader::decode(item.input, item.page, 200, true);
    if (r.image.isNull())
        return {Outcome::Failed, QStringLiteral("cannot read: %1").arg(r.error), {}};
    QImage img;
    try {
        img = EditOps::apply(r.image, options.operations, nullptr);
    } catch (const EditOps::Error &e) {
        return {Outcome::Failed, e.message, {}};
    }
    if (options.maxSize > 0)
        img = EditOps::scaledToMax(img, options.maxSize);
    const QString error = writeImage(img, item.output, options.quality, options.keepMetadata ? item.input : QString());
    if (!error.isEmpty())
        return {Outcome::Failed, error, {}};
    return {Outcome::Converted, {}, img.size()};
}

} // namespace BatchConvert

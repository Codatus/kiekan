// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EditOps.h"

#include "FilterRegistry.h"
#include "ImageOps.h"

#include <QJsonArray>
#include <QTransform>

#include <cmath>

namespace EditOps {

namespace {

[[noreturn]] void fail(const QString &message)
{
    throw Error{message};
}

int integer(const QJsonObject &args, const char *key, int def)
{
    const QJsonValue v = args.value(QLatin1String(key));
    return v.isDouble() ? int(std::lround(v.toDouble())) : def;
}

} // namespace

QImage scaledToMax(const QImage &image, int maxSide)
{
    if (image.width() <= maxSide && image.height() <= maxSide)
        return image;
    return image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}



QImage applyOperation(const QImage &image, const QJsonObject &op)
{
    const QString name = op.value(QLatin1String("op")).toString();
    using namespace ImageOps;

    if (name == QLatin1String("rotate")) {
        const int deg = integer(op, "degrees", 90);
        if (deg % 90 != 0)
            fail(QStringLiteral("rotate: degrees must be a multiple of 90"));
        return image.transformed(QTransform().rotate(deg));
    }
    if (name == QLatin1String("flip")) {
        const bool horizontal = op.value(QLatin1String("direction")).toString(QStringLiteral("horizontal")) == QLatin1String("horizontal");
        return image.mirrored(horizontal, !horizontal);
    }
    if (name == QLatin1String("crop")) {
        const QRect r = QRect(integer(op, "x", 0), integer(op, "y", 0), integer(op, "width", 0), integer(op, "height", 0))
                            .intersected(image.rect());
        if (r.isEmpty())
            fail(QStringLiteral("crop: rectangle is empty or outside the image (%1x%2)").arg(image.width()).arg(image.height()));
        return image.copy(r);
    }
    if (name == QLatin1String("resize")) {
        QSize target = image.size();
        const int w = integer(op, "width", 0), h = integer(op, "height", 0);
        const double pct = op.value(QLatin1String("percent")).toDouble(0);
        const int maxSize = integer(op, "max_size", 0);
        if (w > 0 && h > 0)
            target = QSize(w, h);
        else if (w > 0)
            target = QSize(w, std::max(1, int(std::lround(double(image.height()) * w / image.width()))));
        else if (h > 0)
            target = QSize(std::max(1, int(std::lround(double(image.width()) * h / image.height()))), h);
        else if (pct > 0)
            target = QSize(std::max(1, int(std::lround(image.width() * pct / 100))), std::max(1, int(std::lround(image.height() * pct / 100))));
        else if (maxSize > 0)
            return scaledToMax(image, maxSize);
        else
            fail(QStringLiteral("resize: give width, height, percent or max_size"));
        if (target.width() > 40000 || target.height() > 40000)
            fail(QStringLiteral("resize: target too large"));
        return image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (name == QLatin1String("grayscale"))
        return forDisplay(grayscale(forProcessing(image)));
    if (name == QLatin1String("invert"))
        return forDisplay(invert(forProcessing(image)));
    if (name == QLatin1String("auto_levels") || name == QLatin1String("equalize")) {
        const QImage src = forProcessing(image);
        const Histogram h = ImageOps::histogram(src);
        Lut r, g, b;
        if (name == QLatin1String("auto_levels")) {
            autoLevelsLuts(h, 0.005, r, g, b);
        } else {
            r = g = b = equalizationLut(h);
        }
        return forDisplay(applyLuts(src, r, g, b));
    }
    if (name == QLatin1String("curves")) {
        QVector<QPointF> pts;
        for (const QJsonValue &v : op.value(QLatin1String("points")).toArray()) {
            const QJsonArray pair = v.toArray();
            if (pair.size() == 2)
                pts.append(QPointF(std::clamp(pair.at(0).toDouble(), 0.0, 255.0), std::clamp(pair.at(1).toDouble(), 0.0, 255.0)));
        }
        if (pts.size() < 2)
            fail(QStringLiteral("curves: need at least two points [[in,out],...]"));
        std::sort(pts.begin(), pts.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
        const Lut lut = curveLut(pts);
        const Lut id = identityLut();
        const QString ch = op.value(QLatin1String("channel")).toString(QStringLiteral("all"));
        const QImage src = forProcessing(image);
        if (ch == QLatin1String("red"))
            return forDisplay(applyLuts(src, lut, id, id));
        if (ch == QLatin1String("green"))
            return forDisplay(applyLuts(src, id, lut, id));
        if (ch == QLatin1String("blue"))
            return forDisplay(applyLuts(src, id, id, lut));
        return forDisplay(applyLuts(src, lut, lut, lut));
    }
    if (name == QLatin1String("filter")) {
        const QString id = op.value(QLatin1String("id")).toString();
        const Kiekan::Filter *filter = FilterRegistry::instance().find(id);
        if (!filter)
            fail(QStringLiteral("unknown filter '%1' (see list_filters)").arg(id));
        return FilterRegistry::run(filter, image, op.value(QLatin1String("params")).toObject().toVariantMap());
    }
    fail(QStringLiteral("unknown operation '%1'").arg(name));
}

QImage apply(QImage image, const QJsonArray &ops, QStringList *log)
{
    for (const QJsonValue &v : ops) {
        if (!v.isObject())
            fail(QStringLiteral("each operation must be an object like {\"op\":\"rotate\",\"degrees\":90}"));
        image = applyOperation(image, v.toObject());
        if (log)
            log->append(QStringLiteral("%1 -> %2x%3").arg(v.toObject().value(QLatin1String("op")).toString())
                            .arg(image.width()).arg(image.height()));
    }
    return image;
}


} // namespace EditOps

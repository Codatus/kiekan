// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LosslessJpeg.h"

#include "Metadata.h"

#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QSaveFile>

#include <array>

#ifdef KIEKAN_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

namespace LosslessJpeg {

namespace {

// Lage als 2×2-Matrix auf (x, y) mit y nach unten, Index = EXIF-Wert − 1.
using Matrix = std::array<int, 4>;
constexpr std::array<Matrix, 8> kMatrices{{
    {1, 0, 0, 1},     // None
    {-1, 0, 0, 1},    // FlipH
    {-1, 0, 0, -1},   // Rot180
    {1, 0, 0, -1},    // FlipV
    {0, 1, 1, 0},     // Transpose
    {0, -1, 1, 0},    // Rot90
    {0, -1, -1, 0},   // Transverse
    {0, 1, -1, 0},    // Rot270
}};

Matrix matrix(Op op) { return kMatrices[int(op) - 1]; }

// Ausrichtung, die Qt beim Laden anwendet (entspricht der EXIF-Angabe).
Op fileOrientation(const QString &path)
{
    QImageReader reader(path);
    const QImageIOHandler::Transformations t = reader.transformation();
    Op op = Op::None;
    if (t & QImageIOHandler::TransformationMirror)
        op = compose(op, Op::FlipH);
    if (t & QImageIOHandler::TransformationFlip)
        op = compose(op, Op::FlipV);
    if (t & QImageIOHandler::TransformationRotate90)
        op = compose(op, Op::Rot90);
    return op;
}

#ifdef KIEKAN_HAVE_TURBOJPEG
// Blöcke umordnen; nur wenn das ohne Abschneiden am Rand geht.
bool transformPixels(QByteArray &data, Op op, QString *error)
{
    int xop = TJXOP_NONE;
    switch (op) {
    case Op::None: return true;
    case Op::FlipH: xop = TJXOP_HFLIP; break;
    case Op::FlipV: xop = TJXOP_VFLIP; break;
    case Op::Transpose: xop = TJXOP_TRANSPOSE; break;
    case Op::Transverse: xop = TJXOP_TRANSVERSE; break;
    case Op::Rot90: xop = TJXOP_ROT90; break;
    case Op::Rot180: xop = TJXOP_ROT180; break;
    case Op::Rot270: xop = TJXOP_ROT270; break;
    }
    tjhandle handle = tjInitTransform();
    if (!handle) {
        *error = QStringLiteral("libjpeg-turbo");
        return false;
    }
    tjtransform xf{};
    xf.op = xop;
    xf.options = TJXOPT_PERFECT;   // Markierungen (EXIF, ICC) werden mitkopiert
    unsigned char *out = nullptr;
    unsigned long outSize = 0;
    const int rc = tjTransform(handle, reinterpret_cast<const unsigned char *>(data.constData()),
                               static_cast<unsigned long>(data.size()), 1, &out, &outSize, &xf, 0);
    if (rc == 0)
        data = QByteArray(reinterpret_cast<const char *>(out), qsizetype(outSize));
    else
        *error = QString::fromLocal8Bit(tjGetErrorStr2(handle));
    tjFree(out);
    tjDestroy(handle);
    return rc == 0;
}
#endif

} // namespace

Op compose(Op first, Op then)
{
    const Matrix a = matrix(first);
    const Matrix b = matrix(then);
    const Matrix m{b[0] * a[0] + b[1] * a[2], b[0] * a[1] + b[1] * a[3],
                   b[2] * a[0] + b[3] * a[2], b[2] * a[1] + b[3] * a[3]};
    for (int i = 0; i < 8; ++i)
        if (kMatrices[i] == m)
            return Op(i + 1);
    return Op::None;   // nicht erreichbar: die acht Lagen bilden eine Gruppe
}

bool swapsSides(Op op)
{
    return matrix(op)[0] == 0;
}

bool pixelTransformAvailable()
{
#ifdef KIEKAN_HAVE_TURBOJPEG
    return true;
#else
    return false;
#endif
}

bool isJpeg(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg") || suffix == QLatin1String("jpe")
        || suffix == QLatin1String("jfif");
}

Result apply(const QString &source, const QString &target, Op op, bool autoRotate, QString *error)
{
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        *error = in.errorString();
        return Result::Failed;
    }
    QByteArray data = in.readAll();
    in.close();

    // Die Anzeige zeigt die Datei mit ihrer EXIF-Ausrichtung; gespeichert
    // werden soll, was der Benutzer sieht.
    const Op stored = autoRotate ? fileOrientation(source) : Op::None;
    const Op total = compose(stored, op);

    Result result = Result::Failed;
    QString why;
#ifdef KIEKAN_HAVE_TURBOJPEG
    QByteArray pixels = data;
    if (transformPixels(pixels, total, &why)) {
        // Pixel liegen jetzt richtig; eine vorhandene Ausrichtung zurücksetzen.
        QString metaError;
        if (stored == Op::None || Metadata::setOrientation(pixels, 1, swapsSides(total), &metaError)) {
            data = pixels;
            result = Result::Pixels;
        } else {
            why = metaError;
        }
    }
#endif
    if (result == Result::Failed) {
        // Maße passen nicht zu den 8×8/16×16-Blöcken (oder kein libjpeg-turbo):
        // nur die Ausrichtung ändern – die Bilddaten bleiben unberührt.
        const Op tag = autoRotate ? total : compose(fileOrientation(source), op);
        QString metaError;
        if (Metadata::setOrientation(data, int(tag), false, &metaError)) {
            result = Result::Tag;
        } else {
            *error = why.isEmpty() ? metaError : why + QStringLiteral("; ") + metaError;
            return Result::Failed;
        }
    }

    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
        *error = out.errorString();
        return Result::Failed;
    }
    return result;
}

} // namespace LosslessJpeg

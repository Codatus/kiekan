// KiekAn! – Test: JPEG verlustfrei drehen/spiegeln
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LosslessJpeg.h"
#include "Metadata.h"

#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QTemporaryDir>
#include <QTransform>

#include <cstdio>
#include <cstdlib>

using LosslessJpeg::Op;

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "OK    " : "FEHLER", what);
    if (!ok)
        ++failures;
}

static QImage pattern(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.fillRect(0, 0, w / 2, h / 3, Qt::red);           // oben links rot
    p.fillRect(w / 2, h * 2 / 3, w - w / 2, h - h * 2 / 3, Qt::blue);   // unten rechts blau
    return img;
}

// Mittlere Abweichung zweier gleich großer Bilder (0..255)
static double diff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return 1e9;
    double sum = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            sum += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
        }
    return sum / (3.0 * a.width() * a.height());
}

static QImage load(const QString &path)
{
    QImageReader r(path);
    r.setAutoTransform(true);
    return r.read().convertToFormat(QImage::Format_RGB32);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Metadata::initialize();

    // Verknüpfung der acht Lagen
    check(LosslessJpeg::compose(Op::Rot90, Op::Rot90) == Op::Rot180, "90° + 90° = 180°");
    check(LosslessJpeg::compose(Op::Rot90, Op::Rot270) == Op::None, "90° + 270° = keine Drehung");
    check(LosslessJpeg::compose(Op::FlipH, Op::FlipH) == Op::None, "zweimal spiegeln = nichts");
    check(LosslessJpeg::compose(Op::Rot90, Op::FlipH) == Op::Transpose, "90° dann horizontal = Transponieren");
    check(LosslessJpeg::compose(Op::FlipH, Op::FlipV) == Op::Rot180, "horizontal + vertikal = 180°");

    QTemporaryDir tmp;
    const QString passend = tmp.filePath(QStringLiteral("passend.jpg"));    // 64×48: Vielfache von 16
    const QImage orig = pattern(64, 48);
    orig.save(passend, "JPEG", 95);
    const QImage before = load(passend);
    QString error;
    const auto r1 = LosslessJpeg::apply(passend, tmp.filePath(QStringLiteral("r90.jpg")), Op::Rot90, true, &error);
    const QImage r90 = load(tmp.filePath(QStringLiteral("r90.jpg")));
    if (LosslessJpeg::pixelTransformAvailable()) {
        check(r1 == LosslessJpeg::Result::Pixels, "64×48: Blöcke umgeordnet");
        check(QImageReader(tmp.filePath(QStringLiteral("r90.jpg"))).size() == QSize(48, 64), "gespeicherte Maße gedreht");
    } else {
        check(r1 == LosslessJpeg::Result::Tag, "ohne libjpeg-turbo: EXIF-Ausrichtung");
    }
    const double d = diff(r90, before.transformed(QTransform().rotate(90)));
    std::printf("        Abweichung zum gedrehten Original: %.2f\n", d);
    check(d < 1.0, "Inhalt entspricht dem gedrehten Bild (keine Neukodierung)");

    // Maße passen nicht zu den Blöcken: nur die Ausrichtung ändern
    const QString krumm = tmp.filePath(QStringLiteral("krumm.jpg"));
    pattern(70, 50).save(krumm, "JPEG", 95);
    const QByteArray bytesBefore = [&] { QFile f(krumm); f.open(QIODevice::ReadOnly); return f.readAll(); }();
    const QImage krummBefore = load(krumm);
    const auto r2 = LosslessJpeg::apply(krumm, krumm, Op::Rot270, true, &error);
    check(r2 == LosslessJpeg::Result::Tag, "70×50: nur EXIF-Ausrichtung");
    check(QImageReader(krumm).size() == QSize(70, 50), "Bilddaten unverändert");
    check(diff(load(krumm), krummBefore.transformed(QTransform().rotate(-90))) < 0.01, "Anzeige gedreht, pixelgenau");

    // Datei mit Ausrichtung 6: angezeigt hochkant; nochmals 90° nach rechts
    const QString exif = tmp.filePath(QStringLiteral("exif.jpg"));
    orig.save(exif, "JPEG", 95);
    {
        QFile f(exif);
        f.open(QIODevice::ReadOnly);
        QByteArray data = f.readAll();
        f.close();
        Metadata::setOrientation(data, 6, false);
        f.open(QIODevice::WriteOnly);
        f.write(data);
    }
    const QImage shown = load(exif);   // 48×64
    const auto r3 = LosslessJpeg::apply(exif, exif, Op::Rot90, true, &error);
    check(r3 != LosslessJpeg::Result::Failed, "Datei mit EXIF-Drehung weiterdrehen");
    const QImage after = load(exif);
    check(diff(after, shown.transformed(QTransform().rotate(90))) < 1.0, "Ergebnis = Angezeigtes, um 90° gedreht");

    std::printf("\n%s\n", failures ? "Fehler gefunden" : "alle Prüfungen bestanden");
    return failures ? 1 : 0;
}

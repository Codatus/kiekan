// KiekAn! – Filtertest
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wendet jeden Filter mit Standardwerten auf ein Bild an, prüft Grundregeln
// (Größe, kein Absturz) und misst die Laufzeit. Mit Ausgabeordner werden die
// Ergebnisse als PNG gespeichert:  filtertest bild.jpg [ausgabeordner]

#include "FilterRegistry.h"
#include "ImageOps.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>

#include <cstdio>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();

    QImage input;
    if (args.size() > 1) {
        input.load(args.at(1));
    } else {
        // Synthetisches Testbild mit Verläufen und Transparenz
        input = QImage(640, 480, QImage::Format_ARGB32);
        for (int y = 0; y < input.height(); ++y)
            for (int x = 0; x < input.width(); ++x)
                input.setPixel(x, y, qRgba(x * 255 / 640, y * 255 / 480, 128, x < 40 ? 0 : 255));
    }
    if (input.isNull()) {
        std::fprintf(stderr, "Bild nicht lesbar\n");
        return 2;
    }
    const QString outDir = args.size() > 2 ? args.at(2) : QString();
    if (!outDir.isEmpty())
        QDir().mkpath(outDir);

    FilterRegistry &reg = FilterRegistry::instance();
    reg.loadPlugins();
    QList<const Kiekan::Filter *> all = reg.adjustments();
    all += reg.effects();

    const QImage src = ImageOps::forDisplay(input);
    int failures = 0;
    for (const Kiekan::Filter *f : all) {
        QElapsedTimer t;
        t.start();
        const QImage out = FilterRegistry::run(f, src, {});
        const qint64 ms = t.elapsed();
        const bool sizeOk = f->id().startsWith(QLatin1String("beispiel.")) || out.size() == src.size();
        if (out.isNull() || !sizeOk) {
            ++failures;
            std::printf("FEHLER  %-28s\n", qPrintable(f->id()));
            continue;
        }
        std::printf("ok      %-28s %5lld ms\n", qPrintable(f->id()), ms);
        if (!outDir.isEmpty())
            out.save(QDir(outDir).filePath(f->id() + QStringLiteral(".png")));
    }

    // Einzeloperationen ohne Filterobjekt
    QElapsedTimer t;
    t.start();
    const ImageOps::Histogram h = ImageOps::histogram(src);
    std::printf("ok      %-28s %5lld ms\n", "histogram", t.elapsed());
    const ImageOps::Lut eq = ImageOps::equalizationLut(h);
    const QImage equalized = ImageOps::applyLuts(ImageOps::forProcessing(src), eq, eq, eq);
    if (!outDir.isEmpty())
        equalized.save(QDir(outDir).filePath(QStringLiteral("equalize.png")));

    // Identität: Farben mit Standardwerten dürfen nichts verändern.
    const QImage same = FilterRegistry::run(reg.find(QStringLiteral("farben")), src, {});
    if (same.convertToFormat(QImage::Format_ARGB32) != src.convertToFormat(QImage::Format_ARGB32)) {
        ++failures;
        std::printf("FEHLER  farben mit Standardwerten verändert das Bild\n");
    }

    std::printf("%s\n", failures ? "TEST FEHLGESCHLAGEN" : "alle Filter ok");
    return failures ? 1 : 0;
}

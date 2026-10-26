// Kiek an – Test der Bildformat-Plugins
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later
//
// formattest [--out ordner] datei…
// Lädt jede Datei wie Kiek an (über die Plugins im Build-Ordner) und gibt
// Format, Größe, Seitenzahl, Ladezeit und die Farbe der Bildmitte aus.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>

#include <cstdio>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath() + QStringLiteral("/qtplugins"));
    QImageReader::setAllocationLimit(4096);

    QStringList args = app.arguments().mid(1);
    QString outDir;
    if (args.size() >= 2 && args.first() == QLatin1String("--out")) {
        outDir = args.at(1);
        args = args.mid(2);
        QDir().mkpath(outDir);
    }
    int failures = 0;
    for (const QString &path : args) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize header = reader.size();
        const int count = reader.imageCount();
        QElapsedTimer t;
        t.start();
        const QImage img = reader.read();
        const qint64 ms = t.elapsed();
        if (img.isNull()) {
            ++failures;
            std::printf("FEHLER %-34s %s\n", qPrintable(QFileInfo(path).fileName()), qPrintable(reader.errorString()));
            continue;
        }
        const QRgb c = img.pixel(img.width() / 2, img.height() / 2);
        std::printf("ok     %-34s %-5s %5dx%-5d Kopf %5dx%-5d Seiten %d  Mitte #%06x  Alpha %s  %4lld ms\n",
                    qPrintable(QFileInfo(path).fileName()), reader.format().constData(), img.width(), img.height(),
                    header.width(), header.height(), count, c & 0xffffff, img.hasAlphaChannel() ? "ja " : "nein", ms);
        if (!outDir.isEmpty())
            img.save(QDir(outDir).filePath(QFileInfo(path).fileName() + QStringLiteral(".png")));
    }
    return failures ? 1 : 0;
}

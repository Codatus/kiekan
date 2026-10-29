// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QJsonArray>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

// Stapelkonvertierung: gemeinsam genutzt vom Dialog „Stapelverarbeitung“ und
// dem KI-Werkzeug convert_batch.
namespace BatchConvert {

struct Options
{
    QString format = QStringLiteral("jpg");   // jpg, png, webp, tif, bmp
    QString outputDir;
    int quality = 90;
    int maxSize = 0;                          // längste Seite, 0 = unverändert
    bool overwrite = false;
    bool keepMetadata = true;
    QJsonArray operations;                    // wie edit_image (EditOps)
};

struct Item
{
    QString input;
    int page = 0;        // Seite bei PDF, DjVu, mehrseitigem TIFF
    QString output;
};

struct Outcome
{
    enum Status { Converted, Skipped, Failed } status = Failed;
    QString message;     // Grund bei Skipped/Failed
    QSize size;
};

// Schreibformat zur Endung („jpg“ → "jpeg"); leer, wenn nicht schreibbar.
QByteArray writerFormat(const QString &pathOrSuffix);
QStringList writableFormats();

// Zieldateien festlegen: Seiten mehrseitiger Dokumente werden zu name_page-N,
// gleichnamige Quellen (bild.png, bild.heic) bekommen die Quellendung angehängt.
// `outputNames`: optional eigene Namen ohne Endung je Eingabe (Umbenennen).
QList<Item> plan(const QStringList &inputs, const Options &options, const QStringList &outputNames = {});

// Eine Datei umwandeln (thread-sicher).
Outcome convert(const Item &item, const Options &options);

// Bild atomar schreiben, Metadaten der Quelle übernehmen. Gibt die
// Fehlermeldung zurück (leer bei Erfolg).
QString writeImage(const QImage &image, const QString &output, int quality, const QString &metadataSource);

} // namespace BatchConvert

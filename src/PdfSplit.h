// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

// PDFs in Teile zerlegen (z. B. für Upload-Grenzen wie 30 MB bei Claude)
// und die Teilung nachvollziehbar dokumentieren:
//  – Dateinamen mit Teil- und Seitenangabe,
//  – Titel/Stichwörter jedes Teils nennen Quelle und Seitenbereich,
//  – Seitenzahlen der Teile entsprechen dem Original (PDF-Seitenbeschriftung),
//  – Protokoll als JSON (maschinenlesbar) und Markdown (für Menschen).
namespace PdfSplit {

bool available();

struct Options
{
    double maxMegabytes = 25;            // Größe je Teil (1 MB = 1 000 000 Byte)
    int pagesPerPart = 0;                // > 0: feste Seitenzahl statt Größe
    QList<QPair<int, int>> ranges;       // 1-basiert, einschließlich; hat Vorrang
    bool preferBookmarks = true;         // möglichst an Lesezeichen schneiden
    QString outputDir;                   // leer: <Ordner>/<Name>_Teile
    bool overwrite = false;
};

struct Bookmark
{
    QString title;
    int page = 0;    // 1-basiert im Original
    int level = 0;
};

struct Part
{
    QString file;
    int number = 0;
    int firstPage = 0;   // 1-basiert im Original
    int lastPage = 0;
    qint64 bytes = 0;
    QString sha256;
    bool oversize = false;   // einzelne Seite größer als die Grenze
    QList<Bookmark> bookmarks;
};

struct Result
{
    bool ok = false;
    QString error;
    QString source;
    QString manifestJson;
    QString manifestMarkdown;
    int sourcePages = 0;
    qint64 sourceBytes = 0;
    QList<Part> parts;
    QStringList warnings;
};

// Teilt die PDF; `progress(text)` meldet Zwischenstände.
Result split(const QString &path, const Options &options,
             const std::function<void(const QString &)> &progress = {});

// Seitenzahl und Lesezeichen einer PDF (für Vorschau/Planung).
int pageCount(const QString &path, QString *error = nullptr);

// Protokoll zu einer Datei finden: `path` ist das Protokoll selbst, ein Teil,
// die Quelldatei oder ein Ordner. Leer, wenn keins gefunden wurde.
QString findManifest(const QString &path);
QJsonObject loadManifest(const QString &manifestPath, QString *error = nullptr);

// Prüft Vorhandensein und Prüfsummen der Teile sowie die Quelldatei.
QStringList verify(const QJsonObject &manifest, const QString &manifestPath);

// Informationen eines Teils, aus dessen eigenen PDF-Angaben gelesen
// (funktioniert auch ohne Protokoll). part == 0: kein Kiek-an-Teil.
struct PartInfo
{
    int part = 0;
    int of = 0;
    int firstPage = 0;
    int lastPage = 0;
    QString source;
};
PartInfo partInfo(const QString &pdfPath);

QString sha256(const QString &path);

} // namespace PdfSplit

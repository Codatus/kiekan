// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QSize>
#include <QString>

#include <memory>
#include <optional>

#include <QStringList>

// EXIF/IPTC/XMP lesen und beim Speichern übernehmen (über exiv2).
// Ohne exiv2 gebaut, liefern alle Funktionen leere Ergebnisse.
namespace Metadata {

struct Entry
{
    QString group;   // „EXIF“, „IPTC“, „XMP“
    QString key;     // z. B. Exif.Photo.FNumber
    QString label;   // lesbarer Name
    QString value;   // lesbarer Wert
};

struct Summary
{
    QString camera;
    QString lens;
    QString exposure;
    QString aperture;
    QString iso;
    QString focalLength;
    QString dateTaken;
    bool hasGps = false;
    double latitude = 0;
    double longitude = 0;
    double altitude = 0;
    bool hasAltitude = false;

    // Beschreibende Angaben (XMP, ersatzweise IPTC/EXIF)
    QString title;
    QString description;
    QStringList keywords;
    int rating = -1;   // 0..5, -1 = nicht gesetzt

    bool hasDescriptive() const { return !title.isEmpty() || !description.isEmpty() || !keywords.isEmpty() || rating >= 0; }
    bool isEmpty() const { return camera.isEmpty() && dateTaken.isEmpty() && exposure.isEmpty() && !hasGps && !hasDescriptive(); }
};

bool available();
void initialize();

QList<Entry> read(const QString &path, QString *error = nullptr);
Summary summary(const QString &path);

// Beschreibende Angaben schreiben; nur gesetzte Felder werden geändert.
// Die Bilddaten bleiben unberührt (nur die Metadaten der Datei ändern sich).
struct DescriptiveFields
{
    std::optional<QString> title;
    std::optional<QString> description;
    std::optional<QStringList> keywords;   // ersetzt die vorhandenen
    std::optional<int> rating;             // 0..5
};
bool canWrite(const QString &path);
bool writeDescriptive(const QString &path, const DescriptiveFields &fields, QString *error = nullptr);

// Hält die Metadaten einer Datei fest, bevor sie überschrieben wird, und
// schreibt sie anschließend in die neue Datei.
class Snapshot
{
public:
    Snapshot();
    ~Snapshot();
    Snapshot(Snapshot &&) noexcept;
    Snapshot &operator=(Snapshot &&) noexcept;

    static Snapshot capture(const QString &path);
    bool isEmpty() const;

    // `size`: neue Pixelmaße; `resetOrientation`: Pixel sind bereits richtig
    // gedreht, also Ausrichtung auf „normal“ setzen. Gibt false bei Fehlern.
    bool applyTo(const QString &path, const QSize &size, bool resetOrientation, QString *error = nullptr) const;

private:
    struct Data;
    std::unique_ptr<Data> d;
};

} // namespace Metadata

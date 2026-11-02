// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

// Über Ordnergrenzen hinweg blättern. Alle Ordner bilden eine feste Reihe wie
// im Dateimanager: in jedem Ordner erst die Unterordner, dann die Dateien,
// beides in natürlicher Reihenfolge. Vorwärts und rückwärts laufen dieselbe
// Reihe in entgegengesetzter Richtung ab, hin und zurück führt also wieder
// zum Ausgangsbild. Versteckte Ordner und Ordner-Verknüpfungen (Schleifen)
// werden übersprungen.
namespace FolderWalk {

struct Result {
    QString dir;          // Ordner mit dem nächsten Bild; leer: nichts mehr
    QStringList files;    // seine Dateien, natürlich sortiert
    int index = -1;       // das nächste Bild darin (erstes bzw. letztes)
    bool gaveUp = false;  // zu viele Ordner ohne Bilder durchsucht
};

// Nächster Ordner mit Bildern nach (forward) bzw. vor `dir`, nicht oberhalb
// von `top`. `nameFilters` wählt die Dateien (wie beim Einlesen eines Ordners).
Result neighbour(const QString &dir, bool forward, const QStringList &nameFilters, const QString &top,
                 int maxDirs = 3000);

// Wie weit nach oben gesucht wird: im persönlichen Ordner bis zu dessen
// Unterordner, in dem `dir` liegt (z. B. ~/Bilder – nicht weiter in ~/Downloads),
// sonst bis zur Wurzel des Laufwerks.
QString topFor(const QString &dir);

// Dateien eines Ordners, natürlich sortiert (wie beim Einlesen in KiekAn!)
QStringList files(const QString &dir, const QStringList &nameFilters);

} // namespace FolderWalk

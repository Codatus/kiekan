// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// Stapel-Umbenennen nach einem Muster, mit Vorschau und Rücknahme.
//
// Platzhalter im Muster:
//   ###   laufende Nummer, so viele Stellen wie Rauten (001, 002 …)
//   $N    bisheriger Name (ohne Endung)
//   $D    Aufnahmedatum (EXIF), sonst Änderungsdatum: 2026-10-29
//   $T    Uhrzeit dazu: 14-05-33
//   $F    Name des Ordners
// Die Endung bleibt erhalten (auf Wunsch kleingeschrieben).
namespace BatchRename {

enum class Sort { Name, DateTaken, Modified };

struct Options
{
    QString pattern = QStringLiteral("$N");
    int start = 1;
    int step = 1;
    Sort sort = Sort::Name;
    bool lowercaseSuffix = true;
};

struct Entry
{
    QString source;      // vollständiger Pfad
    QString target;      // vollständiger Pfad
    QString problem;     // leer = in Ordnung; sonst Grund (Doppelung, ungültiger Name …)
    bool unchanged() const { return source == target; }
};

// Aufnahmedatum (EXIF), ersatzweise Änderungszeit der Datei.
QDateTime dateTaken(const QString &path);

// Dateien in der gewünschten Reihenfolge, mit neuen Namen und Prüfung.
QList<Entry> preview(const QStringList &files, const Options &options);

// Nur den Namen erzeugen (ohne Ordner und Endung) – auch für die Konvertierung.
QString expand(const QString &pattern, const QString &path, int number, const QDateTime &taken);

struct Result
{
    QList<QPair<QString, QString>> renamed;   // alt → neu, für die Rücknahme
    QStringList errors;
};

// Benennt in zwei Schritten um (erst Zwischennamen), damit Tauschungen wie
// a→b, b→a gelingen. Einträge mit Problem werden übersprungen.
Result run(const QList<Entry> &entries);

// Macht eine Umbenennung rückgängig.
Result undo(const Result &done);

} // namespace BatchRename

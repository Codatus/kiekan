// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QList>
#include <QRect>
#include <QStringList>

// Texterkennung über Tesseract mit automatischer Wahl des Sprachpakets:
//  1. Schrift und Lage (OSD) bestimmen, gedrehte Seiten aufrichten.
//  2. Probe-Erkennung auf einem Streifen; die Sprache des Textes wird an
//     Füllwörtern erkannt – auch dann, wenn ihr Sprachpaket fehlt.
//  3. Passende installierte Pakete gegeneinander antreten lassen
//     (Erkennungssicherheit + Füllwörter), bei Mischtexten zwei kombinieren.
namespace Ocr {

bool available();

struct Installed
{
    QStringList languages;   // Tesseract-Codes, z. B. "deu", "eng", "deu_latf"
    bool osd = false;        // Lage-/Schrifterkennung verfügbar
};
Installed installed();

struct Word
{
    QString text;
    int confidence = 0;
    QRect box;   // in Koordinaten des übergebenen Bildes
};

struct Candidate
{
    QString language;
    double confidence = 0;     // mittlere Erkennungssicherheit 0..100
    double stopwords = 0;      // Anteil typischer Füllwörter 0..1
    double score = 0;
};

struct Options
{
    QString languages = QStringLiteral("auto");   // oder z. B. "deu+eng"
    bool words = false;                            // Wortliste mit Rahmen
    int dpi = 0;                                   // 0 = aus dem Bild / 300
};

struct Result
{
    bool ok = false;
    QString error;
    QString text;
    QString languages;          // tatsächlich verwendet, z. B. "deu" oder "deu+eng"
    double confidence = 0;      // mittlere Sicherheit 0..100
    int rotation = 0;           // im Uhrzeigersinn angewandte Drehung zum Aufrichten
    QString script;             // laut OSD, z. B. "Latin", "Fraktur"
    QString textLanguage;       // an Füllwörtern erkannte Sprache (Tesseract-Code)
    QList<Candidate> ranking;   // bei automatischer Wahl
    QStringList hints;          // z. B. fehlende Sprachpakete
    QList<Word> words;
};

Result recognize(const QImage &image, const Options &options = {});

// Anzeigename für einen Tesseract-Code („deu“ → „Deutsch“).
QString languageName(const QString &code);
// Paketname zum Nachinstallieren unter Debian/Ubuntu.
QString packageName(const QString &code);

} // namespace Ocr

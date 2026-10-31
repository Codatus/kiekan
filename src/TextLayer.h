// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QRect>
#include <QSize>
#include <QString>

// Erkannter Text eines Bildes: Wörter mit Rahmen in Bildkoordinaten, in
// Lesereihenfolge. Grundlage für das Markieren und Kopieren im Fenster und
// wird in den Metadaten der Datei gespeichert.
struct TextWord
{
    QRect box;
    QString text;
    int line = 0;   // fortlaufende Zeilennummer
    int para = 0;   // fortlaufende Absatznummer
};

struct TextLayer
{
    QSize size;          // Bildmaße, zu denen die Rahmen gehören
    QString languages;   // z. B. "deu" oder "deu+eng"
    QList<TextWord> words;

    bool isEmpty() const { return words.isEmpty(); }

    // Text der Wörter from..to (einschließlich): Leerzeichen zwischen Wörtern,
    // Zeilenumbruch zwischen Zeilen, Leerzeile zwischen Absätzen.
    QString text(int from = 0, int to = -1) const
    {
        if (words.isEmpty())
            return {};
        if (to < 0 || to >= words.size())
            to = int(words.size()) - 1;
        from = qBound(0, from, to);
        QString out;
        for (int i = from; i <= to; ++i) {
            if (i > from) {
                const TextWord &prev = words.at(i - 1);
                if (words.at(i).para != prev.para)
                    out += QStringLiteral("\n\n");
                else if (words.at(i).line != prev.line)
                    out += QLatin1Char('\n');
                else
                    out += QLatin1Char(' ');
            }
            out += words.at(i).text;
        }
        return out;
    }

    // Anfang jedes Wortes in text() (Zeichenposition), für Textcursor und
    // Bildschirmleser.
    QList<int> wordOffsets() const
    {
        QList<int> out;
        int pos = 0;
        for (int i = 0; i < words.size(); ++i) {
            if (i > 0) {
                const TextWord &prev = words.at(i - 1);
                pos += words.at(i).para != prev.para ? 2 : 1;
            }
            out << pos;
            pos += int(words.at(i).text.size());
        }
        return out;
    }
};

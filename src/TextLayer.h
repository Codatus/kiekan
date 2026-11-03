// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QMap>
#include <QRect>
#include <QSize>
#include <QString>

#include <algorithm>
#include <cstdlib>

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

    // Wie text(), aber mehrzeilige Überschriften (große Schrift, Zeilen direkt
    // untereinander) als eine Zeile – zum Vorlesen, wo Orca zeilenweise liest.
    // Gleich lang wie text(), die Zeichenpositionen bleiben gültig.
    QString readingText() const
    {
        QString out = text();
        if (words.size() < 2)
            return out;
        // Höhe je Zeile (Median der Wörter) und typische Höhe des ganzen Textes
        auto quantile = [](QList<int> v, int divisor) {
            std::sort(v.begin(), v.end());
            return v.isEmpty() ? 0 : v.at(v.size() / divisor);
        };
        auto median = [&](const QList<int> &v) { return quantile(v, 2); };
        QList<int> all;
        QMap<int, QList<int>> heights;
        QMap<int, QRect> lineBox;
        for (const TextWord &w : words) {
            all << w.box.height();
            heights[w.line] << w.box.height();
            lineBox[w.line] |= w.box;
        }
        const int body = quantile(all, 3);   // Fließtext: auch bei viel Überschrift
        const QList<int> offsets = wordOffsets();
        for (int i = 1; i < words.size(); ++i) {
            const TextWord &prev = words.at(i - 1), &cur = words.at(i);
            if (cur.line == prev.line)
                continue;
            const int hp = median(heights.value(prev.line)), hc = median(heights.value(cur.line));
            const QRect bp = lineBox.value(prev.line), bc = lineBox.value(cur.line);
            const bool big = hp * 10 >= body * 14 && hc * 10 >= body * 14;
            const bool alike = std::abs(hp - hc) * 4 <= std::max(hp, hc);
            const bool close = bc.top() > bp.top() && bc.top() - bp.bottom() < std::max(hp, hc);
            if (big && alike && close) {
                const int at = offsets.at(i - 1) + int(prev.text.size());
                for (int k = at; k < offsets.at(i); ++k)
                    out[k] = QLatin1Char(' ');
            }
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

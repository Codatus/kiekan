// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

// Natürliche Reihenfolge (IMG_2 vor IMG_10), ohne Groß-/Kleinschreibung.
// Eigene Umsetzung, weil QCollator ohne ICU (z. B. C-Locale) Zahlen nicht
// als Zahlen vergleicht.
inline int naturalCompare(const QString &a, const QString &b)
{
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const QChar ca = a.at(i), cb = b.at(j);
        if (ca.isDigit() && cb.isDigit()) {
            qsizetype si = i, sj = j;
            while (si < a.size() && a.at(si) == QLatin1Char('0'))
                ++si;
            while (sj < b.size() && b.at(sj) == QLatin1Char('0'))
                ++sj;
            qsizetype ei = si, ej = sj;
            while (ei < a.size() && a.at(ei).isDigit())
                ++ei;
            while (ej < b.size() && b.at(ej).isDigit())
                ++ej;
            if (ei - si != ej - sj)
                return ei - si < ej - sj ? -1 : 1;
            const int c = QStringView(a).mid(si, ei - si).compare(QStringView(b).mid(sj, ej - sj));
            if (c != 0)
                return c;
            i = ei;
            j = ej;
            continue;
        }
        const QChar fa = ca.toCaseFolded(), fb = cb.toCaseFolded();
        if (fa != fb) {
            const int c = QString::localeAwareCompare(QString(fa), QString(fb));
            return c != 0 ? c : (fa < fb ? -1 : 1);
        }
        ++i;
        ++j;
    }
    if (i < a.size() || j < b.size())
        return i < a.size() ? 1 : -1;
    return a.compare(b);   // nur Groß-/Kleinschreibung verschieden: fest ordnen
}

// KiekAn! – Test: Lesetext für den Bildschirmleser (mehrzeilige Überschriften)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TextLayer.h"

#include <cstdio>

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "OK    " : "FEHLER", what);
    if (!ok)
        ++failures;
}

int main()
{
    TextLayer t;
    t.size = QSize(2000, 2600);
    auto add = [&](int x, int y, int w, int h, const char *text, int line, int para) {
        t.words.append({QRect(x, y, w, h), QString::fromUtf8(text), line, para});
    };
    // Zweizeilige Überschrift (groß), Untertitel mit Abstand, Fließtext
    add(250, 250, 600, 85, "EINFÜHRUNG", 0, 0);
    add(900, 250, 300, 85, "ARBEIT", 0, 0);
    add(250, 360, 150, 85, "MIT", 1, 0);
    add(450, 360, 1100, 85, "PERSÖNLICHKEITSTEILEN", 1, 0);
    add(260, 650, 300, 70, "»Es", 2, 1);
    add(600, 650, 300, 70, "gibt«", 2, 1);
    add(260, 1000, 100, 37, "Wir", 3, 2);
    add(380, 1000, 100, 37, "sind", 3, 2);
    add(260, 1060, 200, 37, "Situationen", 4, 2);
    add(480, 1060, 100, 37, "die-", 4, 2);
    add(260, 1120, 100, 37, "ses", 5, 2);

    const QString plain = t.text(), reading = t.readingText();
    check(plain.size() == reading.size(), "gleich lang (Zeichenpositionen bleiben gültig)");
    check(reading.startsWith(QString::fromUtf8("EINFÜHRUNG ARBEIT MIT PERSÖNLICHKEITSTEILEN\n\n")),
          "Überschrift als eine Zeile");
    check(plain.startsWith(QString::fromUtf8("EINFÜHRUNG ARBEIT\nMIT")), "Kopiertext behält den Umbruch");
    check(reading.contains(QString::fromUtf8("»Es gibt«\n\nWir")), "Untertitel mit Abstand bleibt eigene Zeile");
    check(reading.contains(QStringLiteral("Wir sind\nSituationen die-\nses")), "Fließtext bleibt zeilenweise");

    std::printf("%s\n", failures ? "Fehlgeschlagen." : "Alles in Ordnung.");
    return failures ? 1 : 0;
}

// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QList>
#include <QString>

#include <functional>

#include "TextLayer.h"

// Bilder als PDF speichern, eine Seite je Bild. Liegt erkannter Text vor,
// steht er unsichtbar über den Wörtern: Die PDF ist durchsuchbar und der
// Text lässt sich kopieren (wie bei gescannten Dokumenten mit Texterkennung).
namespace PdfExport {

struct Page
{
    QImage image;
    TextLayer text;   // Rahmen in Pixeln von `image`; leer = nur Bild
};

// Gibt die Fehlermeldung zurück (leer bei Erfolg). Schreibt atomar.
QString write(const QString &path, const QList<Page> &pages, const QString &title);
// Seite für Seite erzeugt (spart Speicher bei vielen Bildern). `page(i)`
// liefert ein leeres Bild, um die Seite auszulassen; `keepGoing(i)` false
// bricht ab (nach Seite i).
QString write(const QString &path, int count, const std::function<Page(int)> &page, const QString &title,
              const std::function<bool(int)> &keepGoing = {});

// Textebene an eine neue Bildgröße anpassen (z. B. nach Verkleinern); leer,
// wenn die Seitenverhältnisse nicht passen.
TextLayer scaled(const TextLayer &layer, const QSize &to);

} // namespace PdfExport

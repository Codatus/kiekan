// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QStringList>

// Welche Dateien Kiek an kennt (Bilder, PDFs, Videos) – gemeinsam für
// Hauptfenster und KI-Schnittstelle.
namespace Media {

bool isVideo(const QString &path);
bool isPdf(const QString &path);

QStringList imageNameFilters();   // inkl. PDF, falls unterstützt
QStringList allNameFilters();     // zusätzlich Videos, falls unterstützt

int pdfPageCount(const QString &path);   // 0 bei Fehler oder ohne PDF-Unterstützung
// Text aus der Textebene einer PDF-Seite (leer bei reinen Scans).
QString pdfPageText(const QString &path, int page);

} // namespace Media

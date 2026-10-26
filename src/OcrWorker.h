// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Ocr.h"

#include <QList>
#include <QRect>
#include <QString>

#include <functional>

// Texterkennung in Hilfsprozessen (kiekan --ocr-worker), wie bei ocrmypdf:
// Tesseract nutzt über OpenMP sonst mehrere Threads, die kaum schneller
// machen, aber die Prozessoren belegen. Die Grenze OMP_THREAD_LIMIT=1 muss
// gesetzt sein, bevor die OpenMP-Bibliothek geladen wird – also beim
// Prozessstart. Mehrere Seiten laufen stattdessen in parallelen Prozessen.
// Nebenbei kann ein Tesseract-Absturz Kiek an nicht mehr mitreißen.
namespace OcrWorker {

struct Job
{
    QString path;          // Bild oder PDF …
    int page = 0;
    double pdfDpi = 300;
    QString imageFile;     // … oder ein fertiges Bild (z. B. Auswahl, ungespeicherte Änderung)
    QRect region;          // optional, in Pixeln des geladenen Bildes
    Ocr::Options options;
};

// Einstiegspunkt für „kiekan --ocr-worker“: liest einen Auftrag (JSON) von
// stdin, schreibt das Ergebnis (JSON) nach stdout.
int workerMain();

// Ein Auftrag, blockierend.
Ocr::Result run(const Job &job);

// Mehrere Aufträge mit höchstens `parallel` gleichzeitigen Prozessen.
// Ergebnisse in Auftragsreihenfolge.
QList<Ocr::Result> runMany(const QList<Job> &jobs, int parallel,
                           const std::function<void(int done, int total)> &progress = {});

// Sinnvolle Zahl gleichzeitiger Prozesse (Einstellung „ocr/jobs“, sonst Kerne, max. 8).
int defaultParallel();

} // namespace OcrWorker

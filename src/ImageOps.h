// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QPointF>
#include <QVector>
#include <QThread>

#include <algorithm>
#include <array>
#include <cstdint>
#include <thread>
#include <vector>

// Grundlegende, schnelle Bildoperationen. Alle Funktionen erwarten
// Format_RGB32 oder Format_ARGB32 (siehe forProcessing) und verteilen die
// Arbeit zeilenweise auf alle Prozessorkerne.
namespace ImageOps {

using Lut = std::array<uint8_t, 256>;

struct Histogram
{
    std::array<uint32_t, 256> red{}, green{}, blue{}, luma{};
    uint64_t pixels = 0;
};

// Bild in ein Format bringen, auf dem Filter arbeiten (nicht vormultipliziert).
QImage forProcessing(const QImage &image);
// Ergebnis in das schnell zeichenbare Anzeigeformat bringen.
QImage forDisplay(const QImage &image);

// Ruft fn(y0, y1) für Zeilenblöcke parallel auf.
template<typename Fn>
void parallelRows(int height, Fn &&fn)
{
    const int threads = std::clamp(height / 32, 1, std::max(1, QThread::idealThreadCount()));
    if (threads == 1) {
        fn(0, height);
        return;
    }
    const int chunk = (height + threads - 1) / threads;
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int t = 0; t < threads; ++t) {
        const int y0 = t * chunk;
        const int y1 = std::min(height, y0 + chunk);
        if (y0 < y1)
            pool.emplace_back([&fn, y0, y1] { fn(y0, y1); });
    }
    for (auto &th : pool)
        th.join();
}

inline int luma(int r, int g, int b)
{
    // Rec. 709, ganzzahlig
    return (r * 54 + g * 183 + b * 19) >> 8;
}

Histogram histogram(const QImage &image);

Lut identityLut();
// Gradationskurve durch Stützpunkte (x, y in 0..255), glatt und ohne Überschwinger.
Lut curveLut(const QVector<QPointF> &points);
QImage applyLuts(const QImage &image, const Lut &r, const Lut &g, const Lut &b);
// Histogrammausgleich der Helligkeit als Kurve (für den Kurven-Dialog).
Lut equalizationLut(const Histogram &h);
// Auto-Tonwert: streckt jeden Kanal so, dass `clip` Anteil an den Enden abgeschnitten wird.
void autoLevelsLuts(const Histogram &h, double clip, Lut &r, Lut &g, Lut &b);

QImage grayscale(const QImage &image);
QImage invert(const QImage &image);
QImage blackWhite(const QImage &image, int threshold, bool dither);
// 3×3-Farbmatrix (Zeilen: neues R, G, B), wirkt auf 0..255-Werte.
QImage colorMatrix(const QImage &image, const float m[9]);

QImage gaussianBlur(const QImage &image, double sigma);
QImage unsharpMask(const QImage &image, double amount, double sigma, int threshold);
// 3×3-Faltung; bei `gray` wird das Ergebnis als Grauwert ausgegeben.
QImage convolve3x3(const QImage &image, const float kernel[9], float bias, bool gray);

} // namespace ImageOps

// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

// Verlustfreies Drehen und Spiegeln von JPEGs: Die komprimierten Blöcke
// werden umgeordnet (libjpeg-turbo), statt das Bild neu zu kodieren. Geht
// das nicht ohne Randverlust, wird nur die EXIF-Ausrichtung gesetzt.
namespace LosslessJpeg {

// Die acht Lagen eines Bildes (Drehungen und Spiegelungen). Die Werte
// entsprechen der EXIF-Ausrichtung 1..8.
enum class Op {
    None = 1,
    FlipH = 2,
    Rot180 = 3,
    FlipV = 4,
    Transpose = 5,
    Rot90 = 6,       // im Uhrzeigersinn
    Transverse = 7,
    Rot270 = 8,
};

// Erst `first`, dann `then` anwenden.
Op compose(Op first, Op then);
bool swapsSides(Op op);

// Mit libjpeg-turbo gebaut (Pixel umordnen)? Sonst nur EXIF-Ausrichtung.
bool pixelTransformAvailable();

bool isJpeg(const QString &path);

enum class Result {
    Pixels,      // Blöcke umgeordnet, Ausrichtung „normal“
    Tag,         // nur EXIF-Ausrichtung geändert
    Failed,
};

// `op`: Drehung/Spiegelung, wie der Benutzer sie am angezeigten Bild
// vorgenommen hat. `autoRotate`: Die Anzeige berücksichtigt die EXIF-
// Ausrichtung der Datei. Schreibt atomar nach `target` (darf `source` sein).
Result apply(const QString &source, const QString &target, Op op, bool autoRotate, QString *error);

} // namespace LosslessJpeg

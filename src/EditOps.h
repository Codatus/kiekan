// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

// Bearbeitungsschritte als JSON-Liste (z. B. {"op":"crop","x":…}), gemeinsam
// genutzt von der KI-Schnittstelle und dem Fenster („Vorschlag der KI“).
namespace EditOps {

struct Error
{
    QString message;
};

// Wendet die Schritte der Reihe nach an; wirft Error bei ungültigen Angaben.
// `log` erhält je Schritt eine Zeile wie „crop -> 800x600“.
QImage apply(QImage image, const QJsonArray &ops, QStringList *log = nullptr);
QImage applyOperation(const QImage &image, const QJsonObject &op);

QImage scaledToMax(const QImage &image, int maxSide);

} // namespace EditOps

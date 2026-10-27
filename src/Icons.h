// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QIcon>

// Icons nach freedesktop-Namen („zoom-in“, „document-open“ …). Ist ein
// Icon-Thema vorhanden (Linux-Desktops), wird es benutzt; sonst (Windows,
// schlanke Desktops) Standard-Icons des Qt-Stils bzw. selbst gezeichnete.
namespace Icons {
QIcon get(const QString &name);
}

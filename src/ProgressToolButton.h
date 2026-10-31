// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QToolButton>

// Werkzeugknopf mit Fortschrittskreis um das Symbol (z. B. Texterkennung).
// progress < 0: kein Kreis; 0..100: Kreis füllt sich; done: dezenter voller
// Kreis als Zeichen, dass das Ergebnis bereitliegt.
class ProgressToolButton : public QToolButton
{
    Q_OBJECT

public:
    using QToolButton::QToolButton;

    void setProgress(int percent);
    void setDone(bool done);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int m_progress = -1;
    bool m_done = false;
};

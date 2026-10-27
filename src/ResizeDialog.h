// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;

// Dialog „Größe ändern“: Breite/Höhe in Pixeln oder Prozent,
// wahlweise mit festem Seitenverhältnis.
class ResizeDialog : public QDialog
{
    Q_OBJECT

public:
    ResizeDialog(const QSize &original, QWidget *parent = nullptr);

    QSize targetSize() const;
    bool smooth() const;

private:
    void widthChanged(int w);
    void heightChanged(int h);
    void percentChanged(double p);

    QSize m_original;
    QSpinBox *m_width;
    QSpinBox *m_height;
    QDoubleSpinBox *m_percent;
    QCheckBox *m_keepRatio;
    QCheckBox *m_smooth;
    bool m_updating = false;
};

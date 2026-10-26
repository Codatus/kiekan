// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ImageOps.h"

#include <QDialog>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;

namespace HistogramPaint {
enum Channel { All = -1, Luma = 0, Red = 1, Green = 2, Blue = 3 };
const std::array<uint32_t, 256> &channel(const ImageOps::Histogram &h, int ch);
QColor color(int ch);
// Zeichnet die Verteilung eines Kanals als Fläche in `rect`.
void draw(QPainter &p, const QRectF &rect, const ImageOps::Histogram &h, int ch,
          bool logarithmic, int alpha = 255);
}

class HistogramWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HistogramWidget(QWidget *parent = nullptr);
    void setHistogram(const ImageOps::Histogram &h);
    void setChannel(int channel);
    void setLogarithmic(bool on);
    QSize sizeHint() const override { return {512, 220}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    ImageOps::Histogram m_hist;
    int m_channel = HistogramPaint::All;
    bool m_log = false;
};

// Nicht-modales Fenster, das das Histogramm des aktuellen Bildes zeigt.
class HistogramDialog : public QDialog
{
    Q_OBJECT

public:
    explicit HistogramDialog(QWidget *parent = nullptr);
    void setImage(const QImage &image);

private:
    HistogramWidget *m_widget;
    QComboBox *m_channel;
    QCheckBox *m_log;
    QLabel *m_stats;
    ImageOps::Histogram m_hist;
    qint64 m_cacheKey = 0;

    void updateStats();
};

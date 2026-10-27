// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ImageOps.h"

#include <QDialog>
#include <QTimer>
#include <QVector>
#include <QWidget>

class PreviewLabel;
class QComboBox;

// Gradationskurven-Editor über dem Histogramm.
// Kanal 0 wirkt auf alle Farben, 1–3 auf Rot, Grün, Blau.
class CurveWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CurveWidget(QWidget *parent = nullptr);

    void setHistogram(const ImageOps::Histogram &h);
    void setChannel(int channel);
    int channel() const { return m_channel; }

    QVector<QPointF> points(int channel) const { return m_points[channel]; }
    void setPoints(int channel, const QVector<QPointF> &points);
    void resetChannel(int channel);

    ImageOps::Lut lut(int channel) const;
    // Endgültige Tabellen für R, G, B (Kanalkurve nach Gesamtkurve).
    void finalLuts(ImageOps::Lut &r, ImageOps::Lut &g, ImageOps::Lut &b) const;

    QSize sizeHint() const override { return {360, 360}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }

Q_SIGNALS:
    void changed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QRectF area() const;
    QPointF toWidget(const QPointF &v) const;
    QPointF toValue(const QPointF &w) const;
    int pointAt(const QPointF &pos) const;
    void removePoint(int index);

    ImageOps::Histogram m_hist;
    QVector<QPointF> m_points[4];
    int m_channel = 0;
    int m_drag = -1;
};

class CurvesDialog : public QDialog
{
    Q_OBJECT

public:
    CurvesDialog(const QImage &image, QWidget *parent = nullptr);
    void finalLuts(ImageOps::Lut &r, ImageOps::Lut &g, ImageOps::Lut &b) const;

private:
    void schedulePreview();
    void renderPreview();

    CurveWidget *m_curve;
    QComboBox *m_channel;
    PreviewLabel *m_preview;
    QImage m_source;
    ImageOps::Histogram m_hist;
    QTimer m_timer;
};

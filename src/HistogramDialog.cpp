// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "HistogramDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <cmath>

namespace HistogramPaint {

const std::array<uint32_t, 256> &channel(const ImageOps::Histogram &h, int ch)
{
    switch (ch) {
    case Red: return h.red;
    case Green: return h.green;
    case Blue: return h.blue;
    default: return h.luma;
    }
}

QColor color(int ch)
{
    switch (ch) {
    case Red: return QColor(220, 50, 47);
    case Green: return QColor(60, 170, 60);
    case Blue: return QColor(40, 110, 220);
    default: return QColor(150, 150, 150);
    }
}

void draw(QPainter &p, const QRectF &rect, const ImageOps::Histogram &h, int ch, bool logarithmic, int alpha)
{
    const auto &data = channel(h, ch);
    double maxV = 0;
    for (int i = 0; i < 256; ++i) {
        const double v = logarithmic ? std::log1p(double(data[i])) : double(data[i]);
        maxV = std::max(maxV, v);
    }
    if (maxV <= 0)
        return;

    QPainterPath path;
    path.moveTo(rect.left(), rect.bottom());
    for (int i = 0; i < 256; ++i) {
        const double v = logarithmic ? std::log1p(double(data[i])) : double(data[i]);
        const double x = rect.left() + (i + 0.5) * rect.width() / 256.0;
        path.lineTo(x, rect.bottom() - v / maxV * rect.height());
    }
    path.lineTo(rect.right(), rect.bottom());
    path.closeSubpath();

    QColor c = color(ch);
    c.setAlpha(alpha);
    p.fillPath(path, c);
}

} // namespace HistogramPaint

// --- HistogramWidget -------------------------------------------------------

HistogramWidget::HistogramWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(256, 120);
}

void HistogramWidget::setHistogram(const ImageOps::Histogram &h)
{
    m_hist = h;
    update();
}

void HistogramWidget::setChannel(int channel)
{
    m_channel = channel;
    update();
}

void HistogramWidget::setLogarithmic(bool on)
{
    m_log = on;
    update();
}

void HistogramWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 30));
    const QRectF area = QRectF(rect()).adjusted(4, 4, -4, -4);

    p.setPen(QColor(70, 70, 70));
    for (int i = 1; i < 4; ++i) {
        const double x = area.left() + area.width() * i / 4.0;
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }

    p.setRenderHint(QPainter::Antialiasing);
    if (m_channel == HistogramPaint::All) {
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        for (int ch : {HistogramPaint::Red, HistogramPaint::Green, HistogramPaint::Blue})
            HistogramPaint::draw(p, area, m_hist, ch, m_log, 200);
    } else {
        HistogramPaint::draw(p, area, m_hist, m_channel, m_log);
    }
}

// --- HistogramDialog -------------------------------------------------------

HistogramDialog::HistogramDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Histogramm"));

    m_widget = new HistogramWidget(this);

    m_channel = new QComboBox(this);
    m_channel->addItem(tr("RGB überlagert"), int(HistogramPaint::All));
    m_channel->addItem(tr("Helligkeit"), int(HistogramPaint::Luma));
    m_channel->addItem(tr("Rot"), int(HistogramPaint::Red));
    m_channel->addItem(tr("Grün"), int(HistogramPaint::Green));
    m_channel->addItem(tr("Blau"), int(HistogramPaint::Blue));
    connect(m_channel, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_widget->setChannel(m_channel->currentData().toInt());
        updateStats();
    });

    m_log = new QCheckBox(tr("Logarithmisch"), this);
    connect(m_log, &QCheckBox::toggled, m_widget, &HistogramWidget::setLogarithmic);

    m_stats = new QLabel(this);
    m_stats->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *top = new QHBoxLayout;
    top->addWidget(m_channel);
    top->addWidget(m_log);
    top->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_widget, 1);
    layout->addWidget(m_stats);
    resize(560, 340);
}

void HistogramDialog::setImage(const QImage &image)
{
    if (image.isNull()) {
        m_hist = ImageOps::Histogram();
        m_cacheKey = 0;
    } else if (image.cacheKey() != m_cacheKey) {
        m_hist = ImageOps::histogram(image);
        m_cacheKey = image.cacheKey();
    }
    m_widget->setHistogram(m_hist);
    updateStats();
}

void HistogramDialog::updateStats()
{
    int ch = m_channel->currentData().toInt();
    if (ch == HistogramPaint::All)
        ch = HistogramPaint::Luma;
    const auto &data = HistogramPaint::channel(m_hist, ch);
    if (m_hist.pixels == 0) {
        m_stats->clear();
        return;
    }
    double sum = 0;
    for (int i = 0; i < 256; ++i)
        sum += double(i) * data[i];
    const double mean = sum / double(m_hist.pixels);
    double var = 0;
    for (int i = 0; i < 256; ++i)
        var += (i - mean) * (i - mean) * data[i];
    const double stddev = std::sqrt(var / double(m_hist.pixels));
    uint64_t acc = 0;
    int median = 0;
    for (int i = 0; i < 256; ++i) {
        acc += data[i];
        if (acc * 2 >= m_hist.pixels) { median = i; break; }
    }
    const QLocale loc;
    m_stats->setText(tr("Mittelwert: %1   Median: %2   Standardabweichung: %3")
                         .arg(loc.toString(mean, 'f', 1))
                         .arg(median)
                         .arg(loc.toString(stddev, 'f', 1)));
}

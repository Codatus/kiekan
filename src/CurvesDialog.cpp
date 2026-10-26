// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CurvesDialog.h"

#include "FilterDialog.h"
#include "HistogramDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr double kHit = 8.0;   // Fangradius für Punkte in Pixeln

QVector<QPointF> identityPoints()
{
    return {QPointF(0, 0), QPointF(255, 255)};
}

// Kurve aus einer Tabelle mit wenigen Stützpunkten nachbilden. Die Punkte
// werden nach Ausgabewerten verteilt, damit steile Abschnitte (dort, wo das
// Histogramm viele Pixel hat) genug Punkte zum Nachjustieren bekommen.
QVector<QPointF> pointsFromLut(const ImageOps::Lut &lut, int count)
{
    QVector<QPointF> pts{QPointF(0, lut[0])};
    for (int k = 1; k < count - 1; ++k) {
        const int target = int(std::lround(k * 255.0 / (count - 1)));
        int x = 0;
        while (x < 255 && lut[x] < target)
            ++x;
        if (x > pts.last().x() && x < 255)
            pts.append(QPointF(x, lut[x]));
    }
    pts.append(QPointF(255, lut[255]));
    return pts;
}

} // namespace

// --- CurveWidget -----------------------------------------------------------

CurveWidget::CurveWidget(QWidget *parent)
    : QWidget(parent)
{
    for (auto &p : m_points)
        p = identityPoints();
    setMinimumSize(256, 256);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Expanding);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    setMouseTracking(true);
}

void CurveWidget::setHistogram(const ImageOps::Histogram &h)
{
    m_hist = h;
    update();
}

void CurveWidget::setChannel(int channel)
{
    m_channel = std::clamp(channel, 0, 3);
    m_drag = -1;
    update();
}

void CurveWidget::setPoints(int channel, const QVector<QPointF> &points)
{
    m_points[channel] = points;
    update();
    Q_EMIT changed();
}

void CurveWidget::resetChannel(int channel)
{
    setPoints(channel, identityPoints());
}

ImageOps::Lut CurveWidget::lut(int channel) const
{
    return ImageOps::curveLut(m_points[channel]);
}

void CurveWidget::finalLuts(ImageOps::Lut &r, ImageOps::Lut &g, ImageOps::Lut &b) const
{
    const ImageOps::Lut master = lut(0);
    const ImageOps::Lut lr = lut(1), lg = lut(2), lb = lut(3);
    for (int i = 0; i < 256; ++i) {
        r[i] = lr[master[i]];
        g[i] = lg[master[i]];
        b[i] = lb[master[i]];
    }
}

QRectF CurveWidget::area() const
{
    const double side = std::min(width(), height()) - 2 * kHit;
    return QRectF((width() - side) / 2, (height() - side) / 2, side, side);
}

QPointF CurveWidget::toWidget(const QPointF &v) const
{
    const QRectF a = area();
    return QPointF(a.left() + v.x() / 255.0 * a.width(), a.bottom() - v.y() / 255.0 * a.height());
}

QPointF CurveWidget::toValue(const QPointF &w) const
{
    const QRectF a = area();
    return QPointF(std::clamp((w.x() - a.left()) / a.width() * 255.0, 0.0, 255.0),
                   std::clamp((a.bottom() - w.y()) / a.height() * 255.0, 0.0, 255.0));
}

int CurveWidget::pointAt(const QPointF &pos) const
{
    const auto &pts = m_points[m_channel];
    for (int i = 0; i < pts.size(); ++i)
        if (QLineF(toWidget(pts[i]), pos).length() <= kHit)
            return i;
    return -1;
}

void CurveWidget::removePoint(int index)
{
    auto &pts = m_points[m_channel];
    if (pts.size() <= 2 || index <= 0 || index >= pts.size() - 1)
        return;   // Endpunkte bleiben
    pts.removeAt(index);
    update();
    Q_EMIT changed();
}

void CurveWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    const QRectF a = area();
    p.fillRect(a, QColor(30, 30, 30));

    p.setRenderHint(QPainter::Antialiasing);
    const int histChannel = m_channel == 0 ? HistogramPaint::Luma : m_channel;
    HistogramPaint::draw(p, a, m_hist, histChannel, false, 110);

    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QColor(70, 70, 70));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(a.left() + a.width() * i / 4, a.top()), QPointF(a.left() + a.width() * i / 4, a.bottom()));
        p.drawLine(QPointF(a.left(), a.top() + a.height() * i / 4), QPointF(a.right(), a.top() + a.height() * i / 4));
    }
    p.setPen(QPen(QColor(110, 110, 110), 1, Qt::DashLine));
    p.drawLine(a.bottomLeft(), a.topRight());

    p.setRenderHint(QPainter::Antialiasing);
    const ImageOps::Lut l = lut(m_channel);
    QPainterPath path;
    for (int x = 0; x < 256; ++x) {
        const QPointF pt = toWidget(QPointF(x, l[x]));
        if (x == 0)
            path.moveTo(pt);
        else
            path.lineTo(pt);
    }
    const QColor curveColor = m_channel == 0 ? QColor(230, 230, 230) : HistogramPaint::color(m_channel).lighter(130);
    p.setPen(QPen(curveColor, 2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    const auto &pts = m_points[m_channel];
    for (int i = 0; i < pts.size(); ++i) {
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(i == m_drag ? QColor(255, 200, 0) : Qt::white);
        p.drawEllipse(toWidget(pts[i]), 5, 5);
    }
}

void CurveWidget::mousePressEvent(QMouseEvent *event)
{
    auto &pts = m_points[m_channel];
    const int hit = pointAt(event->position());
    if (event->button() == Qt::RightButton) {
        if (hit >= 0)
            removePoint(hit);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;

    if (hit >= 0) {
        m_drag = hit;
    } else {
        const QPointF v = toValue(event->position());
        const double x = std::round(v.x());
        int insertAt = 0;
        while (insertAt < pts.size() && pts[insertAt].x() < x)
            ++insertAt;
        if (insertAt < pts.size() && pts[insertAt].x() == x) {
            m_drag = insertAt;   // gleiche Spalte: vorhandenen Punkt nehmen
        } else {
            pts.insert(insertAt, QPointF(x, std::round(v.y())));
            m_drag = insertAt;
        }
        Q_EMIT changed();
    }
    update();
}

void CurveWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag < 0) {
        setCursor(pointAt(event->position()) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
        return;
    }
    auto &pts = m_points[m_channel];
    const QPointF v = toValue(event->position());
    // Punkte dürfen ihre Nachbarn nicht überholen.
    const double minX = m_drag > 0 ? pts[m_drag - 1].x() + 1 : 0;
    const double maxX = m_drag < pts.size() - 1 ? pts[m_drag + 1].x() - 1 : 255;
    pts[m_drag] = QPointF(std::clamp(std::round(v.x()), minX, maxX), std::round(v.y()));
    update();
    Q_EMIT changed();
}

void CurveWidget::mouseReleaseEvent(QMouseEvent *)
{
    m_drag = -1;
    update();
}

void CurveWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int hit = pointAt(event->position());
    if (hit >= 0)
        removePoint(hit);
}

// --- CurvesDialog ----------------------------------------------------------

CurvesDialog::CurvesDialog(const QImage &image, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Gradationskurven"));
    m_hist = ImageOps::histogram(image);
    m_source = makePreviewSource(image, QSize(800, 600), nullptr);

    m_curve = new CurveWidget(this);
    m_curve->setHistogram(m_hist);
    connect(m_curve, &CurveWidget::changed, this, &CurvesDialog::schedulePreview);

    m_channel = new QComboBox(this);
    m_channel->addItems({tr("Helligkeit (alle Farben)"), tr("Rot"), tr("Grün"), tr("Blau")});
    connect(m_channel, qOverload<int>(&QComboBox::currentIndexChanged), m_curve, &CurveWidget::setChannel);

    auto *resetChannel = new QPushButton(tr("Kanal zurücksetzen"), this);
    connect(resetChannel, &QPushButton::clicked, this, [this] { m_curve->resetChannel(m_curve->channel()); });

    auto *resetAll = new QPushButton(tr("Alle zurücksetzen"), this);
    connect(resetAll, &QPushButton::clicked, this, [this] {
        for (int c = 0; c < 4; ++c)
            m_curve->resetChannel(c);
    });

    auto *equalize = new QPushButton(tr("Histogramm ausgleichen"), this);
    equalize->setToolTip(tr("Setzt die Helligkeitskurve so, dass alle Tonwerte gleich häufig vorkommen. "
                            "Die Punkte lassen sich danach weiter anpassen."));
    connect(equalize, &QPushButton::clicked, this, [this] {
        m_channel->setCurrentIndex(0);
        m_curve->setPoints(0, pointsFromLut(ImageOps::equalizationLut(m_hist), 17));
    });

    auto *autoLevels = new QPushButton(tr("Auto-Tonwert"), this);
    autoLevels->setToolTip(tr("Streckt jeden Farbkanal auf den vollen Tonwertumfang "
                              "(0,5 % an den Enden werden abgeschnitten)."));
    connect(autoLevels, &QPushButton::clicked, this, [this] {
        ImageOps::Lut r, g, b;
        ImageOps::autoLevelsLuts(m_hist, 0.005, r, g, b);
        const ImageOps::Lut *luts[3] = {&r, &g, &b};
        for (int c = 0; c < 3; ++c) {
            int lo = 0, hi = 255;
            while (lo < 255 && (*luts[c])[lo] == 0 && (*luts[c])[lo + 1] == 0) ++lo;
            while (hi > 0 && (*luts[c])[hi] == 255 && (*luts[c])[hi - 1] == 255) --hi;
            m_curve->setPoints(c + 1, hi > lo ? QVector<QPointF>{QPointF(lo, 0), QPointF(hi, 255)}
                                              : QVector<QPointF>{QPointF(0, 0), QPointF(255, 255)});
        }
    });

    m_preview = new PreviewLabel(this);
    m_preview->setImages(m_source, m_source);
    auto *before = new QPushButton(tr("Vorher (gedrückt halten)"), this);
    connect(before, &QPushButton::pressed, this, [this] { m_preview->showBefore(true); });
    connect(before, &QPushButton::released, this, [this] { m_preview->showBefore(false); });

    auto *help = new QLabel(tr("Klicken fügt einen Punkt hinzu, Ziehen verschiebt ihn,\n"
                               "Rechtsklick oder Doppelklick entfernt ihn."), this);
    help->setStyleSheet(QStringLiteral("color: gray"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (QPushButton *b : {resetChannel, resetAll, equalize, autoLevels, before})
        b->setAutoDefault(false);

    auto *left = new QVBoxLayout;
    left->addWidget(m_channel);
    left->addWidget(m_curve, 1);
    left->addWidget(help);
    auto *grid = new QHBoxLayout;
    grid->addWidget(equalize);
    grid->addWidget(autoLevels);
    left->addLayout(grid);
    auto *grid2 = new QHBoxLayout;
    grid2->addWidget(resetChannel);
    grid2->addWidget(resetAll);
    left->addLayout(grid2);

    auto *right = new QVBoxLayout;
    right->addWidget(m_preview, 1);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(before);
    bottom->addStretch(1);
    bottom->addWidget(buttons);
    right->addLayout(bottom);

    auto *layout = new QHBoxLayout(this);
    layout->addLayout(left);
    layout->addLayout(right, 1);

    m_timer.setSingleShot(true);
    m_timer.setInterval(20);
    connect(&m_timer, &QTimer::timeout, this, &CurvesDialog::renderPreview);
    resize(1100, 560);
}

void CurvesDialog::finalLuts(ImageOps::Lut &r, ImageOps::Lut &g, ImageOps::Lut &b) const
{
    m_curve->finalLuts(r, g, b);
}

void CurvesDialog::schedulePreview()
{
    m_timer.start();
}

void CurvesDialog::renderPreview()
{
    ImageOps::Lut r, g, b;
    m_curve->finalLuts(r, g, b);
    m_preview->setAfter(ImageOps::forDisplay(ImageOps::applyLuts(ImageOps::forProcessing(m_source), r, g, b)));
}

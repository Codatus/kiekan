// KiekAn! – Beispiel-Plugin „Rahmen“
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Zeigt, wie ein Filter-Plugin aufgebaut ist: eine Klasse, die
// Kiekan::FilterPlugin implementiert und ihre Filter zurückgibt.
// Bauen wie das Hauptprogramm; die fertige .so in einen Plugin-Ordner
// legen (siehe FilterInterface.h).

#include <kiekan/FilterInterface.h>

#include <QCoreApplication>
#include <QObject>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {

class RahmenFilter : public Kiekan::Filter
{
public:
    QString id() const override { return QStringLiteral("beispiel.rahmen"); }
    QString name() const override { return QCoreApplication::translate("RahmenPlugin", "Rahmen (Plugin)"); }

    QList<Kiekan::FilterParameter> parameters() const override
    {
        Kiekan::FilterParameter width;
        width.id = QStringLiteral("breite");
        width.label = QCoreApplication::translate("RahmenPlugin", "Breite");
        width.minimum = 0;
        width.maximum = 30;
        width.defaultValue = 5;
        width.suffix = QStringLiteral(" %");

        Kiekan::FilterParameter polaroid;
        polaroid.id = QStringLiteral("polaroid");
        polaroid.label = QCoreApplication::translate("RahmenPlugin", "Sofortbild (unten breiter)");
        polaroid.type = Kiekan::FilterParameter::Toggle;
        polaroid.defaultValue = 1;

        Kiekan::FilterParameter dark;
        dark.id = QStringLiteral("dunkel");
        dark.label = QCoreApplication::translate("RahmenPlugin", "Schwarzer Rahmen");
        dark.type = Kiekan::FilterParameter::Toggle;
        dark.defaultValue = 0;

        return {width, polaroid, dark};
    }

    QImage apply(const QImage &image, const QVariantMap &params, double) const override
    {
        // Prozentangabe: gleich in Vorschau und Original, `scale` wird nicht gebraucht.
        const int shortSide = std::min(image.width(), image.height());
        const int border = int(std::lround(shortSide * params.value(QStringLiteral("breite")).toDouble() / 100.0));
        const int bottom = params.value(QStringLiteral("polaroid")).toDouble() > 0.5 ? border * 4 : border;
        const QColor color = params.value(QStringLiteral("dunkel")).toDouble() > 0.5 ? Qt::black : Qt::white;

        QImage out(image.width() + 2 * border, image.height() + border + bottom, QImage::Format_ARGB32);
        out.fill(color);
        QPainter p(&out);
        p.drawImage(border, border, image);
        return out;
    }
};

} // namespace

class RahmenPlugin : public QObject, public Kiekan::FilterPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID KIEKAN_FILTER_PLUGIN_IID FILE "rahmen.json")
    Q_INTERFACES(Kiekan::FilterPlugin)

public:
    QList<Kiekan::Filter *> filters() override { return {&m_rahmen}; }

private:
    RahmenFilter m_rahmen;
};

#include "RahmenPlugin.moc"

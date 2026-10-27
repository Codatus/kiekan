// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Öffentliche Schnittstelle für Filter-Plugins.
//
// Ein Plugin ist eine Qt-Plugin-Bibliothek (QPluginLoader), deren Wurzelobjekt
// Kiekan::FilterPlugin implementiert. KiekAn! sucht Plugins in
//   – <Programmordner>/plugins
//   – ~/.local/share/kiekan/plugins
//   – <Programmordner>/../lib/kiekan/plugins (installierte Plugins)
// Jeder Filter erscheint im Menü „Effekte“ und bekommt automatisch einen
// Dialog mit Reglern für seine Parameter und eine Live-Vorschau.
//
// Ein Beispiel liegt in plugins/rahmen/.

#pragma once

#include <QImage>
#include <QList>
#include <QString>
#include <QVariantMap>
#include <QtPlugin>

namespace Kiekan {

struct FilterParameter
{
    enum Type {
        Number,   // Schieberegler + Zahlenfeld
        Toggle    // Kontrollkästchen (Wert 0 oder 1)
    };

    QString id;
    QString label;
    Type type = Number;
    double minimum = 0;
    double maximum = 100;
    double defaultValue = 0;
    int decimals = 0;
    QString suffix;
    // Pixelmaße (Radius, Blockgröße …) werden für die verkleinerte
    // Vorschau mit `scale` multipliziert, damit sie gleich aussieht.
    bool scalesWithImage = false;
};

class Filter
{
public:
    virtual ~Filter() = default;

    // Eindeutige, unveränderliche Kennung, z. B. "rahmen.polaroid".
    virtual QString id() const = 0;
    // Angezeigter (übersetzter) Name.
    virtual QString name() const = 0;
    virtual QList<FilterParameter> parameters() const { return {}; }

    // `image` hat Format_RGB32 (ohne Transparenz) oder Format_ARGB32
    // (nicht vormultipliziert). Das Ergebnis darf jedes Format haben.
    // `scale` ist 1.0 beim echten Anwenden und < 1.0 in der Vorschau.
    // Wird aus mehreren Threads nie gleichzeitig aufgerufen.
    virtual QImage apply(const QImage &image, const QVariantMap &params, double scale) const = 0;
};

class FilterPlugin
{
public:
    virtual ~FilterPlugin() = default;
    // Die Filter gehören dem Plugin und bleiben bis Programmende gültig.
    virtual QList<Filter *> filters() = 0;
};

} // namespace Kiekan

#define KIEKAN_FILTER_PLUGIN_IID "org.kiekan.FilterPlugin/1.0"
Q_DECLARE_INTERFACE(Kiekan::FilterPlugin, KIEKAN_FILTER_PLUGIN_IID)

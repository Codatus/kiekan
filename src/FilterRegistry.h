// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <kiekan/FilterInterface.h>

#include <QList>
#include <QStringList>

#include <memory>
#include <vector>

// Alle verfügbaren Filter: eingebaute und aus Plugins geladene.
class FilterRegistry
{
public:
    static FilterRegistry &instance();

    // Durchsucht die Plugin-Ordner. Mehrfaches Aufrufen ist unschädlich.
    void loadPlugins();

    // Bildkorrekturen (Farben, Schärfen …) und Effekte (inkl. Plugins).
    const QList<const Kiekan::Filter *> &adjustments() const { return m_adjustments; }
    const QList<const Kiekan::Filter *> &effects() const { return m_effects; }
    const Kiekan::Filter *find(const QString &id) const;

    const QStringList &pluginFiles() const { return m_pluginFiles; }
    const QStringList &pluginErrors() const { return m_pluginErrors; }
    static QStringList pluginDirectories();

    // Wendet einen Filter auf das ganze Bild an (Format wird angepasst).
    static QImage run(const Kiekan::Filter *filter, const QImage &image, const QVariantMap &params, double scale = 1.0);
    static QVariantMap defaults(const Kiekan::Filter *filter);

private:
    FilterRegistry();

    std::vector<std::unique_ptr<Kiekan::Filter>> m_owned;
    QList<const Kiekan::Filter *> m_adjustments;
    QList<const Kiekan::Filter *> m_effects;
    QStringList m_pluginFiles;
    QStringList m_pluginErrors;
    bool m_pluginsLoaded = false;
};

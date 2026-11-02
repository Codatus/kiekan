// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FolderWalk.h"

#include "NaturalSort.h"

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>

#include <algorithm>

namespace FolderWalk {

namespace {

void sortNatural(QStringList &names)
{
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) { return naturalCompare(a, b) < 0; });
}

QStringList subdirs(const QString &dir)
{
    QStringList names = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Readable
                                            | QDir::Executable);
    sortNatural(names);
    return names;
}

struct Walker {
    const QStringList &filters;
    int budget;
    bool forward;

    // Erstes (vorwärts) bzw. letztes Bild in `dir` samt Unterordnern
    Result into(const QString &dir)
    {
        if (--budget < 0)
            return {};
        const QStringList own = files(dir, filters);
        if (!forward && !own.isEmpty())
            return {dir, own, int(own.size()) - 1, false};
        QStringList subs = subdirs(dir);
        if (!forward)
            std::reverse(subs.begin(), subs.end());
        for (const QString &s : std::as_const(subs)) {
            Result r = into(QDir(dir).filePath(s));
            if (!r.dir.isEmpty() || budget < 0)
                return r;
        }
        if (forward && !own.isEmpty())
            return {dir, own, 0, false};
        return {};
    }
};

QString clean(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

QStringList files(const QString &dir, const QStringList &nameFilters)
{
    QDir d(dir);
    QStringList names = d.entryList(nameFilters, QDir::Files | QDir::Readable);
    sortNatural(names);
    QStringList out;
    out.reserve(names.size());
    for (const QString &n : std::as_const(names))
        out << d.absoluteFilePath(n);
    return out;
}

Result neighbour(const QString &dir, bool forward, const QStringList &nameFilters, const QString &top, int maxDirs)
{
    Walker w{nameFilters, maxDirs, forward};
    QString current = clean(dir);
    const QString stop = clean(top);

    // Rückwärts kommen vor den Dateien eines Ordners seine Unterordner.
    if (!forward) {
        QStringList subs = subdirs(current);
        std::reverse(subs.begin(), subs.end());
        for (const QString &s : std::as_const(subs)) {
            Result r = w.into(QDir(current).filePath(s));
            if (!r.dir.isEmpty())
                return r;
            if (w.budget < 0)
                return {{}, {}, -1, true};
        }
    }

    // Dann eine Ebene höher: die Geschwister in Blätterrichtung, vorwärts
    // danach die Dateien des höheren Ordners selbst; sonst weiter nach oben.
    while (current != stop && current.startsWith(stop)) {
        const QFileInfo fi(current);
        const QString parent = clean(fi.absolutePath());
        if (parent == current)
            break;
        const QString name = fi.fileName();
        QStringList subs = subdirs(parent);
        if (!forward)
            std::reverse(subs.begin(), subs.end());
        // Stelle des eigenen Ordners; fehlt er (versteckt, Verknüpfung), nach
        // der natürlichen Reihenfolge einordnen.
        qsizetype pos = subs.indexOf(name);
        qsizetype from;
        if (pos >= 0) {
            from = pos + 1;
        } else {
            from = std::find_if(subs.cbegin(), subs.cend(), [&](const QString &s) {
                       const int c = naturalCompare(s, name);
                       return forward ? c > 0 : c < 0;
                   }) - subs.cbegin();
        }
        for (qsizetype i = from; i < subs.size(); ++i) {
            Result r = w.into(QDir(parent).filePath(subs.at(i)));
            if (!r.dir.isEmpty())
                return r;
            if (w.budget < 0)
                return {{}, {}, -1, true};
        }
        if (forward) {
            const QStringList own = files(parent, nameFilters);
            if (!own.isEmpty())
                return {parent, own, 0, false};
        }
        current = parent;
    }
    return {};
}

QString topFor(const QString &dir)
{
    const QString d = clean(dir);
    const QString home = clean(QDir::homePath());
    if (d == home)
        return home;
    if (d.startsWith(home + QLatin1Char('/')))
        return home + QLatin1Char('/') + d.mid(home.size() + 1).section(QLatin1Char('/'), 0, 0);
    const QString root = QStorageInfo(d).rootPath();
    return root.isEmpty() ? QDir::rootPath() : clean(root);
}

} // namespace FolderWalk

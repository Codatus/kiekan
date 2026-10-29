// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BatchRename.h"

#include "Metadata.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace BatchRename {

namespace {

// Zeichen, die unter Windows oder in Dateisystemen nicht erlaubt sind
const QRegularExpression &badChars()
{
    static const QRegularExpression re(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
    return re;
}

QString key(const QString &path)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return path.toLower();   // Groß-/Kleinschreibung zählt dort nicht
#else
    return path;
#endif
}

} // namespace

QDateTime dateTaken(const QString &path)
{
    const QString raw = Metadata::summary(path).dateTaken;
    QDateTime dt = QDateTime::fromString(raw.left(19), QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    if (!dt.isValid())
        dt = QDateTime::fromString(raw.left(19), Qt::ISODate);
    if (!dt.isValid())
        dt = QFileInfo(path).lastModified();
    return dt;
}

QString expand(const QString &pattern, const QString &path, int number, const QDateTime &taken)
{
    const QFileInfo fi(path);
    QString out;
    for (int i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        if (c == QLatin1Char('#')) {
            int width = 0;
            while (i < pattern.size() && pattern.at(i) == QLatin1Char('#')) {
                ++width;
                ++i;
            }
            --i;
            out += QStringLiteral("%1").arg(number, width, 10, QLatin1Char('0'));
        } else if (c == QLatin1Char('$') && i + 1 < pattern.size()) {
            const QChar k = pattern.at(i + 1).toUpper();
            if (k == QLatin1Char('N'))
                out += fi.completeBaseName();
            else if (k == QLatin1Char('D'))
                out += taken.toString(QStringLiteral("yyyy-MM-dd"));
            else if (k == QLatin1Char('T'))
                out += taken.toString(QStringLiteral("HH-mm-ss"));
            else if (k == QLatin1Char('F'))
                out += fi.absoluteDir().dirName();
            else {
                out += c;
                continue;
            }
            ++i;
        } else {
            out += c;
        }
    }
    return out.trimmed();
}

QList<Entry> preview(const QStringList &files, const Options &options)
{
    struct File { QString path; QDateTime taken; };
    QList<File> list;
    const bool needDate = options.sort == Sort::DateTaken || options.pattern.contains(QLatin1String("$D"), Qt::CaseInsensitive)
                          || options.pattern.contains(QLatin1String("$T"), Qt::CaseInsensitive);
    for (const QString &f : files)
        list.append({QFileInfo(f).absoluteFilePath(), needDate ? dateTaken(f) : QDateTime()});

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(list.begin(), list.end(), [&](const File &a, const File &b) {
        switch (options.sort) {
        case Sort::DateTaken:
            if (a.taken != b.taken)
                return a.taken < b.taken;
            break;
        case Sort::Modified: {
            const QDateTime ma = QFileInfo(a.path).lastModified(), mb = QFileInfo(b.path).lastModified();
            if (ma != mb)
                return ma < mb;
            break;
        }
        case Sort::Name:
            break;
        }
        return collator.compare(QFileInfo(a.path).fileName(), QFileInfo(b.path).fileName()) < 0;
    });

    QList<Entry> entries;
    QSet<QString> sources;
    for (const File &f : list)
        sources.insert(key(f.path));
    QHash<QString, int> targets;
    int number = options.start;
    for (const File &f : list) {
        const QFileInfo fi(f.path);
        QString name = expand(options.pattern, f.path, number, f.taken);
        number += options.step;
        QString suffix = fi.suffix();
        if (options.lowercaseSuffix)
            suffix = suffix.toLower();
        Entry e;
        e.source = f.path;
        QString problem;
        if (name.isEmpty())
            problem = QStringLiteral("empty");
        else if (name.contains(badChars()) || name == QLatin1String(".") || name == QLatin1String(".."))
            problem = QStringLiteral("invalid");
        if (problem.isEmpty()) {
            e.target = fi.absoluteDir().filePath(suffix.isEmpty() ? name : name + QLatin1Char('.') + suffix);
            // Ziel gehört schon einer anderen Datei, die nicht mit umbenannt wird?
            if (key(e.target) != key(e.source) && QFileInfo::exists(e.target) && !sources.contains(key(e.target)))
                problem = QStringLiteral("exists");
        } else {
            e.target = e.source;
        }
        e.problem = problem;
        targets[key(e.target)]++;
        entries.append(e);
    }
    for (Entry &e : entries)
        if (e.problem.isEmpty() && targets.value(key(e.target)) > 1)
            e.problem = QStringLiteral("duplicate");
    return entries;
}

Result run(const QList<Entry> &entries)
{
    Result result;
    // 1. Schritt: alle Quellen auf eindeutige Zwischennamen
    struct Step { QString source, temp, target; };
    QList<Step> steps;
    const QString tag = QUuid::createUuid().toString(QUuid::Id128).left(8);
    for (const Entry &e : entries) {
        if (!e.problem.isEmpty() || e.unchanged())
            continue;
        const QFileInfo fi(e.source);
        const QString temp = fi.absoluteDir().filePath(QStringLiteral(".kiekan-%1-%2").arg(tag).arg(steps.size()));
        if (!QFile::rename(e.source, temp)) {
            result.errors << QStringLiteral("%1: %2").arg(fi.fileName(), QStringLiteral("cannot rename"));
            continue;
        }
        steps.append({e.source, temp, e.target});
    }
    // 2. Schritt: Zwischennamen auf die Ziele; scheitert eines, zurück zum Original
    for (const Step &s : steps) {
        if (!QFileInfo::exists(s.target) && QFile::rename(s.temp, s.target)) {
            result.renamed.append({s.source, s.target});
        } else {
            QFile::rename(s.temp, s.source);
            result.errors << QStringLiteral("%1 → %2").arg(QFileInfo(s.source).fileName(), QFileInfo(s.target).fileName());
        }
    }
    return result;
}

Result undo(const Result &done)
{
    QList<Entry> back;
    for (const auto &[from, to] : done.renamed)
        back.append({to, from, {}});
    return run(back);
}

} // namespace BatchRename

// KiekAn! – Test: Stapel-Umbenennen (Muster, Vorschau, Tausch, Rücknahme)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BatchRename.h"
#include "Metadata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "OK    " : "FEHLER", what);
    if (!ok)
        ++failures;
}

static void touch(const QString &path, const QByteArray &content)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(content);
}

static QByteArray read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Metadata::initialize();
    QTemporaryDir tmp;
    const QDir d(tmp.path());
    for (const char *n : {"IMG_10.JPG", "IMG_2.JPG", "IMG_1.JPG"})
        touch(d.filePath(QString::fromLatin1(n)), QByteArray(n));

    const QStringList files{d.filePath(QStringLiteral("IMG_10.JPG")), d.filePath(QStringLiteral("IMG_2.JPG")),
                            d.filePath(QStringLiteral("IMG_1.JPG"))};

    BatchRename::Options o;
    o.pattern = QStringLiteral("Urlaub_###");
    auto entries = BatchRename::preview(files, o);
    check(entries.size() == 3, "drei Einträge");
    check(QFileInfo(entries[0].source).fileName() == QLatin1String("IMG_1.JPG")
              && QFileInfo(entries[2].source).fileName() == QLatin1String("IMG_10.JPG"),
          "natürliche Sortierung (1, 2, 10)");
    check(QFileInfo(entries[0].target).fileName() == QLatin1String("Urlaub_001.jpg"), "Muster und kleine Endung");
    check(QFileInfo(entries[2].target).fileName() == QLatin1String("Urlaub_003.jpg"), "Nummer 003");

    check(BatchRename::expand(QStringLiteral("$F-$N"), files[0], 1, {}) == d.dirName() + QStringLiteral("-IMG_10"),
          "$F und $N");

    // Doppelte Namen und unzulässige Zeichen werden erkannt
    o.pattern = QStringLiteral("gleich");
    entries = BatchRename::preview(files, o);
    check(entries[0].problem == QLatin1String("duplicate"), "Doppelung erkannt");
    o.pattern = QStringLiteral("a/b");
    check(BatchRename::preview(files, o)[0].problem == QLatin1String("invalid"), "Schrägstrich abgelehnt");

    // Fremde Datei mit dem Zielnamen wird nicht überschrieben
    touch(d.filePath(QStringLiteral("frei_1.jpg")), "fremd");
    o.pattern = QStringLiteral("frei_#");
    entries = BatchRename::preview(files, o);
    check(entries[0].problem == QLatin1String("exists"), "vorhandene fremde Datei geschützt");

    // Umbenennen und zurück
    o.pattern = QStringLiteral("Urlaub_###");
    const auto result = BatchRename::run(BatchRename::preview(files, o));
    check(result.errors.isEmpty() && result.renamed.size() == 3, "drei Dateien umbenannt");
    check(read(d.filePath(QStringLiteral("Urlaub_001.jpg"))) == "IMG_1.JPG", "Inhalt folgt dem Namen");
    const auto back = BatchRename::undo(result);
    check(back.errors.isEmpty() && QFile::exists(files[0]) && read(files[1]) == "IMG_2.JPG", "Rücknahme");

    // Namen tauschen: a ↔ b in einem Lauf
    touch(d.filePath(QStringLiteral("a.png")), "A");
    touch(d.filePath(QStringLiteral("b.png")), "B");
    QList<BatchRename::Entry> swap{{d.filePath(QStringLiteral("a.png")), d.filePath(QStringLiteral("b.png")), {}},
                                   {d.filePath(QStringLiteral("b.png")), d.filePath(QStringLiteral("a.png")), {}}};
    const auto swapped = BatchRename::run(swap);
    check(swapped.errors.isEmpty() && read(d.filePath(QStringLiteral("a.png"))) == "B", "Tausch a ↔ b");
    check(d.entryList(QStringList{QStringLiteral(".kiekan-*")}, QDir::Files | QDir::Hidden).isEmpty(), "keine Zwischendateien übrig");

    std::printf("\n%s\n", failures ? "Fehler gefunden" : "alle Prüfungen bestanden");
    return failures ? 1 : 0;
}

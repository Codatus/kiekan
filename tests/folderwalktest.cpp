// KiekAn! – Test: über Ordnergrenzen blättern (Reihenfolge vorwärts/rückwärts)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FolderWalk.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "OK    " : "FEHLER", what);
    if (!ok)
        ++failures;
}

static void touch(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString root = tmp.path();
    // Reihe wie im Dateimanager (Ordner vor Dateien):
    // A/a1 A/a2 | B/B1/b11 | B/b1 | E/e1 | x
    touch(root + "/A/a1.jpg");
    touch(root + "/A/a2.jpg");
    touch(root + "/A/notiz.txt");
    touch(root + "/B/B1/b11.jpg");
    touch(root + "/B/b1.jpg");
    QDir().mkpath(root + "/C/leer");
    touch(root + "/D/.versteckt/h.jpg");
    touch(root + "/E/e1.jpg");
    touch(root + "/x.jpg");
    QFile::link(root + "/A", root + "/F-verknuepfung");

    const QStringList filters{QStringLiteral("*.jpg")};
    auto next = [&](const QString &dir, bool forward) {
        const FolderWalk::Result r = FolderWalk::neighbour(root + dir, forward, filters, root);
        return r.dir.isEmpty() ? QString() : r.files.value(r.index).mid(root.size());
    };

    check(next("/A", true) == "/B/B1/b11.jpg", "vorwärts: A → erster Unterordner von B");
    check(next("/B/B1", true) == "/B/b1.jpg", "vorwärts: B1 → Dateien von B");
    check(next("/B", true) == "/E/e1.jpg", "vorwärts: B → E (leere und versteckte Ordner übersprungen)");
    check(next("/E", true) == "/x.jpg", "vorwärts: E → Dateien des höheren Ordners");
    check(next("", true).isEmpty(), "vorwärts: am Ende nichts mehr");

    check(next("", false) == "/E/e1.jpg", "rückwärts: höherer Ordner → letzter Unterordner");
    check(next("/E", false) == "/B/b1.jpg", "rückwärts: E → letztes Bild in B");
    check(next("/B", false) == "/B/B1/b11.jpg", "rückwärts: B → sein Unterordner B1");
    check(next("/B/B1", false) == "/A/a2.jpg", "rückwärts: B1 → letztes Bild in A");
    check(next("/A", false).isEmpty(), "rückwärts: am Anfang nichts mehr");

    // Liste und Stelle passen zusammen
    const FolderWalk::Result r = FolderWalk::neighbour(root + "/E", false, filters, root);
    check(r.files.size() == 1 && r.index == 0 && r.dir == QDir::cleanPath(root + "/B"), "Ergebnis enthält die Dateien des Ordners");
    const FolderWalk::Result a = FolderWalk::neighbour(root + "/B/B1", false, filters, root);
    check(a.files.size() == 2 && a.index == 1, "rückwärts: Stelle ist das letzte Bild");

    // Grenze: nicht über `top` hinaus
    check(FolderWalk::neighbour(root + "/E", true, filters, root + "/E").dir.isEmpty(), "nicht über die Obergrenze hinaus");

    const QString home = QDir::cleanPath(QDir::homePath());
    check(FolderWalk::topFor(home + "/Bilder/2026/10") == home + "/Bilder", "Obergrenze: Ordner direkt unter dem persönlichen Ordner");
    check(FolderWalk::topFor(home) == home, "Obergrenze im persönlichen Ordner selbst");

    // Suchbudget: viele leere Ordner
    for (int i = 0; i < 50; ++i)
        QDir().mkpath(root + QStringLiteral("/G/leer%1").arg(i));
    touch(root + "/H/h.jpg");
    const FolderWalk::Result g = FolderWalk::neighbour(root + "/E", true, filters, root, 10);
    check(g.dir.isEmpty() && g.gaveUp, "zu viele leere Ordner: Suche gibt auf");
    check(next("/E", true) == "/H/h.jpg", "mit genug Budget: über leere Ordner hinweg");

    std::printf("%s\n", failures ? "Fehlgeschlagen." : "Alles in Ordnung.");
    return failures ? 1 : 0;
}

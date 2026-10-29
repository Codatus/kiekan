// KiekAn! – Test der Dateizuordnung (mimeapps.list) in einem Wegwerf-Ordner
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FileAssoc.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "OK    " : "FEHLER", what);
    if (!ok)
        ++failures;
}

static QString read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

int main(int argc, char **argv)
{
    QTemporaryDir tmp;
    const QString cfg = tmp.path() + QStringLiteral("/config");
    QDir().mkpath(cfg);
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(cfg));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(tmp.path() + QStringLiteral("/leer")));
    qputenv("XDG_DATA_HOME", QFile::encodeName(tmp.path() + QStringLiteral("/data")));
    qputenv("XDG_DATA_DIRS", QFile::encodeName(tmp.path() + QStringLiteral("/leer")));
    qputenv("XDG_CURRENT_DESKTOP", "LXQt");
    QCoreApplication app(argc, argv);

    // Vorhandene Einträge anderer Programme und fremde Abschnitte bleiben erhalten.
    {
        QFile f(cfg + QStringLiteral("/lxqt-mimeapps.list"));
        f.open(QIODevice::WriteOnly);
        f.write("# eigener Kommentar\n[Default Applications]\napplication/pdf=okular.desktop;\nimage/png=gwenview.desktop;\ntext/plain=kate.desktop;\n");
    }

    const QString err = FileAssoc::apply({QStringLiteral("images"), QStringLiteral("raw")},
                                         {QStringLiteral("pdf"), QStringLiteral("video")});
    check(err.isEmpty(), "apply ohne Fehler");
    const QString lx = read(cfg + QStringLiteral("/lxqt-mimeapps.list"));
    const QString mm = read(cfg + QStringLiteral("/mimeapps.list"));
    check(lx.contains(QLatin1String("image/png=kiekan.desktop;gwenview.desktop;")), "PNG: KiekAn! vor Gwenview");
    check(lx.contains(QLatin1String("image/jpeg=kiekan.desktop;")), "JPEG eingetragen");
    check(lx.contains(QLatin1String("image/x-canon-cr2=kiekan.desktop;")), "RAW eingetragen");
    check(lx.contains(QLatin1String("application/pdf=okular.desktop;")), "PDF bleibt bei Okular");
    check(!lx.contains(QLatin1String("video/mp4")), "Videos unverändert");
    check(lx.contains(QLatin1String("text/plain=kate.desktop;")) && lx.startsWith(QLatin1String("# eigener Kommentar")),
          "fremde Einträge und Kommentar erhalten");
    check(mm.contains(QLatin1String("image/jpeg=kiekan.desktop;")), "mimeapps.list ebenfalls gesetzt");

    FileAssoc::Group images;
    for (const FileAssoc::Group &g : FileAssoc::groups())
        if (g.key == QLatin1String("images"))
            images = g;
    check(FileAssoc::isDefault(images), "isDefault(Bilder)");

    // Zurücknehmen: Gwenview ist wieder Standard für PNG, PDF jetzt KiekAn!
    FileAssoc::apply({QStringLiteral("pdf")}, {QStringLiteral("images"), QStringLiteral("raw")});
    const QString lx2 = read(cfg + QStringLiteral("/lxqt-mimeapps.list"));
    check(lx2.contains(QLatin1String("image/png=gwenview.desktop;")), "PNG zurück an Gwenview");
    const QString defaults2 = lx2.section(QStringLiteral("[Added Associations]"), 0, 0);
    check(!defaults2.contains(QLatin1String("image/jpeg=")), "JPEG-Standard entfernt");
    check(lx2.contains(QLatin1String("application/pdf=kiekan.desktop;okular.desktop;")), "PDF auf Wunsch KiekAn!");
    check(lx2.contains(QLatin1String("[Added Associations]")), "PDF als hinzugefügte Zuordnung");
    check(!FileAssoc::isDefault(images), "isDefault(Bilder) jetzt falsch");

    std::printf("\n%s\n", failures ? "Fehler gefunden" : "alle Prüfungen bestanden");
    return failures ? 1 : 0;
}

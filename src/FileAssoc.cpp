// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FileAssoc.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

namespace FileAssoc {

namespace {

const QString kDefaults = QStringLiteral("Default Applications");
const QString kAdded = QStringLiteral("Added Associations");

bool inFlatpak()
{
    return QFile::exists(QStringLiteral("/.flatpak-info"));
}

QString envDir(const char *name, const QString &fallback)
{
    const QString v = qEnvironmentVariable(name);
    return v.isEmpty() ? fallback : v;
}

// Im Flatpak zeigt XDG_CONFIG_HOME in den Sandbox-Ordner; die Zuordnungen
// gelten aber für das ganze System und liegen im echten ~/.config.
QString userConfigDir()
{
    if (inFlatpak())
        return QDir::homePath() + QStringLiteral("/.config");
    return envDir("XDG_CONFIG_HOME", QDir::homePath() + QStringLiteral("/.config"));
}

QStringList currentDesktops()
{
    QStringList out;
    for (const QString &d : qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QLatin1Char(':'), Qt::SkipEmptyParts))
        out << d.toLower();
    return out;
}

// Suchreihenfolge der Spezifikation: je Ordner erst die Datei des Desktops
// (z. B. lxqt-mimeapps.list), dann mimeapps.list.
QStringList lookupFiles()
{
    QStringList dirs{userConfigDir()};
    dirs += envDir("XDG_CONFIG_DIRS", QStringLiteral("/etc/xdg")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    dirs << envDir("XDG_DATA_HOME", QDir::homePath() + QStringLiteral("/.local/share")) + QStringLiteral("/applications");
    for (const QString &d : envDir("XDG_DATA_DIRS", QStringLiteral("/usr/local/share:/usr/share")).split(QLatin1Char(':'), Qt::SkipEmptyParts))
        dirs << d + QStringLiteral("/applications");
    QStringList files;
    for (const QString &dir : dirs) {
        for (const QString &desk : currentDesktops())
            files << dir + QLatin1Char('/') + desk + QStringLiteral("-mimeapps.list");
        files << dir + QStringLiteral("/mimeapps.list");
    }
    return files;
}

// Einfaches INI-Modell, das Reihenfolge, Kommentare und fremde Abschnitte
// unverändert lässt (QSettings würde Schlüssel und Strichpunkte umschreiben).
struct ListFile {
    struct Line {
        QString section;   // Abschnitt, zu dem die Zeile gehört
        QString key;       // leer bei Kommentar, Abschnittskopf, Leerzeile
        QString text;
    };
    QList<Line> lines;

    bool load(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        QString section;
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString raw = in.readLine();
            const QString t = raw.trimmed();
            Line l{section, QString(), raw};
            if (t.startsWith(QLatin1Char('[')) && t.endsWith(QLatin1Char(']'))) {
                section = t.mid(1, t.size() - 2);
                l.section = section;
            } else if (!t.isEmpty() && !t.startsWith(QLatin1Char('#'))) {
                const int eq = t.indexOf(QLatin1Char('='));
                if (eq > 0)
                    l.key = t.left(eq).trimmed();
            }
            lines << l;
        }
        return true;
    }

    QStringList values(const QString &section, const QString &key) const
    {
        for (const Line &l : lines)
            if (l.section == section && l.key == key)
                return l.text.mid(l.text.indexOf(QLatin1Char('=')) + 1).trimmed().split(QLatin1Char(';'), Qt::SkipEmptyParts);
        return {};
    }

    void set(const QString &section, const QString &key, const QStringList &vals)
    {
        const QString text = key + QLatin1Char('=') + vals.join(QLatin1Char(';')) + QLatin1Char(';');
        for (int i = 0; i < lines.size(); ++i) {
            if (lines[i].section == section && lines[i].key == key) {
                if (vals.isEmpty())
                    lines.removeAt(i);
                else
                    lines[i].text = text;
                return;
            }
        }
        if (vals.isEmpty())
            return;
        // Ans Ende des Abschnitts anhängen bzw. den Abschnitt anlegen.
        int insertAt = -1;
        for (int i = 0; i < lines.size(); ++i)
            if (lines[i].section == section && !lines[i].text.trimmed().isEmpty())
                insertAt = i + 1;
        if (insertAt < 0) {
            if (!lines.isEmpty() && !lines.last().text.trimmed().isEmpty())
                lines << Line{lines.last().section, QString(), QString()};
            lines << Line{section, QString(), QLatin1Char('[') + section + QLatin1Char(']')};
            insertAt = lines.size();
        }
        lines.insert(insertAt, Line{section, key, text});
    }

    bool save(const QString &path, QString *error) const
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            *error = f.errorString();
            return false;
        }
        QTextStream out(&f);
        for (const Line &l : lines)
            out << l.text << '\n';
        out.flush();
        if (!f.commit()) {
            *error = f.errorString();
            return false;
        }
        return true;
    }
};

QString desktopFilePath(const QString &id)
{
    QString path = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, id);
    if (path.isEmpty() && inFlatpak()) {
        for (const QString &dir : {QStringLiteral("/run/host/usr/share/applications"),
                                   QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share/applications"),
                                   QStringLiteral("/var/lib/flatpak/exports/share/applications")})
            if (QFile::exists(dir + QLatin1Char('/') + id))
                return dir + QLatin1Char('/') + id;
    }
    return path;
}

QString desktopName(const QString &id)
{
    QFile f(desktopFilePath(id));
    QString name;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString localized = QStringLiteral("Name[%1]=").arg(QLocale().name().left(2));
        bool inEntry = false;
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString l = in.readLine().trimmed();
            if (l.startsWith(QLatin1Char('[')))
                inEntry = l == QLatin1String("[Desktop Entry]");
            else if (inEntry && l.startsWith(localized))
                return l.mid(localized.size());
            else if (inEntry && name.isEmpty() && l.startsWith(QLatin1String("Name=")))
                name = l.mid(5);
        }
    }
    if (name.isEmpty()) {
        name = id;
        name.chop(name.endsWith(QLatin1String(".desktop")) ? 8 : 0);
    }
    return name;
}

QString currentDefaultId(const QString &mimeType)
{
    for (const QString &file : lookupFiles()) {
        ListFile lf;
        if (!lf.load(file))
            continue;
        for (const QString &id : lf.values(kDefaults, mimeType))
            if (id == desktopId() || !desktopFilePath(id).isEmpty())
                return id;
    }
    return {};
}

} // namespace

QList<Group> groups()
{
    const auto tr = [](const char *s) { return QCoreApplication::translate("FileAssoc", s); };
    return {
        {QStringLiteral("images"), tr("Bilder (JPEG, PNG, GIF, WebP, TIFF, HEIC, AVIF, SVG, PSD, JPEG 2000, DjVu …)"),
         {QStringLiteral("image/jpeg"), QStringLiteral("image/png"), QStringLiteral("image/gif"), QStringLiteral("image/bmp"),
          QStringLiteral("image/webp"), QStringLiteral("image/tiff"), QStringLiteral("image/svg+xml"), QStringLiteral("image/x-icon"),
          QStringLiteral("image/vnd.microsoft.icon"), QStringLiteral("image/x-portable-pixmap"), QStringLiteral("image/x-portable-graymap"),
          QStringLiteral("image/x-portable-bitmap"), QStringLiteral("image/x-xbitmap"), QStringLiteral("image/x-xpixmap"),
          QStringLiteral("image/x-tga"), QStringLiteral("image/x-icns"), QStringLiteral("image/heic"), QStringLiteral("image/heif"),
          QStringLiteral("image/avif"), QStringLiteral("image/jp2"), QStringLiteral("image/jpx"), QStringLiteral("image/x-jp2-codestream"),
          QStringLiteral("image/vnd.djvu"), QStringLiteral("image/vnd.adobe.photoshop")},
         true},
        {QStringLiteral("raw"), tr("Kamera-RAW (CR2, CR3, NEF, ARW, DNG, RAF, ORF, RW2 …)"),
         {QStringLiteral("image/x-canon-cr2"), QStringLiteral("image/x-canon-cr3"), QStringLiteral("image/x-canon-crw"),
          QStringLiteral("image/x-nikon-nef"), QStringLiteral("image/x-nikon-nrw"), QStringLiteral("image/x-sony-arw"),
          QStringLiteral("image/x-sony-srf"), QStringLiteral("image/x-sony-sr2"), QStringLiteral("image/x-adobe-dng"),
          QStringLiteral("image/x-olympus-orf"), QStringLiteral("image/x-panasonic-rw2"), QStringLiteral("image/x-fuji-raf"),
          QStringLiteral("image/x-pentax-pef"), QStringLiteral("image/x-samsung-srw"), QStringLiteral("image/x-sigma-x3f"),
          QStringLiteral("image/x-minolta-mrw"), QStringLiteral("image/x-kodak-dcr"), QStringLiteral("image/x-kodak-kdc"),
          QStringLiteral("image/x-epson-erf"), QStringLiteral("image/x-dcraw")},
         true},
        {QStringLiteral("pdf"), tr("PDF-Dokumente"), {QStringLiteral("application/pdf")}, false},
        {QStringLiteral("video"), tr("Videos (MP4, MKV, WebM, MOV, AVI …)"),
         {QStringLiteral("video/mp4"), QStringLiteral("video/x-m4v"), QStringLiteral("video/x-matroska"), QStringLiteral("video/webm"),
          QStringLiteral("video/quicktime"), QStringLiteral("video/x-msvideo"), QStringLiteral("video/x-ms-wmv"),
          QStringLiteral("video/x-flv"), QStringLiteral("video/mpeg"), QStringLiteral("video/3gpp"), QStringLiteral("video/ogg"),
          QStringLiteral("video/mp2t")},
         false},
    };
}

bool supported()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
}

QString desktopId()
{
    if (inFlatpak()) {
        const QString id = qEnvironmentVariable("FLATPAK_ID");
        if (!id.isEmpty())
            return id + QStringLiteral(".desktop");
    }
    return QStringLiteral("kiekan.desktop");
}

QString currentDefault(const QString &mimeType)
{
    const QString id = currentDefaultId(mimeType);
    return id.isEmpty() ? QString() : desktopName(id);
}

bool isDefault(const Group &group)
{
    for (const QString &m : group.mimeTypes)
        if (currentDefaultId(m) != desktopId())
            return false;
    return true;
}

QStringList userListFiles()
{
    const QString dir = userConfigDir();
    QStringList files;
    // Eine vorhandene Desktop-Datei (z. B. lxqt-mimeapps.list) hat Vorrang
    // vor mimeapps.list und muss deshalb mitgeändert werden.
    for (const QString &desk : currentDesktops()) {
        const QString f = dir + QLatin1Char('/') + desk + QStringLiteral("-mimeapps.list");
        if (QFile::exists(f))
            files << f;
    }
    files << dir + QStringLiteral("/mimeapps.list");
    return files;
}

QString apply(const QStringList &on, const QStringList &off)
{
    if (!supported())
        return QCoreApplication::translate("FileAssoc", "Auf diesem System legt man Standardprogramme in den Systemeinstellungen fest.");
    const QString self = desktopId();
    for (const QString &file : userListFiles()) {
        ListFile lf;
        lf.load(file);   // fehlt die Datei, wird sie angelegt
        for (const Group &g : groups()) {
            const bool set = on.contains(g.key);
            if (!set && !off.contains(g.key))
                continue;
            for (const QString &m : g.mimeTypes) {
                QStringList defaults = lf.values(kDefaults, m);
                QStringList added = lf.values(kAdded, m);
                if (set) {
                    defaults.removeAll(self);
                    defaults.prepend(self);
                    // PDF und Videos stehen nicht in kiekan.desktop; erst die
                    // „hinzugefügte Zuordnung“ macht KiekAn! dafür wählbar.
                    if (!added.contains(self))
                        added.prepend(self);
                    lf.set(kAdded, m, added);
                } else if (defaults.value(0) == self) {
                    defaults.removeFirst();
                }
                lf.set(kDefaults, m, defaults);
            }
        }
        QString error;
        if (!lf.save(file, &error))
            return QStringLiteral("%1: %2").arg(file, error);
    }
    // KDE liest Zuordnungen aus einem Zwischenspeicher.
    if (currentDesktops().contains(QStringLiteral("kde")))
        QProcess::startDetached(QStringLiteral("kbuildsycoca6"), {});
    return {};
}

} // namespace FileAssoc

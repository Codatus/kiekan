// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FilterRegistry.h"
#include "ImageLoader.h"
#include "MainWindow.h"
#include "McpServer.h"
#include "OcrWorker.h"
#include "Metadata.h"
#include "SettingsDialog.h"

#include <QApplication>
#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

#include <clocale>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {

// Unter Windows stdin/stdout ohne Zeilenende-Umwandlung (MCP und
// OCR-Hilfsprozess sprechen JSON über diese Kanäle).
void binaryStdio()
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

// Mitgelieferte Tesseract-Sprachdaten (Windows-Installation, portable
// Fassung) bevorzugen, sofern nichts anderes eingestellt ist.
void useBundledTessdata()
{
    if (!qEnvironmentVariableIsEmpty("TESSDATA_PREFIX"))
        return;
    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/tessdata");
    if (QFileInfo::exists(dir + QStringLiteral("/osd.traineddata")) || QFileInfo::exists(dir + QStringLiteral("/eng.traineddata")))
        qputenv("TESSDATA_PREFIX", QFile::encodeName(dir));
}

// Eigene Bildformat-Plugins (HEIC, RAW, JPEG 2000, DjVu, PSD) finden:
// im Build neben dem Programm, installiert unter lib/kiekan/qtplugins.
void addPluginPaths()
{
    const QString dir = QCoreApplication::applicationDirPath();
    QCoreApplication::addLibraryPath(dir + QStringLiteral("/qtplugins"));
    QCoreApplication::addLibraryPath(QDir::cleanPath(dir + QStringLiteral("/../lib/kiekan/qtplugins")));
}

void setIdentity()
{
    QCoreApplication::setApplicationName(QStringLiteral("kiekan"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KIEKAN_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("kiekan"));
}

// KI-Schnittstelle: ohne Fenster, Nachrichten über stdin/stdout.
int runMcpServer(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    binaryStdio();
    addPluginPaths();
    useBundledTessdata();
    setIdentity();
    std::setlocale(LC_NUMERIC, "C");
    Metadata::initialize();
    QImageReader::setAllocationLimit(4096);
    ImageLoader::setAutoRotate(Settings::autoRotate());
    FilterRegistry::instance().loadPlugins();

    McpServer server;
    server.start();
    return app.exec();
}

} // namespace

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--mcp") == 0)
            return runMcpServer(argc, argv);
        if (qstrcmp(argv[i], "--ocr-worker") == 0) {
            // Hilfsprozess für die Texterkennung (siehe OcrWorker.h)
            if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
                qputenv("QT_QPA_PLATFORM", "offscreen");
            QGuiApplication app(argc, argv);
            binaryStdio();
            addPluginPaths();
            useBundledTessdata();
            setIdentity();
            std::setlocale(LC_NUMERIC, "C");
            QImageReader::setAllocationLimit(4096);
            ImageLoader::setAutoRotate(Settings::autoRotate());
            return OcrWorker::workerMain();
        }
    }

    QApplication app(argc, argv);
    addPluginPaths();
    useBundledTessdata();
    // libmpv verlangt den C-Zahlenformat-Standard (Qt setzt das Gebietsschema
    // der Umgebung, z. B. mit Komma als Dezimaltrenner).
    std::setlocale(LC_NUMERIC, "C");
    Metadata::initialize();
    setIdentity();
    QApplication::setApplicationDisplayName(QStringLiteral("KiekAn!"));
    QApplication::setDesktopFileName(QStringLiteral("kiekan"));
    // Ohne Desktop-Integration (z. B. GNOME ohne Qt-Theme) kennt Qt kein
    // Icon-Thema; dann ein vorhandenes vollständiges Thema nehmen.
    // KIEKAN_ICON_THEME erzwingt ein Thema („none“: eingebaute Ersatz-Icons).
    if (qEnvironmentVariableIsSet("KIEKAN_ICON_THEME")) {
        const QString forced = qEnvironmentVariable("KIEKAN_ICON_THEME");
        QIcon::setThemeName(forced == QLatin1String("none") ? QStringLiteral("kiekan-none") : forced);
    } else if (QIcon::themeName().isEmpty() || QIcon::themeName() == QLatin1String("hicolor")) {
        for (const char *theme : {"Papirus", "breeze", "Yaru", "Adwaita", "Humanity", "oxygen"}) {
            const QString name = QString::fromLatin1(theme);
            if (QFileInfo::exists(QStringLiteral("/usr/share/icons/%1/index.theme").arg(name))) {
                QIcon::setThemeName(name);
                break;
            }
        }
    }
    // Kleine Größen: vereinfachter Fisch. Als Pixmaps, denn eine zweite
    // SVG-Datei würde im SVG-Icon-Engine die große ersetzen.
    QIcon appIcon(QStringLiteral(":/kiekan.svg"));
    const QIcon smallIcon(QStringLiteral(":/kiekan-klein.svg"));
    for (int size : {16, 22, 24, 32})
        appIcon.addPixmap(smallIcon.pixmap(size));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("kiekan"), appIcon));

    // Qt begrenzt Bilder standardmäßig auf 256 MB im Speicher; große Fotos
    // und Panoramen sollen trotzdem aufgehen.
    QImageReader::setAllocationLimit(4096);

    // Sprache: Die Texte im Programm sind deutsch; für andere Sprachen wird
    // eine Übersetzung geladen. Ohne Einstellung gilt die Systemsprache,
    // wobei nicht-deutsche Systeme Englisch bekommen.
    QString lang = Settings::language();
    if (lang.isEmpty())
        lang = QLocale::system().language() == QLocale::German ? QStringLiteral("de") : QStringLiteral("en");
    const QLocale locale(lang);
    QLocale::setDefault(locale);

    QTranslator appTranslator;
    if (lang != QLatin1String("de")
        && appTranslator.load(QStringLiteral(":/i18n/kiekan_%1.qm").arg(lang)))
        QApplication::installTranslator(&appTranslator);

    // Standard-Dialogtexte von Qt (Knöpfe usw.)
    // Linux: qtbase_de.qm im Qt-Verzeichnis; Windows-Paket: windeployqt fasst alles
    // in translations/qt_de.qm neben der Programmdatei zusammen.
    QTranslator qtTranslator;
    const QString qtDir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    const QString appDir = QCoreApplication::applicationDirPath() + QStringLiteral("/translations");
    if (qtTranslator.load(locale, QStringLiteral("qtbase"), QStringLiteral("_"), qtDir)
        || qtTranslator.load(locale, QStringLiteral("qt"), QStringLiteral("_"), qtDir)
        || qtTranslator.load(locale, QStringLiteral("qt"), QStringLiteral("_"), appDir))
        QApplication::installTranslator(&qtTranslator);

    MainWindow window;
    window.show();

    const QStringList args = QApplication::arguments();
    if (args.size() > 1)
        window.openPath(args.at(1));

    return app.exec();
}

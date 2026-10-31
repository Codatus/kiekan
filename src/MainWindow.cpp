// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"

#include "BatchDialog.h"
#include "LosslessJpeg.h"
#include "NaturalSort.h"
#include "ShortcutDialog.h"

#include "AiLogDialog.h"
#include "CurvesDialog.h"
#include "EditOps.h"
#include "ExifDialog.h"
#include "FilterDialog.h"
#include "FilterRegistry.h"
#include "HistogramDialog.h"
#include "ImageLoader.h"
#include "ImageOps.h"
#include "Icons.h"
#include "ImageView.h"
#include "OcrDialog.h"
#include "PdfSplit.h"
#include "PdfSplitDialog.h"
#include "Media.h"
#include "Metadata.h"
#include "ResizeDialog.h"
#include "SettingsDialog.h"
#include "ViewerBridge.h"
#ifdef KIEKAN_HAVE_MPV
#include "VideoView.h"
#endif

#ifdef KIEKAN_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>

#include <fcntl.h>
#include <unistd.h>
#endif

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QPushButton>
#include <QFormLayout>
#include <QTimer>
#ifdef KIEKAN_HAVE_PRINT
#include <QPageLayout>
#include <QPrintPreviewDialog>
#include <QPrinter>
#endif
#include <QDateTime>
#include <QDir>
#include <QBuffer>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMovie>
#include <QPainter>
#include <QPushButton>
#include <QRunnable>
#include <QSaveFile>
#include <QSettings>
#include <QStackedWidget>
#include <QThread>
#include <QStatusBar>
#include <QToolBar>

#include <algorithm>
#include <memory>

namespace {

constexpr int kPanStep = 64;
constexpr qsizetype kUndoBudget = 512LL * 1024 * 1024;   // Bytes
const QColor kViewBackground(0x2b, 0x2b, 0x2b);

QByteArray formatForSuffix(const QString &suffix)
{
    const QString s = suffix.toLower();
    if (s == QLatin1String("jpg") || s == QLatin1String("jpeg") || s == QLatin1String("jpe"))
        return "jpeg";
    if (s == QLatin1String("tif") || s == QLatin1String("tiff"))
        return "tiff";
    return s.toLatin1();
}

bool formatHasAlpha(const QByteArray &format)
{
    return format == "png" || format == "webp" || format == "tiff" || format == "ico";
}

QString humanSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace

namespace {

// Papierkorb. Im Flatpak über das Portal: QFile::moveToTrash nähme dort den
// Papierkorb in ~/.var/app/…, den der Dateimanager nicht zeigt.
bool moveToTrash(const QString &path)
{
#ifdef KIEKAN_HAVE_DBUS
    if (!qEnvironmentVariableIsEmpty("FLATPAK_ID")) {
        const int fd = ::open(QFile::encodeName(path).constData(), O_PATH | O_CLOEXEC);
        if (fd < 0)
            return false;
        QDBusMessage call = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.Trash"), QStringLiteral("TrashFile"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(fd));   // dupliziert fd
        ::close(fd);
        const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 10000);
        return reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()
               && reply.arguments().constFirst().toUInt() == 1;
    }
#endif
    return QFile::moveToTrash(path);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_view = new ImageView(this);
    m_view->setBackground(kViewBackground);
    // Bild und Video teilen sich die Fläche; der Videoplayer wird erst beim
    // ersten Video erzeugt, damit der Start schnell bleibt.
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_view);
    setCentralWidget(m_stack);

    m_loader = new ImageLoader(this);
    connect(m_loader, &ImageLoader::loaded, this, &MainWindow::onLoaded);

    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel, 1);

    connect(m_view, &ImageView::zoomChanged, this, &MainWindow::updateStatus);
    // Über die Auflösung einer RAW-Vorschau hinaus zoomen: voll entwickeln.
    connect(m_view, &ImageView::zoomChanged, this, [this](double zoom) {
        if (m_rawPreview && !m_developing && !m_view->isFitMode() && zoom > 1.0)
            developRaw();
    });
    connect(m_view, &ImageView::selectionChanged, this, &MainWindow::updateStatus);
    connect(m_view, &ImageView::navigateRequested, this, &MainWindow::step);
    connect(m_view, &ImageView::doubleClicked, this, &MainWindow::toggleFullScreen);
    connect(m_view, &ImageView::contextMenuRequested, this, &MainWindow::showContextMenu);

    connect(m_view, &ImageView::imageChanged, this, &MainWindow::refreshHistogram);

    m_scanPool.setMaxThreadCount(1);

    ImageLoader::setAutoRotate(Settings::autoRotate());
    FilterRegistry::instance().loadPlugins();

    createActions();
    setAcceptDrops(true);

    m_viewerServer = new ViewerServer([this](const QJsonObject &req) { return handleViewerRequest(req); }, this);
    updateViewerServer();

    QSettings settings;
    if (!restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray()))
        resize(1100, 780);

    updateTitle();
    updateStatus();
}

MainWindow::~MainWindow() = default;

// --- Aktionen und Menüs ----------------------------------------------------

void MainWindow::createActions()
{
    // Jede Aktion wird auch direkt am Fenster registriert, damit ihre
    // Tastenkürzel im Vollbild (ohne Menüleiste) weiter funktionieren.
    // `id`: feste Kennung für eigene Tastenbelegungen (shortcuts/<id>)
    auto make = [this](const QString &id, const QString &text, const char *icon,
                       const QList<QKeySequence> &keys, auto slot) {
        auto *a = new QAction(Icons::get(QString::fromLatin1(icon)), text, this);
        a->setObjectName(id);
        a->setShortcuts(keys);
        m_actions.append(a);
        m_defaultShortcuts.insert(id, keys);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };
    using K = QKeySequence;

    QAction *open = make(QStringLiteral("open"), tr("&Öffnen …"), "document-open", {K(Qt::Key_O), K::Open}, [this] { openDialog(); });
    QAction *saveAct = make(QStringLiteral("saveAct"), tr("&Speichern"), "document-save", {K::Save}, [this] { save(); });
    QAction *saveAsAct = make(QStringLiteral("saveAsAct"), tr("Speichern &unter …"), "document-save-as", {K(Qt::Key_S)}, [this] { saveAs(); });
    QAction *rename = make(QStringLiteral("rename"), tr("&Umbenennen …"), "edit-rename", {K(Qt::Key_F2)}, [this] { renameFile(); });
    QAction *batch = make(QStringLiteral("batch"), tr("&Stapelverarbeitung …"), "", {K(Qt::Key_B)}, [this] { batchDialog(); });
    QAction *del = make(QStringLiteral("del"), tr("In den &Papierkorb"), "edit-delete", {K(Qt::Key_Delete)}, [this] { deleteFile(); });
    QAction *info = make(QStringLiteral("info"), tr("Bild&informationen"), "document-properties", {K(Qt::Key_I)}, [this] { showInfo(); });
    QAction *printAct = make(QStringLiteral("printAct"), tr("&Drucken …"), "document-print", {K::Print}, [this] { print(); });
    QAction *slideshowAct = make(QStringLiteral("slideshowAct"), tr("&Diashow …"), "media-playback-start", {K(Qt::Key_W)}, [this] { startSlideshow(); });
    QAction *quit = make(QStringLiteral("quit"), tr("&Beenden"), "application-exit", {K(Qt::CTRL | Qt::Key_Q)}, [this] { close(); });

    QAction *undoAct = make(QStringLiteral("undoAct"), tr("&Rückgängig"), "edit-undo", {K::Undo}, [this] { undo(); });
    QAction *copyAct = make(QStringLiteral("copyAct"), tr("&Kopieren"), "edit-copy", {K::Copy}, [this] { copy(); });
    QAction *pasteAct = make(QStringLiteral("pasteAct"), tr("&Einfügen"), "edit-paste", {K::Paste}, [this] { paste(); });
    QAction *selAll = make(QStringLiteral("selAll"), tr("&Alles auswählen"), "edit-select-all", {K::SelectAll}, [this] { m_view->selectAll(); });
    QAction *cropAct = make(QStringLiteral("cropAct"), tr("Auswahl &zuschneiden"), "transform-crop", {K(Qt::CTRL | Qt::Key_Y)}, [this] { crop(); });

    QAction *rotR = make(QStringLiteral("rotR"), tr("Nach &rechts drehen"), "object-rotate-right", {K(Qt::Key_R)}, [this] { rotate(90); });
    QAction *rotL = make(QStringLiteral("rotL"), tr("Nach &links drehen"), "object-rotate-left", {K(Qt::Key_L)}, [this] { rotate(-90); });
    QAction *flipH = make(QStringLiteral("flipH"), tr("&Horizontal spiegeln"), "object-flip-horizontal", {K(Qt::Key_H)}, [this] { flip(true); });
    QAction *flipV = make(QStringLiteral("flipV"), tr("&Vertikal spiegeln"), "object-flip-vertical", {K(Qt::Key_V)}, [this] { flip(false); });
    QAction *resizeAct = make(QStringLiteral("resizeAct"), tr("&Größe ändern …"), "transform-scale", {K(Qt::CTRL | Qt::Key_R)}, [this] { resizeImage(); });

    QAction *gray = make(QStringLiteral("gray"), tr("In &Graustufen umwandeln"), "color-mode-black-white", {K(Qt::CTRL | Qt::Key_G)},
                         [this] { applyToImage(ImageOps::grayscale); });
    QAction *bw = make(QStringLiteral("bw"), tr("In &Schwarzweiß umwandeln …"), "", {K(Qt::CTRL | Qt::SHIFT | Qt::Key_B)},
                       [this] { openFilterDialog({FilterRegistry::instance().find(QStringLiteral("schwarzweiss"))}, nullptr); });
    QAction *negative = make(QStringLiteral("negative"), tr("&Negativ"), "edit-select-invert", {K(Qt::CTRL | Qt::SHIFT | Qt::Key_N)},
                             [this] { applyToImage(ImageOps::invert); });
    QAction *autoCol = make(QStringLiteral("autoCol"), tr("&Automatische Farbkorrektur"), "", {K(Qt::SHIFT | Qt::Key_U)}, [this] { autoColors(); });
    QAction *equalize = make(QStringLiteral("equalize"), tr("Histogramm &ausgleichen"), "", {K(Qt::CTRL | Qt::SHIFT | Qt::Key_E)},
                             [this] { equalizeHistogram(); });
    QAction *colors = make(QStringLiteral("colors"), tr("&Farben anpassen …"), "color-management", {K(Qt::SHIFT | Qt::Key_G)},
                           [this] { openFilterDialog({FilterRegistry::instance().find(QStringLiteral("farben"))}, nullptr); });
    QAction *curves = make(QStringLiteral("curves"), tr("Gradations&kurven …"), "", {K(Qt::CTRL | Qt::Key_M)}, [this] { openCurves(); });
    QAction *sharpenQuick = make(QStringLiteral("sharpenQuick"), tr("Sc&härfen"), "", {K(Qt::SHIFT | Qt::Key_S)},
                                 [this] { runFilter(QStringLiteral("schaerfen")); });
    QAction *sharpen = make(QStringLiteral("sharpen"), tr("Schärfen (Einstellungen) …"), "", {},
                            [this] { openFilterDialog({FilterRegistry::instance().find(QStringLiteral("schaerfen"))}, nullptr); });
    QAction *blur = make(QStringLiteral("blur"), tr("&Weichzeichnen …"), "", {},
                         [this] { openFilterDialog({FilterRegistry::instance().find(QStringLiteral("weichzeichnen"))}, nullptr); });
    QAction *effects = make(QStringLiteral("effects"), tr("&Effekte-Browser …"), "", {K(Qt::CTRL | Qt::Key_E)},
                            [this] { openFilterDialog(FilterRegistry::instance().effects(), nullptr); });
    QAction *histogram = make(QStringLiteral("histogram"), tr("&Histogramm"), "", {K(Qt::SHIFT | Qt::Key_H)}, [this] { toggleHistogram(); });
    QAction *settingsAct = make(QStringLiteral("settingsAct"), tr("&Einstellungen …"), "preferences-system", {K(Qt::Key_P)}, [this] { showSettings(); });
    QAction *shortcutsAct = make(QStringLiteral("shortcutsAct"), tr("&Tastenkürzel anpassen …"), "configure-shortcuts", {},
                                 [this] { customizeShortcuts(); });
    QAction *plugins = make(QStringLiteral("plugins"), tr("&Plugins …"), "", {}, [this] { showPlugins(); });
    QAction *aiLog = make(QStringLiteral("aiLog"), tr("&KI-Protokoll …"), "", {}, [this] { showAiLog(); });

    QAction *exifAct = make(QStringLiteral("exifAct"), tr("&EXIF-Daten …"), "", {K(Qt::Key_E)}, [this] { showExif(); });
    QAction *developAct = make(QStringLiteral("developAct"), tr("RAW voll &entwickeln"), "", {K(Qt::CTRL | Qt::Key_D)}, [this] { developRaw(); });
    QAction *splitAct = make(QStringLiteral("splitAct"), tr("PDF &teilen …"), "", {}, [this] { splitPdf(); });
    QAction *ocrAct = make(QStringLiteral("ocrAct"), tr("&Text erkennen (OCR) …"), "", {K(Qt::CTRL | Qt::Key_T)}, [this] { recognizeText(); });

    // Leertaste: bei Videos Pause, sonst nächstes Bild.
    make(QStringLiteral("space"), tr("Leertaste"), "", {K(Qt::Key_Space)}, [this] {
        if (m_slideshow) {
            // Diashow anhalten / weiter
            if (m_slideTimer->isActive()) {
                m_slideTimer->stop();
                message(tr("Diashow angehalten – Leertaste setzt fort, Esc beendet."));
            } else {
                m_slideTimer->start();
                message(tr("Diashow läuft weiter."));
            }
            return;
        }
#ifdef KIEKAN_HAVE_MPV
        if (m_kind == Kind::Video) {
            m_video->togglePause();
            return;
        }
#endif
        step(1);
    });
    QAction *next = make(QStringLiteral("next"), tr("&Nächstes Bild"), "go-next", {K(Qt::Key_PageDown)}, [this] { step(1); });
    QAction *nextPage = make(QStringLiteral("nextPage"), tr("Nächste &Seite"), "", {K(Qt::CTRL | Qt::Key_PageDown)}, [this] { pageStep(1); });
    QAction *prevPage = make(QStringLiteral("prevPage"), tr("Vorherige S&eite"), "", {K(Qt::CTRL | Qt::Key_PageUp)}, [this] { pageStep(-1); });
    QAction *mute = make(QStringLiteral("mute"), tr("Ton &aus/an"), "", {K(Qt::Key_M)}, [this] {
#ifdef KIEKAN_HAVE_MPV
        if (m_kind == Kind::Video)
            m_video->toggleMute();
#endif
    });
    QAction *prev = make(QStringLiteral("prev"), tr("&Vorheriges Bild"), "go-previous", {K(Qt::Key_Backspace), K(Qt::Key_PageUp)}, [this] { step(-1); });
    QAction *firstAct = make(QStringLiteral("firstAct"), tr("&Erstes Bild"), "go-first", {K(Qt::Key_Home)}, [this] { first(); });
    QAction *lastAct = make(QStringLiteral("lastAct"), tr("&Letztes Bild"), "go-last", {K(Qt::Key_End)}, [this] { last(); });

    QAction *zoomIn = make(QStringLiteral("zoomIn"), tr("Ver&größern"), "zoom-in",
                           {K(Qt::Key_Plus), K(Qt::Key_Equal), K(Qt::KeypadModifier | Qt::Key_Plus)},
                           [this] { m_view->zoomIn(); });
    QAction *zoomOut = make(QStringLiteral("zoomOut"), tr("Ver&kleinern"), "zoom-out",
                            {K(Qt::Key_Minus), K(Qt::KeypadModifier | Qt::Key_Minus)},
                            [this] { m_view->zoomOut(); });
    QAction *zoom100 = make(QStringLiteral("zoom100"), tr("&Originalgröße (100 %)"), "zoom-original", {K(Qt::CTRL | Qt::Key_H)}, [this] { m_view->setZoom(1.0); });
    QAction *zoomFit = make(QStringLiteral("zoomFit"), tr("An &Fenster anpassen"), "zoom-fit-best", {K(Qt::Key_F)}, [this] { m_view->setFitMode(); });
    QAction *full = make(QStringLiteral("full"), tr("&Vollbild"), "view-fullscreen",
                         {K(Qt::Key_Return), K(Qt::Key_Enter), K(Qt::Key_F11)},
                         [this] { toggleFullScreen(); });
    make(QStringLiteral("escape"), tr("Abbrechen / Beenden"), "", {K(Qt::Key_Escape)}, [this] { escape(); });

    QAction *help = make(QStringLiteral("help"), tr("&Tastenkürzel"), "help-contents", {K(Qt::Key_F1)}, [this] { showHelp(); });
    QAction *about = make(QStringLiteral("about"), tr("Über &KiekAn!"), "help-about", {}, [this] { showAbout(); });

    QMenu *file = menuBar()->addMenu(tr("&Datei"));
    file->addActions({open, saveAct, saveAsAct});
    file->addSeparator();
    file->addActions({rename, del, info, exifAct, ocrAct, splitAct});
    file->addSeparator();
    file->addAction(batch);
    file->addAction(printAct);
    file->addSeparator();
    file->addAction(quit);

    QMenu *edit = menuBar()->addMenu(tr("&Bearbeiten"));
    edit->addAction(undoAct);
    edit->addSeparator();
    edit->addActions({copyAct, pasteAct, selAll, cropAct});

    QMenu *image = menuBar()->addMenu(tr("B&ild"));
    image->addActions({rotL, rotR, flipH, flipV});
    image->addSeparator();
    image->addAction(resizeAct);
    image->addAction(developAct);
    image->addSeparator();
    image->addActions({gray, bw, negative});
    image->addSeparator();
    image->addActions({autoCol, equalize, colors, curves});
    image->addSeparator();
    image->addActions({sharpenQuick, sharpen, blur});

    QMenu *effectsMenu = menuBar()->addMenu(tr("&Effekte"));
    effectsMenu->addAction(effects);
    effectsMenu->addSeparator();
    for (const Kiekan::Filter *f : FilterRegistry::instance().effects()) {
        effectsMenu->addAction(f->name() + QStringLiteral(" …"), this, [this, f] {
            openFilterDialog(FilterRegistry::instance().effects(), f);
        });
    }

    QMenu *view = menuBar()->addMenu(tr("&Ansicht"));
    view->addActions({next, prev, firstAct, lastAct});
    view->addActions({nextPage, prevPage});
    view->addSeparator();
    view->addActions({zoomIn, zoomOut, zoom100, zoomFit});
    view->addSeparator();
    view->addActions({full, slideshowAct, histogram, mute});

    QMenu *extras = menuBar()->addMenu(tr("E&xtras"));
    extras->addActions({settingsAct, shortcutsAct, plugins});
    extras->addSeparator();
    extras->addAction(aiLog);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Hilfe"));
    helpMenu->addActions({help, about});

    m_toolBar = addToolBar(tr("Werkzeugleiste"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->addActions({open, saveAsAct});
    m_toolBar->addSeparator();
    m_toolBar->addActions({prev, next});
    m_toolBar->addSeparator();
    m_toolBar->addActions({zoomIn, zoomOut, zoom100, zoomFit});
    m_toolBar->addSeparator();
    m_toolBar->addActions({rotL, rotR, cropAct});
    m_toolBar->addSeparator();
    m_toolBar->addActions({del, info, full});

    m_contextMenu = new QMenu(this);
    m_contextMenu->addActions({open, saveAsAct, copyAct, pasteAct});
    m_contextMenu->addSeparator();
    m_contextMenu->addActions({rotL, rotR, cropAct, resizeAct, colors, effects});
    m_contextMenu->addSeparator();
    m_contextMenu->addActions({zoom100, zoomFit, full});
    m_contextMenu->addSeparator();
    m_contextMenu->addActions({rename, del, info, exifAct, ocrAct});

    ShortcutDialog::apply(m_actions, m_defaultShortcuts);
}

void MainWindow::customizeShortcuts()
{
    ShortcutDialog dlg(m_actions, m_defaultShortcuts, this);
    dlg.exec();
}

// --- Öffnen und Blättern ---------------------------------------------------

void MainWindow::openPath(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) {
        message(tr("Nicht gefunden: %1").arg(path));
        return;
    }
    if (!confirmDiscard())
        return;
    m_modified = false;

    if (fi.isDir()) {
        m_files.clear();
        m_index = -1;
        m_path.clear();
        m_loadError.clear();
        resetEdits();
        m_view->clear();
        scanDirectory(fi.absoluteFilePath(), QString());
        return;
    }

    // Das gewählte Bild sofort laden, den Ordner parallel einlesen.
    const QString abs = fi.absoluteFilePath();
    m_files = QStringList{abs};
    showIndex(0);
    scanDirectory(fi.absolutePath(), abs);
}

void MainWindow::scanDirectory(const QString &dir, const QString &select)
{
    const quint64 generation = ++m_scanGeneration;
    const QStringList filters = Media::allNameFilters();
    m_scanPool.start(QRunnable::create([this, generation, dir, select, filters] {
        QDir d(dir);
        QStringList names = d.entryList(filters, QDir::Files | QDir::Readable);
        std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) {
            return naturalCompare(a, b) < 0;
        });
        QStringList files;
        files.reserve(names.size());
        for (const QString &n : names)
            files << d.absoluteFilePath(n);
        QMetaObject::invokeMethod(this, [this, generation, files, select] {
            onScanned(generation, files, select);
        }, Qt::QueuedConnection);
    }));
}

void MainWindow::onScanned(quint64 generation, const QStringList &files, const QString &select)
{
    if (generation != m_scanGeneration)
        return;

    QStringList list = files;
    // Inzwischen angezeigtes Bild beibehalten (kann sich während des
    // Einlesens geändert haben, z. B. durch schnelles Blättern).
    const QString current = (m_index >= 0 && m_index < m_files.size()) ? m_files.at(m_index) : select;

    if (!current.isEmpty() && !list.contains(current)) {
        // Datei mit unbekannter Endung ausdrücklich geöffnet: einsortieren.
        const QString name = QFileInfo(current).fileName();
        auto pos = std::lower_bound(list.begin(), list.end(), current, [&](const QString &a, const QString &) {
            return naturalCompare(QFileInfo(a).fileName(), name) < 0;
        });
        list.insert(pos, current);
    }

    m_files = list;
    if (current.isEmpty()) {
        if (!m_files.isEmpty())
            showIndex(0);
        else
            message(tr("Keine Bilder oder Videos in diesem Ordner."));
    } else {
        m_index = m_files.indexOf(current);
        prefetch();
    }
    updateStatus();
}

void MainWindow::showIndex(int index)
{
    if (index < 0 || index >= m_files.size())
        return;
    if (!confirmDiscard())
        return;

    stopMedia();
    m_index = index;
    m_path = m_files.at(index);
    m_page = 0;
    m_pageCount = 1;
    m_modified = false;
    resetEdits();
    m_loadError.clear();

    // Teil einer mit KiekAn! geteilten PDF? Dann Originalseiten anzeigen.
    m_partOf = m_partNumber = m_partFirstPage = 0;
    if (Media::isPdf(m_path)) {
        const PdfSplit::PartInfo part = PdfSplit::partInfo(m_path);
        m_partOf = part.of;
        m_partNumber = part.part;
        m_partFirstPage = part.firstPage;
    }

    if (Media::isVideo(m_path)) {
        showVideo();
    } else {
        m_kind = Kind::Image;
        m_stack->setCurrentWidget(m_view);
        showPage();
    }
    prefetch();
    updateTitle();
    updateStatus();
}

void MainWindow::showPage()
{
    if (m_loader->contains(m_path, m_page)) {
        applyLoadResult(m_loader->result(m_path, m_page));
    } else {
        // Altes Bild stehen lassen, bis das neue fertig ist – das wirkt
        // ruhiger als ein kurzes Aufblitzen des leeren Hintergrunds.
        m_loader->request(m_path, m_page);
    }
}

void MainWindow::applyLoadResult(const LoadResult &r)
{
    stopAnimation();
    m_pageCount = r.pageCount;
    if (m_page >= m_pageCount)
        m_page = std::max(0, m_pageCount - 1);   // z. B. viewer_show mit zu großer Seite: geladen wurde die letzte
    m_rawPreview = r.rawPreview;
    m_rawFullSize = r.rawFullSize;
    if (r.image.isNull()) {
        m_loadError = r.error;
        m_view->clear();
    } else {
        m_loadError.clear();
        m_view->setImage(r.image);
        if (r.animated)
            startAnimation();
    }
    m_shownPath = m_path;
    m_shownPage = m_page;
    m_animated = r.animated;
    m_deepColor = r.deepColor;
    updateStatus();
}

bool MainWindow::hasCurrentImage() const
{
    // Beim Blättern bleibt das alte Bild stehen, bis das neue geladen ist.
    // In dieser Zeit darf nichts das alte Bild unter dem neuen Namen speichern
    // oder bearbeiten.
    if (!m_view->hasImage())
        return false;
    return m_kind != Kind::Image || m_path.isEmpty() || (m_shownPath == m_path && m_shownPage == m_page);
}

void MainWindow::pageStep(int delta)
{
    if (m_kind != Kind::Image || m_path.isEmpty() || m_pageCount <= 1)
        return;
    const int target = m_page + delta;
    if (target < 0 || target >= m_pageCount) {
        message(delta > 0 ? tr("Letzte Seite.") : tr("Erste Seite."));
        return;
    }
    if (!confirmDiscard())
        return;
    m_page = target;
    m_modified = false;
    resetEdits();
    showPage();
    // Nachbarseite vorladen; ältere Seiten aus dem Speicher werfen
    prefetch(target + delta >= 0 && target + delta < m_pageCount ? target + delta : -1);
    updateTitle();
    updateStatus();
}

void MainWindow::step(int delta)
{
    if (m_files.isEmpty())
        return;
    m_lastStep = delta >= 0 ? 1 : -1;
    const int target = m_index + delta;
    if (target < 0 || target >= m_files.size()) {
        message(delta > 0 ? tr("Letzte Datei im Ordner.") : tr("Erste Datei im Ordner."));
        return;
    }
    showIndex(target);
    if (m_slideshow && m_slideTimer->isActive())
        m_slideTimer->start();   // nach Blättern von Hand die volle Zeit zeigen
}

void MainWindow::first()
{
    m_lastStep = 1;
    showIndex(0);
}

void MainWindow::last()
{
    m_lastStep = -1;
    showIndex(int(m_files.size()) - 1);
}

void MainWindow::prefetch(int nextPage)
{
    if (m_index < 0 || m_index >= m_files.size())
        return;

    // Nur aktuelle Seite, Nachbarseite und Nachbardateien behalten – sonst
    // wüchse der Speicher beim Blättern durch lange PDFs ohne Grenze.
    QList<QPair<QString, int>> keep;
    if (!Media::isVideo(m_files.at(m_index))) {
        keep.append({m_files.at(m_index), m_page});
        if (nextPage >= 0)
            keep.append({m_files.at(m_index), nextPage});
    }
    for (int i : {m_index + m_lastStep, m_index - m_lastStep}) {
        if (i >= 0 && i < m_files.size() && !Media::isVideo(m_files.at(i)))
            keep.append({m_files.at(i), 0});
    }
    m_loader->retainOnly(keep);
    for (const auto &k : keep)
        m_loader->request(k.first, k.second);
}

void MainWindow::onLoaded(const QString &path, int page, const LoadResult &result)
{
    if (path != m_path || page != m_page || m_modified || m_kind != Kind::Image)
        return;
    applyLoadResult(result);
}

// --- Video und Animation ---------------------------------------------------

void MainWindow::showVideo()
{
#ifdef KIEKAN_HAVE_MPV
    if (!m_video) {
        m_video = new VideoView(this);
        m_stack->addWidget(m_video);
        connect(m_video, &VideoView::stateChanged, this, &MainWindow::updateStatus);
        connect(m_video, &VideoView::navigateRequested, this, &MainWindow::step);
        connect(m_video, &VideoView::doubleClicked, this, &MainWindow::toggleFullScreen);
        connect(m_video, &VideoView::contextMenuRequested, this, &MainWindow::showContextMenu);
        connect(m_video, &VideoView::errorOccurred, this, [this](const QString &msg) {
            m_loadError = msg;
            updateStatus();
        });
        m_video->setFullscreenMode(isFullScreen());
    }
    m_kind = Kind::Video;
    m_view->clear();
    m_stack->setCurrentWidget(m_video);
    if (!m_video->isValid())
        m_loadError = tr("Videowiedergabe nicht verfügbar (libmpv)");
    else
        m_video->open(m_path);
#endif
}

void MainWindow::stopMedia()
{
    stopAnimation();
#ifdef KIEKAN_HAVE_MPV
    if (m_video && m_kind == Kind::Video)
        m_video->stop();
#endif
    m_kind = Kind::Image;
}

void MainWindow::startAnimation()
{
    stopAnimation();
    m_movie = new QMovie(m_path, QByteArray(), this);
    m_movie->setCacheMode(QMovie::CacheNone);
    connect(m_movie, &QMovie::frameChanged, this, [this] {
        QImage frame = m_movie->currentImage();
        if (frame.isNull())
            return;
        frame = frame.convertToFormat(frame.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                              : QImage::Format_RGB32);
        m_view->setFrame(frame);
    });
    m_movie->start();
}

void MainWindow::stopAnimation()
{
    if (!m_movie)
        return;
    m_movie->stop();
    m_movie->deleteLater();
    m_movie = nullptr;
}

void MainWindow::recognizeText()
{
    QImage image = m_view->image();
#ifdef KIEKAN_HAVE_MPV
    if (m_kind == Kind::Video && m_video)
        image = m_video->currentFrame();
#endif
    if (image.isNull() || (m_kind == Kind::Image && !hasCurrentImage()))
        return;

    QString pdfText;
    const bool pdf = !m_path.isEmpty() && Media::isPdf(m_path) && !m_modified;
    if (m_view->hasSelection() && m_kind == Kind::Image) {
        image = image.copy(m_view->selection());
    } else if (pdf) {
        pdfText = Media::pdfPageText(m_path, m_page);
        // Für die Erkennung feiner rendern als für die Anzeige.
        const QImage fine = ImageLoader::decode(m_path, m_page, 300).image;
        if (!fine.isNull())
            image = fine;
    }
    const QFileInfo fi(m_path);
    const QString name = m_path.isEmpty() ? QStringLiteral("Text.txt")
                                          : fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".txt"));
    OcrDialog dlg(image, pdfText, name, this);
    dlg.exec();
}

void MainWindow::developRaw()
{
    if (!m_rawPreview || m_developing || m_path.isEmpty())
        return;
    if (m_modified) {
        message(tr("Das Bild wurde bereits bearbeitet – zum Entwickeln erst speichern oder verwerfen."));
        return;
    }
    m_developing = true;
    updateStatus();
    const QString path = m_path;
    auto result = std::make_shared<LoadResult>();
    QThread *thread = QThread::create([path, result] { *result = ImageLoader::decode(path, 0, 200, true); });
    connect(thread, &QThread::finished, this, [this, thread, path, result] {
        thread->deleteLater();
        m_developing = false;
        if (path != m_path || m_modified || result->image.isNull()) {
            if (path == m_path && result->image.isNull())
                message(tr("RAW konnte nicht entwickelt werden: %1").arg(result->error));
            updateStatus();
            return;
        }
        // Zoomstufe sinngemäß beibehalten (das Bild wird größer).
        const bool fit = m_view->isFitMode();
        const double scale = double(m_view->image().width()) / result->image.width();
        const double zoom = m_view->zoom() * scale;
        m_rawPreview = false;
        m_view->setImage(result->image);
        if (!fit)
            m_view->setZoom(zoom);
        updateStatus();
        message(tr("RAW voll entwickelt."));
    });
    thread->start();
}

void MainWindow::splitPdf()
{
    if (m_path.isEmpty() || !Media::isPdf(m_path)) {
        message(tr("Zum Teilen zuerst eine PDF öffnen."));
        return;
    }
    PdfSplitDialog dlg(m_path, this);
    dlg.exec();
}

void MainWindow::showExif()
{
    if (m_path.isEmpty() || Media::isVideo(m_path)) {
        message(tr("Keine EXIF-Daten verfügbar."));
        return;
    }
    ExifDialog dlg(m_path, this);
    dlg.exec();
}

// --- Datei -----------------------------------------------------------------

void MainWindow::openDialog()
{
    QSettings settings;
    QString dir = m_path.isEmpty() ? settings.value(QStringLiteral("lastDir"), QDir::homePath()).toString()
                                   : QFileInfo(m_path).absolutePath();
    const QString filter = tr("Bilder und Videos (%1);;Alle Dateien (*)").arg(Media::allNameFilters().join(QLatin1Char(' ')));
    const QString file = QFileDialog::getOpenFileName(this, tr("Bild öffnen"), dir, filter);
    if (file.isEmpty())
        return;
    settings.setValue(QStringLiteral("lastDir"), QFileInfo(file).absolutePath());
    openPath(file);
}

bool MainWindow::confirmDiscard()
{
    if (!m_modified)
        return true;
    const auto answer = QMessageBox::question(
        this, tr("Änderungen speichern?"),
        m_path.isEmpty() ? tr("Das eingefügte Bild wurde noch nicht gespeichert. Jetzt speichern?")
                         : tr("„%1“ wurde verändert. Änderungen speichern?").arg(QFileInfo(m_path).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return save();
    if (answer == QMessageBox::Discard) {
        m_modified = false;
        return true;
    }
    return false;
}

bool MainWindow::save()
{
    if (m_kind == Kind::Video)
        return saveAs();   // Einzelbild aus dem Video
    if (!hasCurrentImage())
        return false;
    if (m_path.isEmpty() || m_pageCount > 1)
        return saveAs();
    if (!m_modified) {
        // Unverändert: nicht neu kodieren (bei JPEG kostete das Qualität).
        message(tr("Keine Änderungen zu speichern."));
        return true;
    }
    if (m_animated) {
        // Nur das aktuelle Einzelbild wäre gespeichert, die Animation weg.
        message(tr("Animationen werden nicht überschrieben – bitte unter neuem Namen speichern."));
        return saveAs();
    }

    const QByteArray format = formatForSuffix(QFileInfo(m_path).suffix());
    if (!QImageWriter::supportedImageFormats().contains(format))
        return saveAs();   // z. B. GIF: Qt kann es lesen, aber nicht schreiben

    const Lossless lossless = saveLossless(m_path);
    if (lossless == Lossless::Cancelled)
        return false;
    if (lossless == Lossless::Saved) {
        m_loader->remove(m_path);
        m_modified = false;
        m_editOps.clear();
        updateTitle();
        updateStatus();
        return true;
    }
    if (m_deepColor) {
        const auto answer = QMessageBox::question(
            this, tr("Farbtiefe geht verloren"),
            tr("Das Original hat mehr als 8 Bit je Farbkanal. KiekAn! speichert nur 8 Bit, "
               "feine Abstufungen gehen beim Überschreiben verloren.\n\nTrotzdem überschreiben?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
            return false;
    }
    const int quality = QSettings().value(QStringLiteral("jpegQuality"), 90).toInt();
    if (!writeImage(m_path, format, quality, m_view->image()))
        return false;
    m_loader->remove(m_path);
    m_editOps.clear();
    m_modified = false;
    updateTitle();
    updateStatus();
    message(tr("Gespeichert: %1").arg(QFileInfo(m_path).fileName()));
    return true;
}

bool MainWindow::saveAs()
{
    // Bei Videos das aktuelle Einzelbild speichern.
    QImage image = m_view->image();
    const bool videoFrame = m_kind == Kind::Video;
#ifdef KIEKAN_HAVE_MPV
    if (videoFrame)
        image = m_video->currentFrame();
#endif
    if (image.isNull() || (!videoFrame && !hasCurrentImage()))
        return false;

    QSettings settings;
    const auto writable = QImageWriter::supportedImageFormats();

    struct Choice { QString filter; QString suffix; QByteArray format; };
    QVector<Choice> choices;
    choices.append({tr("PNG (*.png)"), QStringLiteral("png"), "png"});
    choices.append({tr("JPEG (*.jpg *.jpeg)"), QStringLiteral("jpg"), "jpeg"});
    if (writable.contains("webp"))
        choices.append({tr("WebP (*.webp)"), QStringLiteral("webp"), "webp"});
    choices.append({tr("BMP (*.bmp)"), QStringLiteral("bmp"), "bmp"});
    if (writable.contains("tiff"))
        choices.append({tr("TIFF (*.tif *.tiff)"), QStringLiteral("tif"), "tiff"});

    QString suggested;
    QString selectedFilter = choices.first().filter;
    if (m_path.isEmpty()) {
        const QString dir = settings.value(QStringLiteral("lastDir"), QDir::homePath()).toString();
        const QString name = QStringLiteral("Zwischenablage_%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
        suggested = QDir(dir).filePath(name);
    } else if (videoFrame || m_pageCount > 1 || Media::isPdf(m_path)) {
        // Einzelbild aus Video oder Seite aus PDF/TIFF: eigener Dateiname
        const QFileInfo src(m_path);
        QString name;
#ifdef KIEKAN_HAVE_MPV
        if (videoFrame)
            name = tr("%1_Bild_%2s.png").arg(src.completeBaseName()).arg(int(m_video->position()));
#endif
        if (name.isEmpty())
            name = tr("%1_Seite_%2.png").arg(src.completeBaseName()).arg(m_page + 1);
        suggested = src.dir().filePath(name);
    } else {
        suggested = m_path;
        const QByteArray fmt = formatForSuffix(QFileInfo(m_path).suffix());
        for (const Choice &c : choices)
            if (c.format == fmt)
                selectedFilter = c.filter;
    }

    QStringList filters;
    for (const Choice &c : choices)
        filters << c.filter;

    QString file = QFileDialog::getSaveFileName(this, tr("Speichern unter"), suggested,
                                                filters.join(QStringLiteral(";;")), &selectedFilter);
    if (file.isEmpty())
        return false;

    QFileInfo fi(file);
    QByteArray format = formatForSuffix(fi.suffix());
    if (fi.suffix().isEmpty() || !writable.contains(format)) {
        // Keine oder unbekannte Endung: die des gewählten Filters anhängen.
        for (const Choice &c : choices) {
            if (c.filter == selectedFilter) {
                file += QLatin1Char('.') + c.suffix;
                format = c.format;
                break;
            }
        }
    }

    const Lossless losslessResult = !videoFrame && format == "jpeg" ? saveLossless(file) : Lossless::NotApplicable;
    if (losslessResult == Lossless::Cancelled)
        return false;
    const bool lossless = losslessResult == Lossless::Saved;
    int quality = settings.value(QStringLiteral("jpegQuality"), 90).toInt();
    if (!lossless && (format == "jpeg" || format == "webp")) {
        bool ok = false;
        quality = QInputDialog::getInt(this, tr("Qualität"), tr("Qualität (1–100):"),
                                       quality, 1, 100, 1, &ok);
        if (!ok)
            return false;
        settings.setValue(QStringLiteral("jpegQuality"), quality);
    }

    if (!lossless && !writeImage(file, format, quality, image))
        return false;

    settings.setValue(QStringLiteral("lastDir"), QFileInfo(file).absolutePath());
    const QString saved = QFileInfo(file).absoluteFilePath();
    if (videoFrame) {
        // Das Video läuft weiter; die neue Datei taucht beim nächsten Einlesen auf.
        message(tr("Gespeichert: %1").arg(QFileInfo(saved).fileName()));
        return true;
    }
    m_page = 0;
    m_pageCount = 1;
    m_loader->remove(saved);
    m_path = saved;
    m_modified = false;
    m_editOps.clear();

    // Ordner neu einlesen, damit die neue Datei beim Blättern auftaucht.
    if (m_files.isEmpty() || m_index < 0 || m_index >= m_files.size() || m_files.at(m_index) != saved) {
        m_files = QStringList{saved};
        m_index = 0;
    }
    scanDirectory(QFileInfo(saved).absolutePath(), saved);

    updateTitle();
    updateStatus();
    message(tr("Gespeichert: %1").arg(QFileInfo(saved).fileName()));
    return true;
}

// JPEG nur gedreht/gespiegelt: ohne Neukodierung speichern. NotApplicable:
// kommt nicht in Frage oder der Benutzer will doch neu kodieren.
MainWindow::Lossless MainWindow::saveLossless(const QString &target)
{
    if (!Settings::losslessJpeg() || m_kind != Kind::Image || m_path.isEmpty() || m_pageCount > 1
        || !m_editOpsValid || !LosslessJpeg::isJpeg(m_path) || !LosslessJpeg::isJpeg(target))
        return Lossless::NotApplicable;
    LosslessJpeg::Op op = LosslessJpeg::Op::None;
    for (int e : m_editOps) {
        if (e == 0)
            return Lossless::NotApplicable;   // andere Bearbeitung dabei
        op = LosslessJpeg::compose(op, LosslessJpeg::Op(e));
    }
    if (op == LosslessJpeg::Op::None && target == m_path && m_editOps.isEmpty())
        return Lossless::NotApplicable;
    QString error;
    switch (LosslessJpeg::apply(m_path, target, op, Settings::autoRotate(), &error)) {
    case LosslessJpeg::Result::Pixels:
        message(tr("Verlustfrei gespeichert: %1").arg(QFileInfo(target).fileName()));
        return Lossless::Saved;
    case LosslessJpeg::Result::Tag:
        message(tr("Verlustfrei gespeichert (nur EXIF-Ausrichtung geändert): %1").arg(QFileInfo(target).fileName()));
        return Lossless::Saved;
    case LosslessJpeg::Result::Failed:
        break;
    }
    const auto answer = QMessageBox::question(
        this, tr("Verlustfrei nicht möglich"),
        tr("Das JPEG lässt sich nicht verlustfrei drehen:\n%1\n\nStattdessen neu kodieren (mit leichtem Qualitätsverlust)?").arg(error),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Yes ? Lossless::NotApplicable : Lossless::Cancelled;
}

bool MainWindow::writeImage(const QString &path, const QByteArray &format, int quality, const QImage &image)
{
    QImage img = image;
    if (img.hasAlphaChannel() && !formatHasAlpha(format)) {
        // Formate ohne Transparenz: auf weißen Grund legen statt schwarz.
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, img);
        p.end();
        img = flat;
    }

    // Metadaten der Quelldatei festhalten, bevor sie evtl. überschrieben wird.
    Metadata::Snapshot meta;
    const bool fromFile = !m_path.isEmpty() && m_kind == Kind::Image && !Media::isPdf(m_path)
                          && (format == "jpeg" || format == "png" || format == "webp" || format == "tiff");
    if (fromFile)
        meta = Metadata::Snapshot::capture(m_path);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Speichern fehlgeschlagen"),
                             tr("Datei kann nicht geschrieben werden:\n%1\n\n%2").arg(path, file.errorString()));
        return false;
    }
    QImageWriter writer(&file, format);
    if (format == "jpeg" || format == "webp")
        writer.setQuality(quality);
    if (format == "png")
        writer.setCompression(1);   // schnell, trotzdem verlustfrei
    if (!writer.write(img) || !file.commit()) {
        QMessageBox::warning(this, tr("Speichern fehlgeschlagen"),
                             tr("Fehler beim Speichern von\n%1\n\n%2").arg(path, writer.errorString()));
        return false;
    }
    QString metaError;
    if (!meta.applyTo(path, img.size(), Settings::autoRotate(), &metaError))
        message(tr("Gespeichert, aber EXIF-Daten konnten nicht übernommen werden: %1").arg(metaError));
    return true;
}

void MainWindow::deleteFile()
{
    if (m_path.isEmpty()) {
        message(tr("Das Bild ist nicht gespeichert – nichts zu löschen."));
        return;
    }
    const QString name = QFileInfo(m_path).fileName();
    if (QMessageBox::question(this, tr("Löschen"), tr("„%1“ in den Papierkorb verschieben?").arg(name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
        return;

    if (!moveToTrash(m_path)) {
        // Auf manchen Netzlaufwerken gibt es keinen Papierkorb.
        const auto answer = QMessageBox::warning(
            this, tr("Kein Papierkorb"),
            tr("„%1“ kann nicht in den Papierkorb verschoben werden.\nEndgültig löschen?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes || !QFile::remove(m_path)) {
            if (answer == QMessageBox::Yes)
                QMessageBox::warning(this, tr("Löschen fehlgeschlagen"), tr("„%1“ konnte nicht gelöscht werden.").arg(name));
            return;
        }
    }

    m_loader->remove(m_path);
    m_modified = false;
    const int removed = m_index;
    m_files.removeAt(removed);
    m_path.clear();

    if (m_files.isEmpty()) {
        m_index = -1;
        m_view->clear();
        updateTitle();
        updateStatus();
    } else {
        showIndex(std::min(removed, int(m_files.size()) - 1));
    }
    message(tr("Gelöscht: %1").arg(name));
}

void MainWindow::batchDialog()
{
    if (!confirmDiscard())
        return;
    BatchDialog dlg(m_files, m_path, this);
    connect(&dlg, &BatchDialog::filesRenamed, this, [this](const QList<QPair<QString, QString>> &renamed) {
        // Angezeigte Datei und Ordnerliste den neuen Namen nachführen
        for (const auto &[from, to] : renamed) {
            m_loader->remove(from);
            const qsizetype i = m_files.indexOf(from);
            if (i >= 0)
                m_files[i] = to;
            if (m_path == from)
                m_path = to;
        }
        if (!m_path.isEmpty())
            scanDirectory(QFileInfo(m_path).absolutePath(), m_path);
        updateTitle();
        updateStatus();
    });
    dlg.exec();
}

void MainWindow::renameFile()
{
    if (m_path.isEmpty())
        return;
    const QFileInfo fi(m_path);
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Umbenennen"), tr("Neuer Dateiname:"),
                                               QLineEdit::Normal, fi.fileName(), &ok).trimmed();
    if (!ok || name.isEmpty() || name == fi.fileName())
        return;
    if (name.contains(QLatin1Char('/'))) {
        QMessageBox::warning(this, tr("Umbenennen"), tr("Der Name darf keinen Schrägstrich enthalten."));
        return;
    }
    const QString target = fi.dir().filePath(name);
    if (QFileInfo::exists(target)) {
        QMessageBox::warning(this, tr("Umbenennen"), tr("„%1“ gibt es bereits.").arg(name));
        return;
    }
    if (!QFile::rename(m_path, target)) {
        QMessageBox::warning(this, tr("Umbenennen"), tr("Umbenennen fehlgeschlagen."));
        return;
    }
    m_loader->remove(m_path);
    m_path = target;
    if (m_index >= 0 && m_index < m_files.size())
        m_files[m_index] = target;
    updateTitle();
    updateStatus();
}

void MainWindow::showInfo()
{
    if (!hasCurrentImage())
        return;
    const QImage &img = m_view->image();
    QString text;
    auto row = [&text](const QString &k, const QString &v) {
        text += QStringLiteral("<tr><td style='padding-right:12px'><b>%1</b></td><td>%2</td></tr>")
                    .arg(k.toHtmlEscaped(), v.toHtmlEscaped());
    };
    if (m_path.isEmpty()) {
        row(tr("Quelle"), tr("Zwischenablage"));
    } else {
        const QFileInfo fi(m_path);
        QImageReader reader(m_path);
        row(tr("Datei"), fi.fileName());
        row(tr("Ordner"), fi.absolutePath());
        row(tr("Format"), QString::fromLatin1(reader.format()).toUpper());
        row(tr("Dateigröße"), humanSize(fi.size()));
        row(tr("Geändert"), QLocale().toString(fi.lastModified(), QLocale::LongFormat));
    }
    row(tr("Bildgröße"), tr("%1 × %2 Pixel").arg(img.width()).arg(img.height()));
    row(tr("Megapixel"), QLocale().toString(img.width() * double(img.height()) / 1e6, 'f', 2));
    row(tr("Transparenz"), img.hasAlphaChannel() ? tr("ja") : tr("nein"));
    row(tr("Im Speicher"), humanSize(img.sizeInBytes()));
    if (m_pageCount > 1)
        row(tr("Seite"), tr("%1 von %2").arg(m_page + 1).arg(m_pageCount));
    if (m_files.size() > 0 && m_index >= 0)
        row(tr("Position"), tr("%1 von %2").arg(m_index + 1).arg(m_files.size()));

    // Die wichtigsten Aufnahmedaten gleich mit anzeigen.
    Metadata::Summary exif;
    if (!m_path.isEmpty() && !Media::isPdf(m_path))
        exif = Metadata::summary(m_path);
    if (!exif.isEmpty()) {
        text += QStringLiteral("<tr><td colspan='2'>&nbsp;</td></tr>");
        auto opt = [&row](const QString &k, const QString &v) { if (!v.isEmpty()) row(k, v); };
        opt(tr("Kamera"), exif.camera);
        opt(tr("Aufgenommen"), exif.dateTaken);
        QStringList shot;
        for (const QString &v : {exif.exposure, exif.aperture, exif.iso.isEmpty() ? QString() : QStringLiteral("ISO ") + exif.iso, exif.focalLength})
            if (!v.isEmpty())
                shot << v;
        opt(tr("Belichtung"), shot.join(QStringLiteral(" · ")));
        if (exif.hasGps)
            row(tr("Ort"), QStringLiteral("%1, %2").arg(exif.latitude, 0, 'f', 5).arg(exif.longitude, 0, 'f', 5));
        opt(tr("Titel"), exif.title);
        opt(tr("Beschreibung"), exif.description);
        opt(tr("Stichwörter"), exif.keywords.join(QStringLiteral(", ")));
        if (exif.rating >= 0)
            row(tr("Bewertung"), QString(exif.rating, QChar(0x2605)) + QString(5 - exif.rating, QChar(0x2606)));
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("Bildinformationen"));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral("<table>%1</table>").arg(text));
    QPushButton *exifButton = nullptr;
    if (!m_path.isEmpty() && Metadata::available())
        exifButton = box.addButton(tr("Alle EXIF-Daten …"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (exifButton && box.clickedButton() == exifButton)
        showExif();
}

// --- Bearbeiten ------------------------------------------------------------

void MainWindow::resetEdits()
{
    m_undo.clear();
    m_editOps.clear();
    m_editOpsValid = true;
}

void MainWindow::applyEdit(const QImage &result, int geoOp)
{
    if (result.isNull())
        return;
    m_editOps.append(geoOp);
    stopAnimation();   // Bearbeitung friert das aktuelle Einzelbild ein
    m_undo.append(m_view->image());
    qsizetype total = 0;
    for (const QImage &i : m_undo)
        total += i.sizeInBytes();
    while (m_undo.size() > 1 && total > kUndoBudget) {
        total -= m_undo.first().sizeInBytes();
        m_undo.removeFirst();
    }
    m_view->replaceImage(result);
    m_modified = true;
    updateTitle();
    updateStatus();
}

void MainWindow::undo()
{
    if (m_undo.isEmpty()) {
        message(tr("Nichts rückgängig zu machen."));
        return;
    }
    m_view->replaceImage(m_undo.takeLast());
    // Hinter den letzten Speicherstand zurück: Datei und Anzeige passen nicht mehr
    // über reine Drehungen zusammen.
    if (m_editOps.isEmpty())
        m_editOpsValid = false;
    else
        m_editOps.removeLast();
    // Erst wenn alle Schritte zurückgenommen sind, entspricht das Bild der Datei.
    m_modified = !m_undo.isEmpty() || m_path.isEmpty();
    updateTitle();
    updateStatus();
}

void MainWindow::copy()
{
#ifdef KIEKAN_HAVE_MPV
    if (m_kind == Kind::Video) {
        const QImage frame = m_video->currentFrame();
        if (frame.isNull()) {
            message(tr("Kein Videobild verfügbar."));
            return;
        }
        QGuiApplication::clipboard()->setImage(frame);
        message(tr("Videobild kopiert."));
        return;
    }
#endif
    if (!hasCurrentImage())
        return;
    const QImage img = m_view->hasSelection() ? m_view->image().copy(m_view->selection()) : m_view->image();
    QGuiApplication::clipboard()->setImage(img);
    message(m_view->hasSelection() ? tr("Auswahl kopiert.") : tr("Bild kopiert."));
}

void MainWindow::paste()
{
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime) {
        message(tr("Die Zwischenablage ist leer."));
        return;
    }

    QImage img;
    if (mime->hasImage())
        img = qvariant_cast<QImage>(mime->imageData());

    if (img.isNull() && mime->hasUrls()) {
        // Im Dateimanager kopierte Bilddatei: einfach öffnen.
        for (const QUrl &url : mime->urls()) {
            if (url.isLocalFile()) {
                openPath(url.toLocalFile());
                return;
            }
        }
    }
    if (img.isNull()) {
        message(tr("Die Zwischenablage enthält kein Bild."));
        return;
    }
    if (!confirmDiscard())
        return;

    img = img.convertToFormat(img.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                    : QImage::Format_RGB32);
    stopAnimation();   // sonst überschreibt das nächste Einzelbild das eingefügte
    m_animated = m_deepColor = false;
    m_path.clear();
    resetEdits();
    m_loadError.clear();
    m_modified = true;
    m_view->setImage(img);
    updateTitle();
    updateStatus();
}

void MainWindow::crop()
{
    if (!hasCurrentImage())
        return;
    if (!m_view->hasSelection()) {
        message(tr("Zuerst mit der linken Maustaste einen Bereich markieren."));
        return;
    }
    applyEdit(m_view->image().copy(m_view->selection()));
}

void MainWindow::resizeImage()
{
    if (!hasCurrentImage())
        return;
    ResizeDialog dlg(m_view->image().size(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QSize size = dlg.targetSize();
    if (size == m_view->image().size())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QImage result = m_view->image().scaled(size, Qt::IgnoreAspectRatio,
                                                 dlg.smooth() ? Qt::SmoothTransformation : Qt::FastTransformation);
    QApplication::restoreOverrideCursor();
    applyEdit(result);
}

void MainWindow::rotate(int degrees)
{
    if (!hasCurrentImage())
        return;
    const int d = ((degrees % 360) + 360) % 360;
    const LosslessJpeg::Op op = d == 90 ? LosslessJpeg::Op::Rot90 : d == 180 ? LosslessJpeg::Op::Rot180 : LosslessJpeg::Op::Rot270;
    applyEdit(m_view->image().transformed(QTransform().rotate(degrees)), int(op));
}

void MainWindow::flip(bool horizontal)
{
    if (!hasCurrentImage())
        return;
    applyEdit(m_view->image().mirrored(horizontal, !horizontal),
              int(horizontal ? LosslessJpeg::Op::FlipH : LosslessJpeg::Op::FlipV));
}

// --- Bildkorrekturen und Effekte -------------------------------------------

void MainWindow::applyToImage(const std::function<QImage(const QImage &)> &op)
{
    if (!hasCurrentImage())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QImage &img = m_view->image();
    QImage result;
    if (m_view->hasSelection()) {
        // Nur den markierten Bereich bearbeiten (wie in IrfanView).
        const QRect r = m_view->selection();
        const QImage part = ImageOps::forDisplay(op(ImageOps::forProcessing(img.copy(r))));
        result = img.copy();
        QPainter p(&result);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(r.topLeft(), part);
    } else {
        result = ImageOps::forDisplay(op(ImageOps::forProcessing(img)));
    }
    QApplication::restoreOverrideCursor();
    applyEdit(result);
}

void MainWindow::runFilter(const QString &id, const QVariantMap &params)
{
    const Kiekan::Filter *filter = FilterRegistry::instance().find(id);
    if (!filter)
        return;
    applyToImage([filter, params](const QImage &img) { return FilterRegistry::run(filter, img, params); });
}

void MainWindow::openFilterDialog(const QList<const Kiekan::Filter *> &filters, const Kiekan::Filter *initial)
{
    if (!hasCurrentImage() || filters.isEmpty() || !filters.first())
        return;
    // Die Vorschau zeigt, worauf der Filter wirkt: Auswahl oder ganzes Bild.
    const QImage source = m_view->hasSelection() ? m_view->image().copy(m_view->selection()) : m_view->image();
    FilterDialog dlg(filters, initial, source, this);
    if (dlg.exec() != QDialog::Accepted || !dlg.filter())
        return;
    const Kiekan::Filter *filter = dlg.filter();
    const QVariantMap params = dlg.parameters();
    applyToImage([filter, params](const QImage &img) { return FilterRegistry::run(filter, img, params); });
}

void MainWindow::openCurves()
{
    if (!hasCurrentImage())
        return;
    const QImage source = m_view->hasSelection() ? m_view->image().copy(m_view->selection()) : m_view->image();
    CurvesDialog dlg(source, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    ImageOps::Lut r, g, b;
    dlg.finalLuts(r, g, b);
    applyToImage([r, g, b](const QImage &img) { return ImageOps::applyLuts(img, r, g, b); });
}

void MainWindow::equalizeHistogram()
{
    applyToImage([](const QImage &img) {
        const ImageOps::Lut lut = ImageOps::equalizationLut(ImageOps::histogram(img));
        return ImageOps::applyLuts(img, lut, lut, lut);
    });
}

void MainWindow::autoColors()
{
    applyToImage([](const QImage &img) {
        ImageOps::Lut r, g, b;
        ImageOps::autoLevelsLuts(ImageOps::histogram(img), 0.005, r, g, b);
        return ImageOps::applyLuts(img, r, g, b);
    });
}

void MainWindow::toggleHistogram()
{
    if (!m_histogram) {
        m_histogram = new HistogramDialog(this);
        m_histogram->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    if (m_histogram->isVisible()) {
        m_histogram->hide();
        return;
    }
    m_histogram->show();
    refreshHistogram();
}

void MainWindow::refreshHistogram()
{
    if (m_histogram && m_histogram->isVisible())
        m_histogram->setImage(m_view->image());
}

// --- Extras ----------------------------------------------------------------

void MainWindow::showSettings()
{
    SettingsDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const bool rotateChanged = dlg.autoRotateChanged();
    const bool languageChanged = dlg.apply();

    if (rotateChanged) {
        ImageLoader::setAutoRotate(Settings::autoRotate());
        m_loader->clear();
        if (!m_modified && m_index >= 0 && !m_path.isEmpty())
            showIndex(m_index);   // aktuelles Bild neu laden
    }
    if (dlg.aiAccessChanged())
        updateViewerServer();
    if (languageChanged)
        QMessageBox::information(this, tr("Sprache"), tr("Die neue Sprache gilt nach einem Neustart von KiekAn!"));
}

void MainWindow::showPlugins()
{
    const FilterRegistry &reg = FilterRegistry::instance();
    QString text = QStringLiteral("<p><b>%1</b></p><ul>").arg(tr("Plugin-Ordner:").toHtmlEscaped());
    for (const QString &dir : FilterRegistry::pluginDirectories())
        text += QStringLiteral("<li>%1</li>").arg(QDir::toNativeSeparators(dir).toHtmlEscaped());
    text += QStringLiteral("</ul><p><b>%1</b></p>").arg(tr("Geladene Plugins:").toHtmlEscaped());
    if (reg.pluginFiles().isEmpty()) {
        text += QStringLiteral("<p>%1</p>").arg(tr("keine").toHtmlEscaped());
    } else {
        text += QStringLiteral("<ul>");
        for (const QString &f : reg.pluginFiles())
            text += QStringLiteral("<li>%1</li>").arg(QFileInfo(f).fileName().toHtmlEscaped());
        text += QStringLiteral("</ul>");
    }
    if (!reg.pluginErrors().isEmpty()) {
        text += QStringLiteral("<p><b>%1</b></p><ul>").arg(tr("Fehler:").toHtmlEscaped());
        for (const QString &e : reg.pluginErrors())
            text += QStringLiteral("<li>%1</li>").arg(e.toHtmlEscaped());
        text += QStringLiteral("</ul>");
    }
    QMessageBox box(this);
    box.setWindowTitle(tr("Plugins"));
    box.setTextFormat(Qt::RichText);
    box.setText(text);
    box.exec();
}

// --- KI-Schnittstelle -------------------------------------------------------

void MainWindow::askAiViewerAccessOnce()
{
    if (Settings::aiViewerAccessDecided())
        return;
    QMessageBox box(QMessageBox::Question, tr("KI-Zugriff auf das Fenster"),
                    tr("KiekAn! kann mit KI-Assistenten wie Claude zusammenarbeiten."), QMessageBox::Ok, this);
    box.setInformativeText(
        tr("Auf Dateien darf eine KI nur in Ordnern zugreifen, die ausdrücklich freigegeben wurden.\n\n"
           "Davon getrennt ist der Zugriff auf das offene Fenster: Ist er erlaubt, sieht ein verbundener "
           "Assistent das gerade angezeigte Bild – auch wenn es in keinem freigegebenen Ordner liegt. Er kann "
           "außerdem einen Bereich markieren und Bearbeitungen als ungespeicherten Vorschlag zeigen. Jeder "
           "Zugriff wird im KI-Protokoll vermerkt.\n\n"
           "Ohne verbundenen Assistenten hat die Einstellung keine Wirkung. Ändern lässt sie sich jederzeit "
           "unter Extras → Einstellungen → KI-Schnittstelle."));
    auto *off = new QCheckBox(tr("KI-Zugriff auf das offene Fenster standardmäßig aus"), &box);
    off->setChecked(true);
    box.setCheckBox(off);
    box.exec();
    Settings::setAiViewerAccess(!off->isChecked());
    updateViewerServer();
}

void MainWindow::updateViewerServer()
{
    if (Settings::aiViewerAccess()) {
        // false auch, wenn schon ein anderes Fenster lauscht – das ist normal.
        if (!m_viewerServer->start() && !m_viewerServer->errorString().isEmpty())
            message(tr("KI-Verbindung zum Fenster nicht möglich: %1").arg(m_viewerServer->errorString()));
    } else
        m_viewerServer->stop();
}

QJsonObject MainWindow::handleViewerRequest(const QJsonObject &req)
{
    const QString cmd = req.value(QLatin1String("cmd")).toString();
    auto error = [](const QString &text) { return QJsonObject{{QStringLiteral("error"), text}}; };

    if (cmd == QLatin1String("show")) {
        const QString path = req.value(QLatin1String("path")).toString();
        if (m_modified)
            return error(QStringLiteral("the image in KiekAn! has unsaved changes; ask the user to save or discard them first"));
        if (!QFileInfo::exists(path))
            return error(QStringLiteral("not found: ") + path);
        openPath(path);
        const int page = req.value(QLatin1String("page")).toInt();
        if (page > 0 && m_kind == Kind::Image) {
            m_page = page;
            showPage();
            updateTitle();
            updateStatus();
        }
        if (isMinimized())
            showNormal();
        raise();
        activateWindow();
        return {{QStringLiteral("ok"), true}};
    }

    if (cmd == QLatin1String("state")) {
        QJsonObject state{
            {QStringLiteral("file"), m_path.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(m_path)},
            {QStringLiteral("kind"), m_kind == Kind::Video ? QStringLiteral("video")
                                     : m_path.isEmpty() && m_view->hasImage() ? QStringLiteral("clipboard image")
                                                                               : QStringLiteral("image")},
            {QStringLiteral("index_in_folder"), m_index},
            {QStringLiteral("files_in_folder"), int(m_files.size())},
            {QStringLiteral("unsaved_changes"), m_modified},
            {QStringLiteral("fullscreen"), isFullScreen()},
        };
        if (m_pageCount > 1) {
            state.insert(QStringLiteral("page"), m_page);
            state.insert(QStringLiteral("pages"), m_pageCount);
        }
        if (m_kind == Kind::Image && m_view->hasImage() && !hasCurrentImage())
            state.insert(QStringLiteral("loading"), true);
        if (hasCurrentImage() && m_kind == Kind::Image) {
            state.insert(QStringLiteral("width"), m_view->image().width());
            state.insert(QStringLiteral("height"), m_view->image().height());
            state.insert(QStringLiteral("zoom_percent"), qRound(m_view->zoom() * 100));
        }
#ifdef KIEKAN_HAVE_MPV
        if (m_kind == Kind::Video && m_video) {
            state.insert(QStringLiteral("video_position_s"), m_video->position());
            state.insert(QStringLiteral("video_duration_s"), m_video->duration());
        }
#endif
        if (m_view->hasSelection()) {
            const QRect s = m_view->selection();
            state.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("x"), s.x()}, {QStringLiteral("y"), s.y()},
                                                                  {QStringLiteral("width"), s.width()}, {QStringLiteral("height"), s.height()}});
        } else {
            state.insert(QStringLiteral("selection"), QJsonValue::Null);
        }
        return state;
    }

    if (cmd == QLatin1String("image")) {
        QImage img = m_view->image();
#ifdef KIEKAN_HAVE_MPV
        if (m_kind == Kind::Video && m_video)
            img = m_video->currentFrame();
#endif
        if (img.isNull() || (m_kind == Kind::Image && !hasCurrentImage()))
            return error(QStringLiteral("no image is shown (or it is still loading)"));
        QJsonObject answer{{QStringLiteral("original_width"), img.width()},
                           {QStringLiteral("original_height"), img.height()},
                           {QStringLiteral("modified"), m_modified},
                           {QStringLiteral("file"), m_path}};
        if (req.value(QLatin1String("selection_only")).toBool()) {
            if (!m_view->hasSelection() || m_kind != Kind::Image)
                return error(QStringLiteral("the user has not selected an area"));
            const QRect s = m_view->selection();
            img = img.copy(s);
            answer.insert(QStringLiteral("offset_x"), s.x());
            answer.insert(QStringLiteral("offset_y"), s.y());
        }
        const int maxSize = std::clamp(req.value(QLatin1String("max_size")).toInt(1024), 64, 2048);
        if (img.width() > maxSize || img.height() > maxSize)
            img = img.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        img.save(&buffer, "png");
        answer.insert(QStringLiteral("data"), QString::fromLatin1(bytes.toBase64()));
        return answer;
    }

    if (cmd == QLatin1String("apply")) {
        if (m_kind != Kind::Image || !hasCurrentImage())
            return error(QStringLiteral("no image is shown (or it is still loading)"));
        try {
            const QImage result = EditOps::apply(m_view->image(), req.value(QLatin1String("operations")).toArray());
            applyEdit(result);   // ungespeichert, mit Strg+Z rücknehmbar
        } catch (const EditOps::Error &e) {
            return error(e.message);
        }
        const QString note = req.value(QLatin1String("note")).toString();
        // Länger sichtbar als normale Meldungen: der Benutzer soll entscheiden.
        statusBar()->showMessage(tr("Vorschlag der KI%1 – Strg+S speichert, Strg+Z verwirft")
                                     .arg(note.isEmpty() ? QString() : QStringLiteral(": ") + note), 30000);
        raise();
        return {{QStringLiteral("width"), m_view->image().width()}, {QStringLiteral("height"), m_view->image().height()}};
    }

    if (cmd == QLatin1String("select")) {
        if (m_kind != Kind::Image || !hasCurrentImage())
            return error(QStringLiteral("no image is shown (or it is still loading)"));
        if (req.value(QLatin1String("clear")).toBool()) {
            m_view->clearSelection();
            return {{QStringLiteral("message"), QStringLiteral("selection cleared")}};
        }
        const QRect r(req.value(QLatin1String("x")).toInt(), req.value(QLatin1String("y")).toInt(),
                      req.value(QLatin1String("width")).toInt(), req.value(QLatin1String("height")).toInt());
        const QRect clipped = r.intersected(m_view->image().rect());
        if (clipped.isEmpty())
            return error(QStringLiteral("rectangle lies outside the image"));
        m_view->setSelection(clipped);
        raise();
        return {{QStringLiteral("message"), QStringLiteral("selected x=%1 y=%2 %3x%4").arg(clipped.x()).arg(clipped.y())
                                                    .arg(clipped.width()).arg(clipped.height())}};
    }

    if (cmd == QLatin1String("notify")) {
        warnAboutInjection(req);
        return {{QStringLiteral("message"), QStringLiteral("user notified")}};
    }

    return error(QStringLiteral("unknown command: ") + cmd);
}

void MainWindow::showAiLog(int filter)
{
    AiLogDialog dlg(this);
    dlg.setFilter(AiLogDialog::Filter(filter));
    dlg.exec();
}

// Die KI hat in einer Datei Text gefunden, der sich an eine KI richtet
// (Prompt Injection). Ein Hinweisfenster, das sich bei weiteren Funden nur
// aktualisiert, statt sich zu stapeln.
void MainWindow::warnAboutInjection(const QJsonObject &req)
{
    ++m_injectionCount;
    QStringList sources;
    for (const QJsonValue &v : req.value(QLatin1String("sources")).toArray())
        sources << v.toString();
    QStringList found;
    const QJsonArray warnings = req.value(QLatin1String("warnings")).toArray();
    for (int i = 0; i < std::min<qsizetype>(warnings.size(), 4); ++i)
        found << QStringLiteral("• %1").arg(warnings.at(i).toObject().value(QLatin1String("excerpt")).toString().left(160));

    const QString text = tr("Eine KI hat über die Schnittstelle von KiekAn! eine Datei gelesen, die Text enthält, "
                            "der wie eine Anweisung an eine KI aussieht oder versteckte Zeichen enthält "
                            "(mögliche „Prompt Injection“):\n\n%1\n\n%2\n\n"
                            "Die KI wurde gewarnt, diesen Text nicht zu befolgen. Prüfen Sie bitte, was die KI "
                            "danach tut, und brechen Sie im Zweifel ab.")
                             .arg(sources.join(QLatin1Char('\n')), found.join(QLatin1Char('\n')));
    statusBar()->showMessage(tr("Warnung: verdächtiger Text in einer von der KI gelesenen Datei"), 15000);

    if (m_injectionWarning) {
        m_injectionWarning->setText(text);
        m_injectionWarning->setInformativeText(tr("Funde in dieser Sitzung: %1").arg(m_injectionCount));
        m_injectionWarning->raise();
        return;
    }
    auto *box = new QMessageBox(QMessageBox::Warning, tr("KiekAn! – Warnung zur KI"), text, QMessageBox::Close, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    QPushButton *logButton = box->addButton(tr("KI-Protokoll anzeigen"), QMessageBox::ActionRole);
    connect(logButton, &QPushButton::clicked, this, [this] { showAiLog(AiLogDialog::Warnings); });
    m_injectionWarning = box;
    box->show();
    box->raise();
    box->activateWindow();
}

// --- Ansicht ---------------------------------------------------------------

void MainWindow::toggleFullScreen()
{
    if (isFullScreen()) {
        menuBar()->show();
        m_toolBar->show();
        statusBar()->show();
        m_view->setBackground(kViewBackground);
#ifdef KIEKAN_HAVE_MPV
        if (m_video)
            m_video->setFullscreenMode(false);
#endif
        if (m_wasMaximized)
            showMaximized();
        else
            showNormal();
    } else {
        m_wasMaximized = isMaximized();
        menuBar()->hide();
        m_toolBar->hide();
        statusBar()->hide();
        m_view->setBackground(Qt::black);
#ifdef KIEKAN_HAVE_MPV
        if (m_video)
            m_video->setFullscreenMode(true);
#endif
        showFullScreen();
    }
}

void MainWindow::escape()
{
    if (m_slideshow) {
        stopSlideshow();
        return;
    }
    if (isFullScreen())
        toggleFullScreen();
    else if (m_view->hasSelection())
        m_view->clearSelection();
    else if (Settings::escapeQuits())
        close();     // wie in IrfanView
}

// --- Diashow und Drucken ------------------------------------------------------

void MainWindow::startSlideshow()
{
    if (m_files.size() < 2) {
        message(tr("Für eine Diashow braucht der Ordner mindestens zwei Bilder."));
        return;
    }
    QSettings settings;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Diashow"));
    auto *delay = new QDoubleSpinBox(&dlg);
    delay->setRange(0.5, 3600);
    delay->setDecimals(1);
    delay->setSingleStep(0.5);
    delay->setSuffix(tr(" Sekunden"));
    delay->setValue(settings.value(QStringLiteral("slideshow/delay"), 5.0).toDouble());
    auto *loop = new QCheckBox(tr("Am Ende von vorn beginnen"), &dlg);
    loop->setChecked(settings.value(QStringLiteral("slideshow/loop"), true).toBool());
    auto *fromStart = new QCheckBox(tr("Mit dem ersten Bild beginnen"), &dlg);
    fromStart->setChecked(settings.value(QStringLiteral("slideshow/fromStart"), false).toBool());
    auto *fullScreen = new QCheckBox(tr("Im Vollbild"), &dlg);
    fullScreen->setChecked(settings.value(QStringLiteral("slideshow/fullScreen"), true).toBool());
    auto *hint = new QLabel(tr("Während der Diashow: Leertaste hält an und setzt fort, Pfeiltasten blättern, Esc beendet."), &dlg);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: gray"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Starten"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *form = new QFormLayout(&dlg);
    form->addRow(tr("Anzeigedauer je Bild:"), delay);
    form->addRow(loop);
    form->addRow(fromStart);
    form->addRow(fullScreen);
    form->addRow(hint);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;
    settings.setValue(QStringLiteral("slideshow/delay"), delay->value());
    settings.setValue(QStringLiteral("slideshow/loop"), loop->isChecked());
    settings.setValue(QStringLiteral("slideshow/fromStart"), fromStart->isChecked());
    settings.setValue(QStringLiteral("slideshow/fullScreen"), fullScreen->isChecked());

    if (!m_slideTimer) {
        m_slideTimer = new QTimer(this);
        connect(m_slideTimer, &QTimer::timeout, this, &MainWindow::slideshowTick);
    }
    m_slideLoop = loop->isChecked();
    m_slideTimer->setInterval(int(delay->value() * 1000));
    m_slideshow = true;
    if (fromStart->isChecked())
        first();
    m_slideFullScreen = fullScreen->isChecked() && !isFullScreen();
    if (m_slideFullScreen)
        toggleFullScreen();
    m_slideTimer->start();
    message(tr("Diashow: Leertaste hält an, Esc beendet."));
}

void MainWindow::slideshowTick()
{
    if (m_index + 1 < m_files.size()) {
        step(1);
    } else if (m_slideLoop) {
        first();
    } else {
        stopSlideshow();
        message(tr("Diashow beendet – letztes Bild erreicht."));
    }
}

void MainWindow::stopSlideshow()
{
    if (!m_slideshow)
        return;
    m_slideshow = false;
    m_slideTimer->stop();
    if (m_slideFullScreen && isFullScreen())
        toggleFullScreen();
    m_slideFullScreen = false;
    message(tr("Diashow beendet."));
}

void MainWindow::print()
{
#ifdef KIEKAN_HAVE_PRINT
    if (m_kind == Kind::Image && !hasCurrentImage())
        return;
    QImage image = m_view->image();
#ifdef KIEKAN_HAVE_MPV
    if (m_kind == Kind::Video)
        image = m_video->currentFrame();
#endif
    if (image.isNull()) {
        message(tr("Nichts zu drucken."));
        return;
    }
    // Mit Auswahl nur den markierten Bereich drucken (wie IrfanView)
    if (m_kind != Kind::Video && m_view->hasSelection())
        image = image.copy(m_view->selection());

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(m_path.isEmpty() ? tr("KiekAn!") : QFileInfo(m_path).fileName());
    printer.setPageOrientation(image.width() > image.height() ? QPageLayout::Landscape : QPageLayout::Portrait);
    QPrintPreviewDialog preview(&printer, this);
    preview.setWindowTitle(tr("Druckvorschau"));
    connect(&preview, &QPrintPreviewDialog::paintRequested, this, [&image](QPrinter *p) {
        QPainter painter(p);
        const QRect page = p->pageLayout().paintRectPixels(p->resolution());
        // An die Seite anpassen (Seitenverhältnis bleibt), mittig
        QSize size = image.size();
        size.scale(page.size(), Qt::KeepAspectRatio);
        const QRect target(QPoint((page.width() - size.width()) / 2, (page.height() - size.height()) / 2), size);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, image);
    });
    preview.resize(900, 700);
    preview.exec();
#else
    message(tr("Diese Fassung von KiekAn! wurde ohne Druckunterstützung gebaut."));
#endif
}

void MainWindow::showContextMenu(const QPoint &globalPos)
{
    m_contextMenu->popup(globalPos);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
#ifdef KIEKAN_HAVE_MPV
    // Video: ←/→ spult 5 s, mit Umschalt 60 s. Blättern geht mit ↑/↓ und Bild↑/↓.
    if (m_kind == Kind::Video && (event->key() == Qt::Key_Right || event->key() == Qt::Key_Left)) {
        const double amount = (event->modifiers() & Qt::ShiftModifier) ? 60 : 5;
        m_video->seekRelative(event->key() == Qt::Key_Right ? amount : -amount);
        return;
    }
#endif
    // Pfeiltasten: Ist das Bild größer als das Fenster, wird verschoben,
    // sonst geblättert.
    switch (event->key()) {
    case Qt::Key_Right:
        if (m_view->canPan()) m_view->panBy(kPanStep, 0); else step(1);
        return;
    case Qt::Key_Left:
        if (m_view->canPan()) m_view->panBy(-kPanStep, 0); else step(-1);
        return;
    case Qt::Key_Down:
        if (m_view->canPan()) m_view->panBy(0, kPanStep); else step(1);
        return;
    case Qt::Key_Up:
        if (m_view->canPan()) m_view->panBy(0, -kPanStep); else step(-1);
        return;
    default:
        QMainWindow::keyPressEvent(event);
    }
}

void MainWindow::showHelp()
{
    // Aktuelle Belegung aus den Aktionen (auch eigene Tasten), dazu Maus und
    // Sonderfälle, die keine eigene Aktion sind.
    struct Entry { QString keys; QString what; };
    QList<Entry> entries;
    for (const QAction *a : std::as_const(m_actions)) {
        if (a->shortcuts().isEmpty() || a->objectName() == QLatin1String("space")
            || a->objectName() == QLatin1String("escape"))
            continue;
        QStringList keys;
        for (const QKeySequence &k : a->shortcuts())
            keys << k.toString(QKeySequence::NativeText);
        QString what = a->text();
        what.remove(QLatin1Char('&'));
        entries.append({keys.join(QStringLiteral(", ")), what});
    }
    static const char *const extra[][2] = {
        {QT_TR_NOOP("Leertaste"), QT_TR_NOOP("Nächstes Bild; bei Videos Abspielen / Pause; in der Diashow Anhalten")},
        {QT_TR_NOOP("Esc"), QT_TR_NOOP("Diashow / Vollbild beenden, Auswahl aufheben oder Beenden")},
        {QT_TR_NOOP("Mausrad"), QT_TR_NOOP("Blättern")},
        {QT_TR_NOOP("Strg + Mausrad"), QT_TR_NOOP("Zoomen")},
        {QT_TR_NOOP("Pfeiltasten, rechte Maustaste ziehen"), QT_TR_NOOP("Verschieben (wenn gezoomt)")},
        {QT_TR_NOOP("Linke Maustaste ziehen"), QT_TR_NOOP("Bereich auswählen")},
        {QT_TR_NOOP("Doppelklick"), QT_TR_NOOP("Vollbild")},
        {QT_TR_NOOP("→ / ← (Video)"), QT_TR_NOOP("5 s vor / zurück (mit Umschalt: 60 s); ↑ / ↓ blättern")},
    };
    for (const auto &e : extra)
        entries.append({tr(e[0]), tr(e[1])});

    QString rows;
    for (const Entry &e : entries)
        rows += QStringLiteral("<tr><td style='padding-right:16px'><b>%1</b></td><td>%2</td></tr>")
                    .arg(e.keys.toHtmlEscaped(), e.what.toHtmlEscaped());

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Tastenkürzel"));
    auto *text = new QTextBrowser(&dlg);
    text->setHtml(QStringLiteral("<table>%1</table><p>%2</p>")
                      .arg(rows, tr("Ändern unter Extras → Tastenkürzel anpassen.").toHtmlEscaped()));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(text);
    layout->addWidget(buttons);
    dlg.resize(560, 640);
    dlg.exec();
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("Über KiekAn!"),
        tr("<h3>KiekAn! %1</h3>"
           "<p>Schneller, schlanker Bildbetrachter.</p>"
           "<p>Copyright © 2026 Carl Kliefert</p>"
           "<p>Freie Software unter der GNU General Public License, "
           "Version 3 oder später.</p>")
            .arg(QStringLiteral(KIEKAN_VERSION)));
}

// --- Titel und Statuszeile -------------------------------------------------

void MainWindow::updateTitle()
{
    QString name;
    if (m_path.isEmpty())
        name = m_view->hasImage() ? tr("Zwischenablage") : QString();
    else
        name = QFileInfo(m_path).fileName();
    if (!name.isEmpty() && m_pageCount > 1)
        name += tr(" – Seite %1 von %2").arg(m_page + 1).arg(m_pageCount);

    if (name.isEmpty())
        setWindowTitle(QString());   // nur der Anwendungsname
    else
        setWindowTitle(name + (m_modified ? QStringLiteral(" *") : QString()));
}

void MainWindow::updateStatus()
{
    QStringList parts;
    if (!m_files.isEmpty() && m_index >= 0 && !m_path.isEmpty())
        parts << QStringLiteral("%1 / %2").arg(m_index + 1).arg(m_files.size());

    if (!m_loadError.isEmpty()) {
        parts << tr("Kann nicht geöffnet werden: %1").arg(m_loadError);
#ifdef KIEKAN_HAVE_MPV
    } else if (m_kind == Kind::Video && m_video) {
        parts << tr("Video");
        if (m_video->videoSize().isValid())
            parts << QStringLiteral("%1 × %2").arg(m_video->videoSize().width()).arg(m_video->videoSize().height());
        parts << humanSize(QFileInfo(m_path).size());
#endif
    } else if (m_view->hasImage()) {
        const QImage &img = m_view->image();
        if (m_pageCount > 1)
            parts << tr("Seite %1/%2").arg(m_page + 1).arg(m_pageCount);
        if (m_developing)
            parts << tr("RAW wird entwickelt …");
        else if (m_rawPreview)
            parts << tr("RAW-Vorschau (voll: %1 × %2, Strg+D entwickelt)").arg(m_rawFullSize.width()).arg(m_rawFullSize.height());
        if (m_partNumber > 0)
            parts << tr("Teil %1/%2 · Originalseite %3").arg(m_partNumber).arg(m_partOf).arg(m_partFirstPage + m_page);
        parts << QStringLiteral("%1 × %2").arg(img.width()).arg(img.height());
        if (!m_path.isEmpty())
            parts << humanSize(QFileInfo(m_path).size());
        parts << QStringLiteral("%1 %").arg(qRound(m_view->zoom() * 100));
        if (m_view->hasSelection()) {
            const QRect s = m_view->selection();
            parts << tr("Auswahl: %1 × %2 bei %3, %4").arg(s.width()).arg(s.height()).arg(s.x()).arg(s.y());
        }
    } else if (!m_path.isEmpty()) {
        parts << tr("Lädt …");
    }

    if (!m_path.isEmpty())
        parts << QDir::toNativeSeparators(m_path);
    else if (m_view->hasImage())
        parts << tr("Zwischenablage (nicht gespeichert)");

    m_statusLabel->setText(parts.join(QStringLiteral("   ·   ")));
}

void MainWindow::message(const QString &text)
{
    statusBar()->showMessage(text, 3000);
}

// --- Fenster ---------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscard()) {
        event->ignore();
        return;
    }
    if (isFullScreen())
        toggleFullScreen();
    QSettings().setValue(QStringLiteral("window/geometry"), saveGeometry());
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            openPath(url.toLocalFile());
            event->acceptProposedAction();
            return;
        }
    }
}

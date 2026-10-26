// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"

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

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCollator>
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

QCollator naturalCollator()
{
    QCollator c;
    c.setNumericMode(true);
    c.setCaseSensitivity(Qt::CaseInsensitive);
    return c;
}

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
    auto make = [this](const QString &text, const char *icon,
                       const QList<QKeySequence> &keys, auto slot) {
        auto *a = new QAction(Icons::get(QString::fromLatin1(icon)), text, this);
        a->setShortcuts(keys);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };
    using K = QKeySequence;

    QAction *open = make(tr("&Öffnen …"), "document-open", {K(Qt::Key_O), K::Open}, [this] { openDialog(); });
    QAction *saveAct = make(tr("&Speichern"), "document-save", {K::Save}, [this] { save(); });
    QAction *saveAsAct = make(tr("Speichern &unter …"), "document-save-as", {K(Qt::Key_S)}, [this] { saveAs(); });
    QAction *rename = make(tr("&Umbenennen …"), "edit-rename", {K(Qt::Key_F2)}, [this] { renameFile(); });
    QAction *del = make(tr("In den &Papierkorb"), "edit-delete", {K(Qt::Key_Delete)}, [this] { deleteFile(); });
    QAction *info = make(tr("Bild&informationen"), "document-properties", {K(Qt::Key_I)}, [this] { showInfo(); });
    QAction *quit = make(tr("&Beenden"), "application-exit", {K(Qt::CTRL | Qt::Key_Q)}, [this] { close(); });

    QAction *undoAct = make(tr("&Rückgängig"), "edit-undo", {K::Undo}, [this] { undo(); });
    QAction *copyAct = make(tr("&Kopieren"), "edit-copy", {K::Copy}, [this] { copy(); });
    QAction *pasteAct = make(tr("&Einfügen"), "edit-paste", {K::Paste}, [this] { paste(); });
    QAction *selAll = make(tr("&Alles auswählen"), "edit-select-all", {K::SelectAll}, [this] { m_view->selectAll(); });
    QAction *cropAct = make(tr("Auswahl &zuschneiden"), "transform-crop", {K(Qt::CTRL | Qt::Key_Y)}, [this] { crop(); });

    QAction *rotR = make(tr("Nach &rechts drehen"), "object-rotate-right", {K(Qt::Key_R)}, [this] { rotate(90); });
    QAction *rotL = make(tr("Nach &links drehen"), "object-rotate-left", {K(Qt::Key_L)}, [this] { rotate(-90); });
    QAction *flipH = make(tr("&Horizontal spiegeln"), "object-flip-horizontal", {K(Qt::Key_H)}, [this] { flip(true); });
    QAction *flipV = make(tr("&Vertikal spiegeln"), "object-flip-vertical", {K(Qt::Key_V)}, [this] { flip(false); });
    QAction *resizeAct = make(tr("&Größe ändern …"), "transform-scale", {K(Qt::CTRL | Qt::Key_R)}, [this] { resizeImage(); });

    QAction *gray = make(tr("In &Graustufen umwandeln"), "color-mode-black-white", {K(Qt::CTRL | Qt::Key_G)},
                         [this] { applyToImage(ImageOps::grayscale); });
    QAction *bw = make(tr("In &Schwarzweiß umwandeln …"), "", {K(Qt::CTRL | Qt::SHIFT | Qt::Key_B)},
                       [this] { openFilterDialog({FilterRegistry::instance().find(QStringLiteral("schwarzweiss"))}, nullptr); });
    QAction *negative = make(tr("&Negativ"), "edit-select-invert", {K(Qt::CTRL | Qt::SHIFT | Qt::Key_N)},
                             [this] { applyToImage(ImageOps::invert); });
    QAction *autoCol = make(tr("&Automatische Farbkorrektur"), "", {K(Qt::SHIFT | Qt::Key_U)}, [this] { autoColors(); });
    QAction *equalize = make(tr("Histogramm &ausgleichen"), "", {K(Qt::CTRL | Qt::SHIFT | Qt::Key_E)},
                             [this] { equalizeHistogram(); });
    QAction *colors = make(tr("&Farben anpassen …"), "color-management", {K(Qt::SHIFT | Qt::Key_G)},
                           [this] { openFilterDialog({FilterRegistry::instance().find(QStringLiteral("farben"))}, nullptr); });
    QAction *curves = make(tr("Gradations&kurven …"), "", {K(Qt::CTRL | Qt::Key_M)}, [this] { openCurves(); });
    QAction *sharpenQuick = make(tr("Sc&härfen"), "", {K(Qt::SHIFT | Qt::Key_S)},
                                 [this] { runFilter(QStringLiteral("schaerfen")); });
    QAction *sharpen = make(tr("Schärfen (Einstellungen) …"), "", {},
                            [this] { openFilterDialog({FilterRegistry::instance().find(QStringLiteral("schaerfen"))}, nullptr); });
    QAction *blur = make(tr("&Weichzeichnen …"), "", {},
                         [this] { openFilterDialog({FilterRegistry::instance().find(QStringLiteral("weichzeichnen"))}, nullptr); });
    QAction *effects = make(tr("&Effekte-Browser …"), "", {K(Qt::CTRL | Qt::Key_E)},
                            [this] { openFilterDialog(FilterRegistry::instance().effects(), nullptr); });
    QAction *histogram = make(tr("&Histogramm"), "", {K(Qt::SHIFT | Qt::Key_H)}, [this] { toggleHistogram(); });
    QAction *settingsAct = make(tr("&Einstellungen …"), "preferences-system", {K(Qt::Key_P)}, [this] { showSettings(); });
    QAction *plugins = make(tr("&Plugins …"), "", {}, [this] { showPlugins(); });
    QAction *aiLog = make(tr("&KI-Protokoll …"), "", {}, [this] { showAiLog(); });

    QAction *exifAct = make(tr("&EXIF-Daten …"), "", {K(Qt::Key_E)}, [this] { showExif(); });
    QAction *developAct = make(tr("RAW voll &entwickeln"), "", {K(Qt::CTRL | Qt::Key_D)}, [this] { developRaw(); });
    QAction *splitAct = make(tr("PDF &teilen …"), "", {}, [this] { splitPdf(); });
    QAction *ocrAct = make(tr("&Text erkennen (OCR) …"), "", {K(Qt::CTRL | Qt::Key_T)}, [this] { recognizeText(); });

    // Leertaste: bei Videos Pause, sonst nächstes Bild.
    make(tr("Leertaste"), "", {K(Qt::Key_Space)}, [this] {
#ifdef KIEKAN_HAVE_MPV
        if (m_kind == Kind::Video) {
            m_video->togglePause();
            return;
        }
#endif
        step(1);
    });
    QAction *next = make(tr("&Nächstes Bild"), "go-next", {K(Qt::Key_PageDown)}, [this] { step(1); });
    QAction *nextPage = make(tr("Nächste &Seite"), "", {K(Qt::CTRL | Qt::Key_PageDown)}, [this] { pageStep(1); });
    QAction *prevPage = make(tr("Vorherige S&eite"), "", {K(Qt::CTRL | Qt::Key_PageUp)}, [this] { pageStep(-1); });
    QAction *mute = make(tr("Ton &aus/an"), "", {K(Qt::Key_M)}, [this] {
#ifdef KIEKAN_HAVE_MPV
        if (m_kind == Kind::Video)
            m_video->toggleMute();
#endif
    });
    QAction *prev = make(tr("&Vorheriges Bild"), "go-previous", {K(Qt::Key_Backspace), K(Qt::Key_PageUp)}, [this] { step(-1); });
    QAction *firstAct = make(tr("&Erstes Bild"), "go-first", {K(Qt::Key_Home)}, [this] { first(); });
    QAction *lastAct = make(tr("&Letztes Bild"), "go-last", {K(Qt::Key_End)}, [this] { last(); });

    QAction *zoomIn = make(tr("Ver&größern"), "zoom-in",
                           {K(Qt::Key_Plus), K(Qt::Key_Equal), K(Qt::KeypadModifier | Qt::Key_Plus)},
                           [this] { m_view->zoomIn(); });
    QAction *zoomOut = make(tr("Ver&kleinern"), "zoom-out",
                            {K(Qt::Key_Minus), K(Qt::KeypadModifier | Qt::Key_Minus)},
                            [this] { m_view->zoomOut(); });
    QAction *zoom100 = make(tr("&Originalgröße (100 %)"), "zoom-original", {K(Qt::CTRL | Qt::Key_H)}, [this] { m_view->setZoom(1.0); });
    QAction *zoomFit = make(tr("An &Fenster anpassen"), "zoom-fit-best", {K(Qt::Key_F)}, [this] { m_view->setFitMode(); });
    QAction *full = make(tr("&Vollbild"), "view-fullscreen",
                         {K(Qt::Key_Return), K(Qt::Key_Enter), K(Qt::Key_F11)},
                         [this] { toggleFullScreen(); });
    make(tr("Abbrechen / Beenden"), "", {K(Qt::Key_Escape)}, [this] { escape(); });

    QAction *help = make(tr("&Tastenkürzel"), "help-contents", {K(Qt::Key_F1)}, [this] { showHelp(); });
    QAction *about = make(tr("Über &Kiek an"), "help-about", {}, [this] { showAbout(); });

    QMenu *file = menuBar()->addMenu(tr("&Datei"));
    file->addActions({open, saveAct, saveAsAct});
    file->addSeparator();
    file->addActions({rename, del, info, exifAct, ocrAct, splitAct});
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
    view->addActions({full, histogram, mute});

    QMenu *extras = menuBar()->addMenu(tr("E&xtras"));
    extras->addActions({settingsAct, plugins});
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
        m_undo.clear();
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
        const QCollator collator = naturalCollator();
        std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) {
            return collator.compare(a, b) < 0;
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
        const QCollator collator = naturalCollator();
        const QString name = QFileInfo(current).fileName();
        auto pos = std::lower_bound(list.begin(), list.end(), current, [&](const QString &a, const QString &) {
            return collator.compare(QFileInfo(a).fileName(), name) < 0;
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
    m_undo.clear();
    m_loadError.clear();

    // Teil einer mit Kiek an geteilten PDF? Dann Originalseiten anzeigen.
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
    updateStatus();
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
    m_undo.clear();
    showPage();
    // Nachbarseiten vorladen
    if (target + delta >= 0 && target + delta < m_pageCount)
        m_loader->request(m_path, target + delta);
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

void MainWindow::prefetch()
{
    if (m_index < 0 || m_index >= m_files.size())
        return;

    QList<QPair<QString, int>> keep;
    if (!Media::isVideo(m_files.at(m_index)))
        keep.append({m_files.at(m_index), m_page});
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
    if (image.isNull())
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
    if (!m_view->hasImage())
        return false;
    if (m_path.isEmpty() || m_pageCount > 1)
        return saveAs();

    const QByteArray format = formatForSuffix(QFileInfo(m_path).suffix());
    if (!QImageWriter::supportedImageFormats().contains(format))
        return saveAs();   // z. B. GIF: Qt kann es lesen, aber nicht schreiben

    const int quality = QSettings().value(QStringLiteral("jpegQuality"), 90).toInt();
    if (!writeImage(m_path, format, quality, m_view->image()))
        return false;
    m_loader->remove(m_path);
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
    if (image.isNull())
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

    int quality = settings.value(QStringLiteral("jpegQuality"), 90).toInt();
    if (format == "jpeg" || format == "webp") {
        bool ok = false;
        quality = QInputDialog::getInt(this, tr("Qualität"), tr("Qualität (1–100):"),
                                       quality, 1, 100, 1, &ok);
        if (!ok)
            return false;
        settings.setValue(QStringLiteral("jpegQuality"), quality);
    }

    if (!writeImage(file, format, quality, image))
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

    if (!QFile::moveToTrash(m_path)) {
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
    if (!m_view->hasImage())
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

void MainWindow::applyEdit(const QImage &result)
{
    if (result.isNull())
        return;
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
    if (!m_view->hasImage())
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
    m_path.clear();
    m_undo.clear();
    m_loadError.clear();
    m_modified = true;
    m_view->setImage(img);
    updateTitle();
    updateStatus();
}

void MainWindow::crop()
{
    if (!m_view->hasImage())
        return;
    if (!m_view->hasSelection()) {
        message(tr("Zuerst mit der linken Maustaste einen Bereich markieren."));
        return;
    }
    applyEdit(m_view->image().copy(m_view->selection()));
}

void MainWindow::resizeImage()
{
    if (!m_view->hasImage())
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
    if (!m_view->hasImage())
        return;
    applyEdit(m_view->image().transformed(QTransform().rotate(degrees)));
}

void MainWindow::flip(bool horizontal)
{
    if (!m_view->hasImage())
        return;
    applyEdit(m_view->image().mirrored(horizontal, !horizontal));
}

// --- Bildkorrekturen und Effekte -------------------------------------------

void MainWindow::applyToImage(const std::function<QImage(const QImage &)> &op)
{
    if (!m_view->hasImage())
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
    if (!m_view->hasImage() || filters.isEmpty() || !filters.first())
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
    if (!m_view->hasImage())
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
        QMessageBox::information(this, tr("Sprache"), tr("Die neue Sprache gilt nach einem Neustart von Kiek an."));
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

void MainWindow::updateViewerServer()
{
    if (Settings::aiViewerAccess())
        m_viewerServer->start();   // false, wenn schon ein anderes Fenster lauscht
    else
        m_viewerServer->stop();
}

QJsonObject MainWindow::handleViewerRequest(const QJsonObject &req)
{
    const QString cmd = req.value(QLatin1String("cmd")).toString();
    auto error = [](const QString &text) { return QJsonObject{{QStringLiteral("error"), text}}; };

    if (cmd == QLatin1String("show")) {
        const QString path = req.value(QLatin1String("path")).toString();
        if (m_modified)
            return error(QStringLiteral("the image in Kiek an has unsaved changes; ask the user to save or discard them first"));
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
        if (m_view->hasImage() && m_kind == Kind::Image) {
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
        if (img.isNull())
            return error(QStringLiteral("no image is shown"));
        QJsonObject answer{{QStringLiteral("original_width"), img.width()},
                           {QStringLiteral("original_height"), img.height()},
                           {QStringLiteral("modified"), m_modified}};
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
        if (m_kind != Kind::Image || !m_view->hasImage())
            return error(QStringLiteral("no image is shown"));
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
        if (m_kind != Kind::Image || !m_view->hasImage())
            return error(QStringLiteral("no image is shown"));
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

    const QString text = tr("Eine KI hat über die Schnittstelle von Kiek an eine Datei gelesen, die Text enthält, "
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
    auto *box = new QMessageBox(QMessageBox::Warning, tr("Kiek an – Warnung zur KI"), text, QMessageBox::Close, this);
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
    if (isFullScreen())
        toggleFullScreen();
    else if (m_view->hasSelection())
        m_view->clearSelection();
    else if (Settings::escapeQuits())
        close();     // wie in IrfanView
}

void MainWindow::showContextMenu(const QPoint &globalPos)
{
    m_contextMenu->popup(globalPos);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
#ifdef KIEKAN_HAVE_MPV
    // Video: Umschalt+Pfeil spult 5 s, Strg+Umschalt+Pfeil 60 s.
    if (m_kind == Kind::Video && (event->modifiers() & Qt::ShiftModifier)
        && (event->key() == Qt::Key_Right || event->key() == Qt::Key_Left)) {
        const double amount = (event->modifiers() & Qt::ControlModifier) ? 60 : 5;
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
    struct Entry { const char *keys; const char *what; };
    static const Entry entries[] = {
        {QT_TR_NOOP("Leertaste, Bild ab, →"), QT_TR_NOOP("Nächstes Bild")},
        {QT_TR_NOOP("Rücktaste, Bild auf, ←"), QT_TR_NOOP("Vorheriges Bild")},
        {QT_TR_NOOP("Pos1 / Ende"), QT_TR_NOOP("Erstes / letztes Bild")},
        {QT_TR_NOOP("Mausrad"), QT_TR_NOOP("Blättern")},
        {QT_TR_NOOP("Strg + Mausrad, + / −"), QT_TR_NOOP("Zoomen")},
        {QT_TR_NOOP("Strg+H"), QT_TR_NOOP("Originalgröße (100 %)")},
        {QT_TR_NOOP("F"), QT_TR_NOOP("An Fenster anpassen")},
        {QT_TR_NOOP("Pfeiltasten, rechte Maustaste ziehen"), QT_TR_NOOP("Verschieben (wenn gezoomt)")},
        {QT_TR_NOOP("Eingabe, F11, Doppelklick"), QT_TR_NOOP("Vollbild")},
        {QT_TR_NOOP("Esc"), QT_TR_NOOP("Vollbild beenden / Auswahl aufheben / Beenden")},
        {QT_TR_NOOP("Linke Maustaste ziehen"), QT_TR_NOOP("Bereich auswählen")},
        {QT_TR_NOOP("Strg+A"), QT_TR_NOOP("Alles auswählen")},
        {QT_TR_NOOP("Strg+Y"), QT_TR_NOOP("Auf Auswahl zuschneiden")},
        {QT_TR_NOOP("Strg+C / Strg+V"), QT_TR_NOOP("Kopieren / Einfügen")},
        {QT_TR_NOOP("R / L"), QT_TR_NOOP("Rechts / links drehen")},
        {QT_TR_NOOP("H / V"), QT_TR_NOOP("Horizontal / vertikal spiegeln")},
        {QT_TR_NOOP("Strg+R"), QT_TR_NOOP("Größe ändern")},
        {QT_TR_NOOP("Strg+G"), QT_TR_NOOP("Graustufen")},
        {QT_TR_NOOP("Strg+Umschalt+B"), QT_TR_NOOP("Schwarzweiß")},
        {QT_TR_NOOP("Strg+Umschalt+N"), QT_TR_NOOP("Negativ")},
        {QT_TR_NOOP("Umschalt+G"), QT_TR_NOOP("Farben anpassen")},
        {QT_TR_NOOP("Strg+M"), QT_TR_NOOP("Gradationskurven")},
        {QT_TR_NOOP("Umschalt+U"), QT_TR_NOOP("Automatische Farbkorrektur")},
        {QT_TR_NOOP("Strg+Umschalt+E"), QT_TR_NOOP("Histogramm ausgleichen")},
        {QT_TR_NOOP("Umschalt+S"), QT_TR_NOOP("Schärfen")},
        {QT_TR_NOOP("Strg+E"), QT_TR_NOOP("Effekte-Browser")},
        {QT_TR_NOOP("Umschalt+H"), QT_TR_NOOP("Histogramm anzeigen")},
        {QT_TR_NOOP("P"), QT_TR_NOOP("Einstellungen")},
        {QT_TR_NOOP("E"), QT_TR_NOOP("EXIF-Daten")},
        {QT_TR_NOOP("Strg+D"), QT_TR_NOOP("RAW voll entwickeln (sonst eingebettete Vorschau)")},
        {QT_TR_NOOP("Strg+T"), QT_TR_NOOP("Text erkennen (OCR), mit Auswahl nur im markierten Bereich")},
        {QT_TR_NOOP("Strg+Bild ab / Strg+Bild auf"), QT_TR_NOOP("Nächste / vorherige Seite (PDF, TIFF)")},
        {QT_TR_NOOP("Leertaste (Video)"), QT_TR_NOOP("Abspielen / Pause")},
        {QT_TR_NOOP("Umschalt+→ / ← (Video)"), QT_TR_NOOP("5 s vor / zurück (mit Strg: 60 s)")},
        {QT_TR_NOOP("M (Video)"), QT_TR_NOOP("Ton aus / an")},
        {QT_TR_NOOP("Strg+C (Video)"), QT_TR_NOOP("Aktuelles Videobild kopieren")},
        {QT_TR_NOOP("Strg+Z"), QT_TR_NOOP("Rückgängig")},
        {QT_TR_NOOP("O"), QT_TR_NOOP("Öffnen")},
        {QT_TR_NOOP("Strg+S / S"), QT_TR_NOOP("Speichern / Speichern unter")},
        {QT_TR_NOOP("F2"), QT_TR_NOOP("Umbenennen")},
        {QT_TR_NOOP("Entf"), QT_TR_NOOP("In den Papierkorb")},
        {QT_TR_NOOP("I"), QT_TR_NOOP("Bildinformationen")},
    };
    QString rows;
    for (const Entry &e : entries)
        rows += QStringLiteral("<tr><td style='padding-right:16px'><b>%1</b></td><td>%2</td></tr>")
                    .arg(tr(e.keys).toHtmlEscaped(), tr(e.what).toHtmlEscaped());

    QMessageBox box(this);
    box.setWindowTitle(tr("Tastenkürzel"));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral("<table>%1</table>").arg(rows));
    box.exec();
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("Über Kiek an"),
        tr("<h3>Kiek an %1</h3>"
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

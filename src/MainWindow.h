// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QHash>
#include <QKeySequence>
#include <QElapsedTimer>

#include <atomic>
#include <memory>
#include <QMainWindow>
#include <QPointer>
#include <QThreadPool>
#include <QVariantMap>
#include <QVector>

#include <functional>

namespace Kiekan { class Filter; }
struct LoadResult;
class HistogramDialog;
class QMovie;
class QStackedWidget;
class VideoView;
class ViewerServer;
class ImageLoader;
class ImageView;
class QThread;
class QLabel;
class QMenu;
class QMessageBox;
class QTimer;
class QToolBar;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Öffnet eine Datei (und liest ihren Ordner zum Blättern ein)
    // oder einen Ordner (zeigt dessen erstes Bild).
    void openPath(const QString &path);
    // Beim ersten Start fragen, ob KI-Assistenten das Fenster nutzen dürfen.
    void askAiViewerAccessOnce();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void createActions();

    // Navigation
    void showIndex(int index);
    void showPage();
    void applyLoadResult(const LoadResult &result);
    bool hasCurrentImage() const;
    bool canStoreText() const;
    void loadStoredText();
    void toggleTextMode(bool on, bool thorough = false);
    void rereadText();
    void recognizeFolder();

    // Bildschirmleser
    void announceCurrent();
    void announceText(const QString &summary);
    void updateCaretKeys();
    QString descriptionFor(const QString &path) const;
    QStringList upcomingFiles(int count) const;
    void askDescribeAheadOnce();
    void editDescription();
    bool storeTextLayer();
    bool resolveUnsavedText();
    void pageStep(int delta);
    void step(int delta);
    void jump(int delta);
    void keyStep(int delta, bool autoRepeat);
    void first();
    void last();
    void prefetch(int nextPage = -1);   // nextPage: zusätzlich vorzuladende Seite
    void onLoaded(const QString &path, int page, const LoadResult &result);

    // Video und Animation
    void showVideo();
    void stopMedia();
    void startAnimation();
    void stopAnimation();
    void showExif();
    void recognizeText();
    void splitPdf();
    void developRaw();
    void scanDirectory(const QString &dir, const QString &select);
    void onScanned(quint64 generation, const QStringList &files, const QString &select);

    // Datei
    void openDialog();
    bool save();
    bool saveAs();
    bool writeImage(const QString &path, const QByteArray &format, int quality, const QImage &image);
    void deleteFile();
    void renameFile();
    void batchDialog();
    void showInfo();
    bool confirmDiscard();

    // Bearbeiten
    // geoOp: LosslessJpeg::Op bei reinem Drehen/Spiegeln, sonst 0
    void applyEdit(const QImage &result, int geoOp = 0);
    void resetEdits();
    enum class Lossless { NotApplicable, Saved, Cancelled };
    Lossless saveLossless(const QString &target);
    void undo();
    void copy();
    void paste();
    void crop();
    void resizeImage();
    void rotate(int degrees);
    void flip(bool horizontal);

    // Bildkorrekturen und Effekte – wirken auf die Auswahl, falls vorhanden.
    void applyToImage(const std::function<QImage(const QImage &)> &op);
    void openFilterDialog(const QList<const Kiekan::Filter *> &filters, const Kiekan::Filter *initial);
    void runFilter(const QString &id, const QVariantMap &params = {});
    void openCurves();
    void equalizeHistogram();
    void autoColors();
    void toggleHistogram();
    void refreshHistogram();

    // Extras
    void showSettings();
    void updateViewerServer();
    QJsonObject handleViewerRequest(const QJsonObject &request);
    void showPlugins();
    void showAiLog(int filter = 0);
    void warnAboutInjection(const QJsonObject &req);
    // Ansicht
    void toggleFullScreen();
    void escape();
    void print();
    void customizeShortcuts();
    void startSlideshow();
    void stopSlideshow();
    void slideshowTick();
    void showContextMenu(const QPoint &globalPos);
    void showHelp();
    void showAbout();

    void updateTitle();
    void updateStatus();
    void message(const QString &text);

    enum class Kind { Image, Video };

    QStackedWidget *m_stack = nullptr;
    ImageView *m_view = nullptr;
    VideoView *m_video = nullptr;   // erst beim ersten Video erzeugt
    QMovie *m_movie = nullptr;      // läuft nur bei animierten Bildern
    Kind m_kind = Kind::Image;
    ImageLoader *m_loader = nullptr;
    QLabel *m_statusLabel = nullptr;
    QMenu *m_contextMenu = nullptr;
    QToolBar *m_toolBar = nullptr;
    HistogramDialog *m_histogram = nullptr;
    ViewerServer *m_viewerServer = nullptr;   // für die KI-Schnittstelle
    QPointer<QMessageBox> m_injectionWarning;
    int m_injectionCount = 0;

    QStringList m_files;          // Bilder im aktuellen Ordner, natürlich sortiert
    int m_index = -1;
    int m_lastStep = 1;           // Blätterrichtung, für das Vorladen
    QElapsedTimer m_holdTimer;    // Blättern bei gedrückt gehaltener Taste
    int m_page = 0;               // Seite in PDF/TIFF
    int m_pageCount = 1;
    bool m_rawPreview = false;      // Kamera-RAW: nur eingebettete Vorschau angezeigt
    bool m_developing = false;
    QSize m_rawFullSize;
    int m_partOf = 0, m_partNumber = 0, m_partFirstPage = 0;   // KiekAn!-Teil einer geteilten PDF
    QString m_path;               // leer = Bild aus der Zwischenablage
    QString m_loadError;
    bool m_modified = false;
    QString m_shownPath;          // Datei/Seite, zu der das angezeigte Bild gehört
    int m_shownPage = -1;
    bool m_animated = false;
    // Text markieren
    QAction *m_textAct = nullptr;
    class ProgressToolButton *m_textButton = nullptr;
    quint64 m_ocrGeneration = 0;
    std::shared_ptr<std::atomic<bool>> m_ocrCancel;
    QList<QThread *> m_ocrThreads;
    bool m_textUnsaved = false;   // erkannter Text noch nicht in der Datei
    bool m_ocrRotation = false;   // Bild wurde von der Texterkennung aufgerichtet
    bool m_deepColor = false;
    QHash<QString, QString> m_aiDescriptions;   // Bildbeschreibungen der KI (Pfad → Text)
    bool m_describeQuestionShown = false;
    QVector<QImage> m_undo;
    // Seit dem Laden/Speichern: nur Drehungen/Spiegelungen? (für verlustfreies JPEG)
    QVector<int> m_editOps;
    bool m_editOpsValid = true;
    quint64 m_scanGeneration = 0;
    bool m_wasMaximized = false;
    QList<QAction *> m_actions;                                  // alle Aktionen mit Kennung
    QHash<QString, QList<QKeySequence>> m_defaultShortcuts;
    QTimer *m_slideTimer = nullptr;   // Diashow; läuft nur, solange nicht pausiert
    bool m_slideshow = false;
    bool m_slideLoop = true;
    bool m_slideFullScreen = false;   // Vollbild wurde von der Diashow eingeschaltet

    QThreadPool m_scanPool;       // zuletzt deklariert: wartet beim Beenden zuerst
};

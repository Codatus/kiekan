// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
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
class QLabel;
class QMenu;
class QMessageBox;
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

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void createActions();

    // Navigation
    void showIndex(int index);
    void showPage();
    void applyLoadResult(const LoadResult &result);
    void pageStep(int delta);
    void step(int delta);
    void first();
    void last();
    void prefetch();
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
    void showInfo();
    bool confirmDiscard();

    // Bearbeiten
    void applyEdit(const QImage &result);
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
    int m_page = 0;               // Seite in PDF/TIFF
    int m_pageCount = 1;
    bool m_rawPreview = false;      // Kamera-RAW: nur eingebettete Vorschau angezeigt
    bool m_developing = false;
    QSize m_rawFullSize;
    int m_partOf = 0, m_partNumber = 0, m_partFirstPage = 0;   // KiekAn!-Teil einer geteilten PDF
    QString m_path;               // leer = Bild aus der Zwischenablage
    QString m_loadError;
    bool m_modified = false;
    QVector<QImage> m_undo;
    quint64 m_scanGeneration = 0;
    bool m_wasMaximized = false;

    QThreadPool m_scanPool;       // zuletzt deklariert: wartet beim Beenden zuerst
};

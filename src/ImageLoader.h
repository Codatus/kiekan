// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThreadPool>

struct LoadResult
{
    QImage image;
    QString error;
    int pageCount = 1;       // PDF-Seiten oder TIFF-Seiten
    bool animated = false;   // animiertes GIF/WebP: Wiedergabe über QMovie
    bool rawPreview = false; // Kamera-RAW: nur eingebettete Vorschau geladen
    QSize rawFullSize;       // volle Größe des RAW (falls bekannt)
};

// Dekodiert Bilder (und PDF-Seiten) in Hintergrund-Threads und hält eine
// kleine Menge fertig dekodierter Bilder vor (aktuelles Bild plus Nachbarn),
// damit das Blättern ohne Wartezeit funktioniert.
class ImageLoader : public QObject
{
    Q_OBJECT

public:
    explicit ImageLoader(QObject *parent = nullptr);
    ~ImageLoader() override;

    // Startet das Laden, falls das Bild weder vorliegt noch gerade lädt.
    void request(const QString &path, int page = 0);

    bool contains(const QString &path, int page = 0) const { return m_cache.contains(key(path, page)); }
    LoadResult result(const QString &path, int page = 0) const { return m_cache.value(key(path, page)); }

    // Verwirft alle zwischengespeicherten Bilder außer den genannten (Seite 0
    // bzw. die als Paar übergebenen Seiten).
    void retainOnly(const QList<QPair<QString, int>> &keep);
    void remove(const QString &path);
    void clear();

    // Liest ein Bild synchron (für die Worker-Threads und Sonderfälle).
    // `pdfDpi`: Auflösung für PDF-Seiten (Anzeige 200, Texterkennung 300).
    // `fullQuality`: Kamera-RAW voll entwickeln statt Vorschau (langsam).
    static LoadResult decode(const QString &path, int page, double pdfDpi = 200, bool fullQuality = false);

    // EXIF-Ausrichtung anwenden (Standard: ja). Gilt für künftige Ladevorgänge.
    static void setAutoRotate(bool on);

Q_SIGNALS:
    void loaded(const QString &path, int page, const LoadResult &result);

private:
    static QString key(const QString &path, int page);
    void finish(const QString &path, int page, const LoadResult &result);

    QHash<QString, LoadResult> m_cache;
    QSet<QString> m_pending;
    QSet<QString> m_wanted;   // leer = alles behalten
    QThreadPool m_pool;
};

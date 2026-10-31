// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>

#include <algorithm>

#include "TextLayer.h"

// Zeichnet ein Bild mit Zoom, Verschieben und Rechteckauswahl.
//
// Für die Geschwindigkeit wichtig: Bei Verkleinerung wird das Bild einmal
// geglättet herunterskaliert und zwischengespeichert; danach wird nur noch
// die fertige Kopie gezeichnet. Bei Vergrößerung wird nur der sichtbare
// Ausschnitt gezeichnet.
class ImageView : public QWidget
{
    Q_OBJECT

public:
    explicit ImageView(QWidget *parent = nullptr);

    void setImage(const QImage &image);          // setzt Zoom auf „Einpassen“
    void replaceImage(const QImage &image);      // nach Bearbeitung: Einpassen beibehalten
    void setFrame(const QImage &frame);          // Animationsbild: Zoom und Auswahl bleiben
    void clear();
    const QImage &image() const { return m_image; }
    bool hasImage() const { return !m_image.isNull(); }

    double zoom() const { return m_zoom; }
    bool isFitMode() const { return m_fit; }
    void setFitMode();
    void setZoom(double zoom);                   // um die Fenstermitte
    void zoomIn();
    void zoomOut();

    // Ob das Bild größer als das Fenster ist und verschoben werden kann.
    bool canPan() const;
    void panBy(int dx, int dy);

    QRect selection() const { return m_selection; }
    bool hasSelection() const { return !m_selection.isEmpty(); }
    void clearSelection();
    void setSelection(const QRect &rect);   // in Bildkoordinaten
    void selectAll();

    void setBackground(const QColor &color);

    // Erkannter Text: Im Textmodus markiert die linke Maustaste Wörter statt
    // eines Rechtecks. Die Rahmen gelten für das aktuelle Bild; ein neues
    // oder verändertes Bild löscht die Textschicht.
    void setTextLayer(const TextLayer &layer);
    void clearTextLayer();
    const TextLayer &textLayer() const { return m_text; }
    bool hasTextLayer() const { return !m_text.isEmpty(); }
    void setTextMode(bool on);
    bool textMode() const { return m_textMode; }
    bool hasTextSelection() const { return m_textFrom >= 0; }
    QString selectedText() const;
    void selectAllText();

    // Bildschirmleser: Name (z. B. Dateiname und Position) und Beschreibung
    // der Bildfläche. Der erkannte Text ist als Lesetext mit Textcursor
    // zugänglich; mit `caretKeys` bewegen auch die Pfeiltasten ohne Umschalt
    // den Cursor (sonst blättern sie).
    void setAccessibleInfo(const QString &name, const QString &description);
    QString accessibleInfoName() const { return m_a11yName; }
    QString accessibleInfoDescription() const { return m_a11yDescription; }
    void setCaretKeys(bool on) { m_caretKeys = on; }
    const QString &fullText() const { return m_textFull; }
    int caretOffset() const { return std::max(0, m_caret); }
    void setCaretOffset(int offset);
    // Markierung als Zeichenbereich [start, end) in fullText(); leer = keine
    void selectionOffsets(int *start, int *end) const;
    void setSelectionOffsets(int start, int end);
    int wordAtOffset(int offset) const;
    QRect characterRect(int offset) const;   // in Widget-Koordinaten
    int offsetAt(const QPoint &widgetPos) const;

Q_SIGNALS:
    void imageChanged();
    void zoomChanged(double zoom);
    void selectionChanged(const QRect &selection);
    void navigateRequested(int step);            // Mausrad ohne Strg
    void doubleClicked();
    void contextMenuRequested(const QPoint &globalPos);
    void textLayerCleared();
    void textSelectionChanged();

protected:
    bool event(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    double fitZoom() const;
    QSizeF scaledSize() const;
    QRectF imageRect() const;                    // Bildfläche in Widget-Koordinaten
    QPointF toImage(const QPointF &widgetPos) const;
    QPointF toWidget(const QPointF &imagePos) const;
    void zoomAt(double zoom, const QPointF &widgetPos);
    void clampOffset();
    void invalidateScaled(bool immediate);
    void rebuildScaled();
    int wordAt(const QPointF &imagePos, bool nearest) const;
    bool caretKey(const QKeyEvent *event) const;
    void moveCaret(int offset, bool extend);
    void notifyTextSelection();

    QImage m_image;
    QImage m_scaled;          // geglättete Kopie für Zoom < 1
    double m_scaledZoom = 0;
    qint64 m_scaledKey = 0;   // cacheKey des Bildes, aus dem m_scaled entstand
    QTimer m_smoothTimer;

    double m_zoom = 1.0;
    bool m_fit = true;
    QPointF m_offset;         // linke obere Bildecke, falls größer als Fenster

    QRect m_selection;        // in Bildkoordinaten
    bool m_selecting = false;
    QPointF m_selectStart;

    bool m_panning = false;
    bool m_panMoved = false;
    QPoint m_panLast;
    QPoint m_pressPos;

    int m_wheelAccum = 0;     // für Touchpads mit feinen Radschritten

    QColor m_background;

    TextLayer m_text;
    bool m_textMode = false;
    bool m_textSelecting = false;
    int m_textAnchor = -1;
    int m_textFrom = -1;      // markierte Wörter from..to, -1 = keine
    int m_textTo = -1;

    QString m_textFull;       // m_text.text(), einmal berechnet
    QList<int> m_wordStart;   // Anfang jedes Wortes in m_textFull
    int m_caret = -1;         // Textcursor (Zeichen), -1 = noch nicht benutzt
    bool m_caretKeys = false;
    QString m_a11yName;
    QString m_a11yDescription;
};

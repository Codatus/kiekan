// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <kiekan/FilterInterface.h>

#include <QDialog>
#include <QImage>
#include <QTimer>

class PreviewLabel;
class QFormLayout;
class QListWidget;
class QWidget;

// Dialog für einen oder mehrere Filter: Regler werden aus den
// Filter-Parametern erzeugt, die Vorschau aktualisiert sich sofort.
class FilterDialog : public QDialog
{
    Q_OBJECT

public:
    FilterDialog(const QList<const Kiekan::Filter *> &filters, const Kiekan::Filter *initial,
                 const QImage &image, QWidget *parent = nullptr);

    const Kiekan::Filter *filter() const { return m_filter; }
    QVariantMap parameters() const { return m_values; }

private:
    void setFilter(const Kiekan::Filter *filter);
    void buildControls();
    void schedulePreview();
    void renderPreview();

    QList<const Kiekan::Filter *> m_filters;
    const Kiekan::Filter *m_filter = nullptr;
    QVariantMap m_values;

    QListWidget *m_list = nullptr;
    PreviewLabel *m_preview = nullptr;
    QWidget *m_controls = nullptr;
    QFormLayout *m_form = nullptr;

    QImage m_source;     // verkleinerte Vorschau-Quelle
    double m_scale = 1.0;
    QTimer m_timer;
};

// Vorschaufläche mit „Vorher“-Umschaltung (gedrückt halten).
class PreviewLabel : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewLabel(QWidget *parent = nullptr);
    void setImages(const QImage &before, const QImage &after);
    void setAfter(const QImage &after);
    void showBefore(bool before);
    QSize sizeHint() const override { return {640, 480}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_before;
    QImage m_after;
    bool m_showBefore = false;
};

// Verkleinert ein Bild für die Vorschau; liefert den Maßstab zurück.
QImage makePreviewSource(const QImage &image, const QSize &box, double *scale);

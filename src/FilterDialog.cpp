// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FilterDialog.h"

#include "FilterRegistry.h"
#include "ImageOps.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>

using Kiekan::Filter;
using Kiekan::FilterParameter;

QImage makePreviewSource(const QImage &image, const QSize &box, double *scale)
{
    QImage src = image;
    if (image.width() > box.width() || image.height() > box.height())
        src = image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (scale)
        *scale = image.width() > 0 ? double(src.width()) / image.width() : 1.0;
    return ImageOps::forDisplay(src);
}

// --- PreviewLabel ----------------------------------------------------------

PreviewLabel::PreviewLabel(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PreviewLabel::setImages(const QImage &before, const QImage &after)
{
    m_before = before;
    m_after = after;
    update();
}

void PreviewLabel::setAfter(const QImage &after)
{
    m_after = after;
    update();
}

void PreviewLabel::showBefore(bool before)
{
    m_showBefore = before;
    update();
}

void PreviewLabel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x2b, 0x2b, 0x2b));
    const QImage &img = m_showBefore ? m_before : m_after;
    if (img.isNull())
        return;
    // Schachbrett hinter transparenten Bildern
    QSizeF size = img.size();
    size.scale(QSizeF(this->size()).boundedTo(size), Qt::KeepAspectRatio);
    const QRectF target(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    if (img.hasAlphaChannel()) {
        QPixmap checker(16, 16);
        checker.fill(Qt::lightGray);
        QPainter cp(&checker);
        cp.fillRect(0, 0, 8, 8, Qt::white);
        cp.fillRect(8, 8, 8, 8, Qt::white);
        cp.end();
        p.fillRect(target, QBrush(checker));
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(target, img);
}

// --- FilterDialog ----------------------------------------------------------

FilterDialog::FilterDialog(const QList<const Filter *> &filters, const Filter *initial,
                           const QImage &image, QWidget *parent)
    : QDialog(parent)
    , m_filters(filters)
{
    m_source = makePreviewSource(image, QSize(800, 600), &m_scale);

    m_preview = new PreviewLabel(this);
    m_preview->setImages(m_source, m_source);

    auto *beforeButton = new QPushButton(tr("Vorher (gedrückt halten)"), this);
    beforeButton->setAutoDefault(false);
    connect(beforeButton, &QPushButton::pressed, this, [this] { m_preview->showBefore(true); });
    connect(beforeButton, &QPushButton::released, this, [this] { m_preview->showBefore(false); });

    auto *resetButton = new QPushButton(tr("Zurücksetzen"), this);
    resetButton->setAutoDefault(false);
    connect(resetButton, &QPushButton::clicked, this, [this] {
        m_values = FilterRegistry::defaults(m_filter);
        buildControls();
        schedulePreview();
    });

    m_controls = new QWidget(this);
    m_form = new QFormLayout(m_controls);
    m_form->setContentsMargins(0, 0, 0, 0);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *side = new QVBoxLayout;
    side->addWidget(m_controls);
    side->addStretch(1);
    auto *row = new QHBoxLayout;
    row->addWidget(beforeButton);
    row->addWidget(resetButton);
    side->addLayout(row);
    side->addWidget(buttons);

    auto *main = new QHBoxLayout(this);
    if (filters.size() > 1) {
        m_list = new QListWidget(this);
        m_list->setAccessibleName(tr("Effekte"));
        m_list->setMaximumWidth(200);
        for (const Filter *f : filters)
            m_list->addItem(f->name());
        connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
            if (row >= 0 && row < m_filters.size())
                setFilter(m_filters.at(row));
        });
        main->addWidget(m_list);
    }
    main->addWidget(m_preview, 1);
    main->addLayout(side);

    m_timer.setSingleShot(true);
    m_timer.setInterval(30);
    connect(&m_timer, &QTimer::timeout, this, &FilterDialog::renderPreview);

    const Filter *start = initial ? initial : (filters.isEmpty() ? nullptr : filters.first());
    if (m_list && start)
        m_list->setCurrentRow(int(filters.indexOf(start)));
    else
        setFilter(start);
}

void FilterDialog::setFilter(const Filter *filter)
{
    m_filter = filter;
    if (!filter)
        return;
    setWindowTitle(filter->name());
    m_values = FilterRegistry::defaults(filter);
    buildControls();
    schedulePreview();
}

void FilterDialog::buildControls()
{
    while (m_form->rowCount() > 0)
        m_form->removeRow(0);
    if (!m_filter)
        return;

    const auto params = m_filter->parameters();
    if (params.isEmpty())
        m_form->addRow(new QLabel(tr("Keine Einstellungen."), m_controls));

    for (const FilterParameter &p : params) {
        const double current = m_values.value(p.id, p.defaultValue).toDouble();
        if (p.type == FilterParameter::Toggle) {
            auto *box = new QCheckBox(p.label, m_controls);
            box->setChecked(current > 0.5);
            connect(box, &QCheckBox::toggled, this, [this, id = p.id](bool on) {
                m_values.insert(id, on ? 1.0 : 0.0);
                schedulePreview();
            });
            m_form->addRow(box);
            continue;
        }

        // Regler arbeitet ganzzahlig in der kleinsten Nachkommastelle.
        const double factor = std::pow(10.0, p.decimals);
        auto *slider = new QSlider(Qt::Horizontal, m_controls);
        slider->setRange(int(std::lround(p.minimum * factor)), int(std::lround(p.maximum * factor)));
        slider->setValue(int(std::lround(current * factor)));
        slider->setMinimumWidth(180);

        auto *spin = new QDoubleSpinBox(m_controls);
        spin->setRange(p.minimum, p.maximum);
        spin->setDecimals(p.decimals);
        spin->setSingleStep(p.decimals > 0 ? 1.0 / factor : 1.0);
        spin->setSuffix(p.suffix);
        spin->setValue(current);

        connect(slider, &QSlider::valueChanged, spin, [spin, factor](int v) {
            spin->setValue(v / factor);
        });
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, slider, factor, id = p.id](double v) {
                    const QSignalBlocker block(slider);
                    slider->setValue(int(std::lround(v * factor)));
                    m_values.insert(id, v);
                    schedulePreview();
                });

        // Bildschirmleser: Regler und Zahlenfeld stehen ohne eigenes Label
        slider->setAccessibleName(p.label);
        spin->setAccessibleName(p.label);
        auto *row = new QWidget(m_controls);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(slider, 1);
        layout->addWidget(spin);
        m_form->addRow(p.label, row);
    }
}

void FilterDialog::schedulePreview()
{
    m_timer.start();
}

void FilterDialog::renderPreview()
{
    if (!m_filter)
        return;
    m_preview->setAfter(FilterRegistry::run(m_filter, m_source, m_values, m_scale));
}

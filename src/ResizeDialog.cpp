// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResizeDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

ResizeDialog::ResizeDialog(const QSize &original, QWidget *parent)
    : QDialog(parent)
    , m_original(original)
{
    setWindowTitle(tr("Größe ändern"));

    m_width = new QSpinBox(this);
    m_width->setRange(1, 100000);
    m_width->setSuffix(tr(" px"));
    m_width->setValue(original.width());

    m_height = new QSpinBox(this);
    m_height->setRange(1, 100000);
    m_height->setSuffix(tr(" px"));
    m_height->setValue(original.height());

    m_percent = new QDoubleSpinBox(this);
    m_percent->setRange(0.1, 10000);
    m_percent->setDecimals(1);
    m_percent->setSuffix(tr(" %"));
    m_percent->setValue(100);

    m_keepRatio = new QCheckBox(tr("Seitenverhältnis beibehalten"), this);
    m_keepRatio->setChecked(true);
    m_smooth = new QCheckBox(tr("Geglättet (bessere Qualität)"), this);
    m_smooth->setChecked(true);

    auto *form = new QFormLayout;
    form->addRow(new QLabel(tr("Original: %1 × %2 px").arg(original.width()).arg(original.height())));
    form->addRow(tr("&Breite:"), m_width);
    form->addRow(tr("&Höhe:"), m_height);
    form->addRow(tr("&Prozent:"), m_percent);
    form->addRow(m_keepRatio);
    form->addRow(m_smooth);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this, &ResizeDialog::widthChanged);
    connect(m_height, qOverload<int>(&QSpinBox::valueChanged), this, &ResizeDialog::heightChanged);
    connect(m_percent, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ResizeDialog::percentChanged);

    m_width->selectAll();
    m_width->setFocus();
}

QSize ResizeDialog::targetSize() const
{
    return QSize(m_width->value(), m_height->value());
}

bool ResizeDialog::smooth() const
{
    return m_smooth->isChecked();
}

void ResizeDialog::widthChanged(int w)
{
    if (m_updating)
        return;
    m_updating = true;
    const double p = 100.0 * w / m_original.width();
    if (m_keepRatio->isChecked())
        m_height->setValue(std::max(1, int(std::lround(m_original.height() * w / double(m_original.width())))));
    m_percent->setValue(p);
    m_updating = false;
}

void ResizeDialog::heightChanged(int h)
{
    if (m_updating)
        return;
    m_updating = true;
    const double p = 100.0 * h / m_original.height();
    if (m_keepRatio->isChecked()) {
        m_width->setValue(std::max(1, int(std::lround(m_original.width() * h / double(m_original.height())))));
        m_percent->setValue(p);
    }
    m_updating = false;
}

void ResizeDialog::percentChanged(double p)
{
    if (m_updating)
        return;
    m_updating = true;
    m_width->setValue(std::max(1, int(std::lround(m_original.width() * p / 100.0))));
    m_height->setValue(std::max(1, int(std::lround(m_original.height() * p / 100.0))));
    m_updating = false;
}

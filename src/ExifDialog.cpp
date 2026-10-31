// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ExifDialog.h"

#include "Metadata.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

ExifDialog::ExifDialog(const QString &path, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("EXIF-Daten – %1").arg(QFileInfo(path).fileName()));

    const Metadata::Summary s = Metadata::summary(path);
    QString error;
    const QList<Metadata::Entry> entries = Metadata::read(path, &error);

    auto *layout = new QVBoxLayout(this);

    if (!Metadata::available()) {
        layout->addWidget(new QLabel(tr("Diese Version wurde ohne EXIF-Unterstützung gebaut."), this));
    } else if (!s.isEmpty()) {
        auto *form = new QFormLayout;
        auto add = [&](const QString &label, const QString &value) {
            if (value.isEmpty())
                return;
            auto *l = new QLabel(value, this);
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            form->addRow(label, l);
        };
        add(tr("Kamera:"), s.camera);
        add(tr("Objektiv:"), s.lens);
        add(tr("Aufgenommen:"), s.dateTaken);
        add(tr("Belichtung:"), s.exposure);
        add(tr("Blende:"), s.aperture);
        add(tr("ISO:"), s.iso);
        add(tr("Brennweite:"), s.focalLength);
        if (s.hasGps) {
            const QLocale loc;
            QString gps = QStringLiteral("%1, %2").arg(loc.toString(s.latitude, 'f', 6), loc.toString(s.longitude, 'f', 6));
            if (s.hasAltitude)
                gps += tr(" · %1 m Höhe").arg(loc.toString(s.altitude, 'f', 0));
            auto *row = new QHBoxLayout;
            auto *label = new QLabel(gps, this);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            auto *map = new QPushButton(tr("In OpenStreetMap zeigen"), this);
            map->setAutoDefault(false);
            const double lat = s.latitude, lon = s.longitude;
            connect(map, &QPushButton::clicked, this, [lat, lon] {
                QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=16/%1/%2")
                                                   .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6)));
            });
            row->addWidget(label);
            row->addWidget(map);
            row->addStretch(1);
            form->addRow(tr("Ort:"), row);
        }
        layout->addLayout(form);
    }

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Suchen …"));
    m_search->setAccessibleName(tr("Suchen"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, &ExifDialog::applyFilter);

    m_table = new QTableWidget(int(entries.size()), 3, this);
    m_table->setHorizontalHeaderLabels({tr("Bereich"), tr("Eigenschaft"), tr("Wert")});
    m_table->setAccessibleName(tr("Metadaten"));
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setStretchLastSection(true);
    for (int i = 0; i < entries.size(); ++i) {
        const Metadata::Entry &e = entries.at(i);
        auto *label = new QTableWidgetItem(e.label.isEmpty() ? e.key : e.label);
        label->setToolTip(e.key);
        m_table->setItem(i, 0, new QTableWidgetItem(e.group));
        m_table->setItem(i, 1, label);
        auto *value = new QTableWidgetItem(e.value);
        value->setToolTip(e.value);
        m_table->setItem(i, 2, value);
    }
    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(2, std::max(m_table->columnWidth(2), 260));

    if (entries.isEmpty() && Metadata::available()) {
        layout->addWidget(new QLabel(error.isEmpty() ? tr("Diese Datei enthält keine Metadaten.")
                                                     : tr("Metadaten nicht lesbar: %1").arg(error), this));
    } else {
        layout->addWidget(m_search);
        layout->addWidget(m_table, 1);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *copy = buttons->addButton(tr("Alles kopieren"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, this, [entries] {
        QString text;
        for (const Metadata::Entry &e : entries)
            text += e.key + QLatin1Char('\t') + e.value + QLatin1Char('\n');
        QGuiApplication::clipboard()->setText(text);
    });
    copy->setEnabled(!entries.isEmpty());
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    resize(720, 600);
}

void ExifDialog::applyFilter(const QString &text)
{
    for (int r = 0; r < m_table->rowCount(); ++r) {
        bool match = text.isEmpty();
        for (int c = 0; c < 3 && !match; ++c) {
            const QTableWidgetItem *item = m_table->item(r, c);
            match = item && (item->text().contains(text, Qt::CaseInsensitive)
                             || item->toolTip().contains(text, Qt::CaseInsensitive));
        }
        m_table->setRowHidden(r, !match);
    }
}

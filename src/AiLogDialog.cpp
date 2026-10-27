// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AiLogDialog.h"

#include "AiGuard.h"

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QString joined(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &x : v.toArray())
        out << x.toString();
    return out.join(QLatin1Char('\n'));
}

} // namespace

AiLogDialog::AiLogDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("KI-Protokoll"));
    resize(1000, 560);

    m_filter = new QComboBox(this);
    m_filter->addItem(tr("Alle Zugriffe"), All);
    m_filter->addItem(tr("Nur Änderungen an Dateien"), Changes);
    m_filter->addItem(tr("Nur Warnungen (verdächtiger Text)"), Warnings);
    connect(m_filter, &QComboBox::currentIndexChanged, this, &AiLogDialog::reload);

    auto *refresh = new QPushButton(tr("Aktualisieren"), this);
    connect(refresh, &QPushButton::clicked, this, &AiLogDialog::reload);
    auto *openFolder = new QPushButton(tr("Protokollordner öffnen"), this);
    connect(openFolder, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(AiGuard::logPath()).absolutePath()));
    });

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Anzeigen:"), this));
    top->addWidget(m_filter);
    top->addStretch(1);
    top->addWidget(refresh);
    top->addWidget(openFolder);

    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({tr("Zeit"), tr("Werkzeug"), tr("Ergebnis"), tr("Gelesen / geschrieben"), tr("Warnung")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setWordWrap(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    // Zeilenhöhe erst nach dem endgültigen Spaltenumbruch bestimmen
    connect(m_table->horizontalHeader(), &QHeaderView::sectionResized, m_table, &QTableWidget::resizeRowsToContents);

    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setStyleSheet(QStringLiteral("color: gray"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_info);
    layout->addWidget(buttons);

    reload();
}

void AiLogDialog::setFilter(Filter filter)
{
    m_filter->setCurrentIndex(std::max(0, m_filter->findData(filter)));
}

void AiLogDialog::reload()
{
    const Filter filter = Filter(m_filter->currentData().toInt());
    const QList<QJsonObject> entries = AiGuard::readLog();
    int changes = 0, warnings = 0;

    m_table->setRowCount(0);
    // Neueste zuerst
    for (auto it = entries.crbegin(); it != entries.crend(); ++it) {
        const QJsonObject &e = *it;
        const bool wrote = e.contains(QLatin1String("written"));
        const bool warned = e.contains(QLatin1String("warnings"));
        changes += wrote ? 1 : 0;
        warnings += warned ? 1 : 0;
        if ((filter == Changes && !wrote) || (filter == Warnings && !warned))
            continue;

        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QDateTime when = QDateTime::fromString(e.value(QLatin1String("time")).toString(), Qt::ISODate);
        auto *time = new QTableWidgetItem(QLocale().toString(when, QLocale::ShortFormat));
        QString tool = e.value(QLatin1String("tool")).toString();
        if (e.contains(QLatin1String("client")))
            tool += QStringLiteral("\n(%1)").arg(e.value(QLatin1String("client")).toString());
        const bool ok = e.value(QLatin1String("ok")).toBool();
        auto *result = new QTableWidgetItem(ok ? tr("ausgeführt") : tr("abgelehnt/Fehler:\n%1").arg(e.value(QLatin1String("error")).toString().left(300)));

        QString files;
        if (wrote)
            files += tr("Geschrieben:") + QLatin1Char('\n') + joined(e.value(QLatin1String("written")));
        if (e.contains(QLatin1String("read"))) {
            if (!files.isEmpty())
                files += QLatin1Char('\n');
            files += tr("Gelesen:") + QLatin1Char('\n') + joined(e.value(QLatin1String("read")));
        }
        if (e.value(QLatin1String("overwrite")).toBool())
            files += QLatin1Char('\n') + tr("(mit Überschreiben)");

        QStringList warn;
        for (const QJsonValue &w : e.value(QLatin1String("warnings")).toArray())
            warn << QStringLiteral("%1: «%2»").arg(w.toObject().value(QLatin1String("kind")).toString(),
                                                   w.toObject().value(QLatin1String("excerpt")).toString());

        for (const QJsonValue &w : e.value(QLatin1String("trusted_findings")).toArray())
            warn << tr("(eigener Ordner, nicht gewarnt) %1: «%2»").arg(w.toObject().value(QLatin1String("kind")).toString(),
                                                                     w.toObject().value(QLatin1String("excerpt")).toString());

        const QList<QTableWidgetItem *> items{time, new QTableWidgetItem(tool), result,
                                              new QTableWidgetItem(files), new QTableWidgetItem(warn.join(QLatin1Char('\n')))};
        for (int c = 0; c < items.size(); ++c) {
            QTableWidgetItem *item = items.at(c);
            if (warned)
                item->setBackground(QColor(255, 210, 200));
            else if (wrote)
                item->setBackground(QColor(255, 240, 200));
            if (warned || wrote)
                item->setForeground(Qt::black);
            m_table->setItem(row, c, item);
        }
    }
    m_table->resizeColumnToContents(0);
    m_table->resizeColumnToContents(1);
    m_table->setColumnWidth(2, 220);
    QTimer::singleShot(0, m_table, &QTableWidget::resizeRowsToContents);
    m_info->setText(tr("%1 Einträge, davon %2 mit Dateiänderungen und %3 mit Warnungen. Gelb: Dateien geschrieben; "
                       "rot: verdächtiger Text in einer Datei. Protokolldatei: %4")
                        .arg(entries.size()).arg(changes).arg(warnings).arg(AiGuard::logPath()));
}

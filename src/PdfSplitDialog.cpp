// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PdfSplitDialog.h"

#include "PdfSplit.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

PdfSplitDialog::PdfSplitDialog(const QString &pdfPath, QWidget *parent)
    : QDialog(parent)
    , m_path(pdfPath)
{
    const QFileInfo fi(pdfPath);
    setWindowTitle(tr("PDF teilen – %1").arg(fi.fileName()));
    QSettings settings;

    QString error;
    const int pages = PdfSplit::pageCount(pdfPath, &error);
    auto *info = new QLabel(pages > 0 ? tr("%1 Seiten · %2 MB").arg(pages).arg(fi.size() / 1e6, 0, 'f', 1)
                                      : tr("PDF nicht lesbar: %1").arg(error), this);

    m_bySize = new QRadioButton(tr("Nach Größe, höchstens"), this);
    m_megabytes = new QDoubleSpinBox(this);
    m_megabytes->setRange(0.5, 2000);
    m_megabytes->setDecimals(1);
    m_megabytes->setSuffix(tr(" MB je Teil"));
    m_megabytes->setValue(settings.value(QStringLiteral("split/megabytes"), 25.0).toDouble());
    m_megabytes->setToolTip(tr("Claude Desktop nimmt PDFs bis 30 MB an; 25 MB lassen Spielraum."));

    m_byPages = new QRadioButton(tr("Nach Seitenzahl:"), this);
    m_pages = new QSpinBox(this);
    m_pages->setRange(1, 100000);
    m_pages->setSuffix(tr(" Seiten je Teil"));
    m_pages->setValue(settings.value(QStringLiteral("split/pages"), 100).toInt());
    m_bySize->setChecked(true);

    m_bookmarks = new QCheckBox(tr("Möglichst an Lesezeichen (Abschnittsanfängen) schneiden"), this);
    m_bookmarks->setChecked(true);

    m_outDir = new QLineEdit(fi.dir().filePath(fi.completeBaseName() + QStringLiteral("_Teile")), this);
    auto *browse = new QPushButton(tr("…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Zielordner"), m_outDir->text());
        if (!d.isEmpty())
            m_outDir->setText(d);
    });

    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(m_bySize);
    sizeRow->addWidget(m_megabytes);
    sizeRow->addStretch(1);
    auto *pageRow = new QHBoxLayout;
    pageRow->addWidget(m_byPages);
    pageRow->addWidget(m_pages);
    pageRow->addStretch(1);
    auto *dirRow = new QHBoxLayout;
    dirRow->addWidget(m_outDir, 1);
    dirRow->addWidget(browse);

    auto *form = new QFormLayout;
    form->addRow(tr("Datei:"), info);
    form->addRow(sizeRow);
    form->addRow(pageRow);
    form->addRow(m_bookmarks);
    form->addRow(tr("Zielordner:"), dirRow);

    m_status = new QLabel(tr("Die Originaldatei bleibt unverändert. Jeder Teil zeigt die Originalseitenzahlen "
                             "und nennt Quelle und Seitenbereich; ein Teilungsprotokoll wird mitgeschrieben."), this);
    m_status->setWordWrap(true);
    m_result = new QPlainTextEdit(this);
    m_result->setReadOnly(true);
    m_result->setVisible(false);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_startButton = buttons->addButton(tr("Teilen"), QDialogButtonBox::ActionRole);
    m_openFolder = buttons->addButton(tr("Ordner öffnen"), QDialogButtonBox::ActionRole);
    m_openProtocol = buttons->addButton(tr("Protokoll öffnen"), QDialogButtonBox::ActionRole);
    m_openFolder->setVisible(false);
    m_openProtocol->setVisible(false);
    m_startButton->setEnabled(pages > 0 && PdfSplit::available());
    connect(m_startButton, &QPushButton::clicked, this, &PdfSplitDialog::start);
    connect(m_openFolder, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_outDir->text()));
    });
    connect(m_openProtocol, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_manifestMarkdown));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_status);
    layout->addWidget(m_result, 1);
    layout->addWidget(buttons);
    resize(640, 360);
}

PdfSplitDialog::~PdfSplitDialog()
{
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        QApplication::restoreOverrideCursor();
    }
}

void PdfSplitDialog::start()
{
    if (m_thread)
        return;
    PdfSplit::Options opt;
    if (m_bySize->isChecked())
        opt.maxMegabytes = m_megabytes->value();
    else
        opt.pagesPerPart = m_pages->value();
    opt.preferBookmarks = m_bookmarks->isChecked();
    opt.outputDir = m_outDir->text().trimmed();

    QSettings settings;
    settings.setValue(QStringLiteral("split/megabytes"), m_megabytes->value());
    settings.setValue(QStringLiteral("split/pages"), m_pages->value());

    m_startButton->setEnabled(false);
    m_status->setText(tr("Teile …"));
    QApplication::setOverrideCursor(Qt::BusyCursor);

    auto result = std::make_shared<PdfSplit::Result>();
    const QString path = m_path;
    QPointer<QLabel> status = m_status;
    m_thread = QThread::create([path, opt, result, status] {
        *result = PdfSplit::split(path, opt, [status](const QString &text) {
            QMetaObject::invokeMethod(qApp, [status, text] { if (status) status->setText(text); });
        });
    });
    connect(m_thread, &QThread::finished, this, [this, result] {
        QApplication::restoreOverrideCursor();
        m_thread->deleteLater();
        m_thread = nullptr;
        if (!result->ok) {
            m_status->setText(tr("Fehler: %1").arg(result->error));
            m_startButton->setEnabled(true);
            return;
        }
        m_status->setText(tr("Fertig: %1 Teile. Protokoll: %2").arg(result->parts.size())
                              .arg(QFileInfo(result->manifestMarkdown).fileName()));
        QString text;
        for (const PdfSplit::Part &p : result->parts)
            text += tr("Teil %1: Seiten %2–%3 · %4 MB · %5\n").arg(p.number).arg(p.firstPage).arg(p.lastPage)
                        .arg(p.bytes / 1e6, 0, 'f', 1).arg(p.file);
        for (const QString &w : result->warnings)
            text += QStringLiteral("⚠ %1\n").arg(w);
        m_result->setPlainText(text);
        m_result->setVisible(true);
        m_manifestMarkdown = result->manifestMarkdown;
        m_openFolder->setVisible(true);
        m_openProtocol->setVisible(true);
    });
    m_thread->start();
}

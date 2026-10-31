// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OcrDialog.h"

#include "Ocr.h"
#include "OcrWorker.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryFile>
#include <QThread>
#include <QVBoxLayout>

#include <memory>

OcrDialog::OcrDialog(const QImage &image, const QString &pdfText, const QString &suggestedName, QWidget *parent)
    : QDialog(parent)
    , m_image(image)
    , m_suggestedName(suggestedName)
{
    setWindowTitle(tr("Text erkennen"));

    m_language = new QComboBox(this);
    m_language->addItem(tr("Sprache automatisch wählen"), QStringLiteral("auto"));
    for (const QString &l : Ocr::installed().languages)
        m_language->addItem(QStringLiteral("%1 – %2").arg(Ocr::languageName(l), l), l);

    m_rerun = new QPushButton(tr("Erkennen"), this);
    m_rerun->setAutoDefault(false);
    connect(m_rerun, &QPushButton::clicked, this, &OcrDialog::startRecognition);

    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_text = new QPlainTextEdit(this);
    m_text->setAccessibleName(tr("Erkannter Text"));
    m_text->setLineWrapMode(QPlainTextEdit::WidgetWidth);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *copy = buttons->addButton(tr("Kopieren"), QDialogButtonBox::ActionRole);
    auto *save = buttons->addButton(tr("Als Textdatei speichern …"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setText(m_text->toPlainText()); });
    connect(save, &QPushButton::clicked, this, &OcrDialog::saveText);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *top = new QHBoxLayout;
    top->addWidget(m_language, 1);
    top->addWidget(m_rerun);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_info);
    layout->addWidget(m_text, 1);
    layout->addWidget(buttons);
    resize(760, 640);

    if (!Ocr::available()) {
        m_language->setEnabled(false);
        m_rerun->setEnabled(false);
    }
    if (!pdfText.trimmed().isEmpty()) {
        // PDF mit Textebene: exakter Text ohne Erkennung.
        m_text->setPlainText(pdfText.trimmed());
        m_info->setText(tr("Text aus der Textebene der PDF (keine Texterkennung nötig). "
                           "Mit „Erkennen“ wird die Seite trotzdem per OCR gelesen."));
    } else if (!Ocr::available()) {
        m_info->setText(tr("Texterkennung nicht verfügbar: Tesseract oder ein Sprachpaket fehlt "
                           "(z. B. %1).").arg(Ocr::installCommand(QStringLiteral("deu"))));
    } else {
        startRecognition();
    }
}

OcrDialog::~OcrDialog()
{
    if (m_thread) {
        // Dialog während der Erkennung geschlossen: Hilfsprozess beenden,
        // dann aufräumen (dauert höchstens Sekundenbruchteile).
        m_cancel = true;
        m_thread->wait();
        delete m_thread;
        QApplication::restoreOverrideCursor();
    }
}

void OcrDialog::startRecognition()
{
    if (m_thread)
        return;
    m_rerun->setEnabled(false);
    m_language->setEnabled(false);
    m_info->setText(tr("Texterkennung läuft …"));
    QApplication::setOverrideCursor(Qt::BusyCursor);

    // Bild für den Hilfsprozess ablegen (verlustfrei, mit Auflösung).
    auto file = std::make_shared<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/kiekan-ocr-XXXXXX.png"));
    if (!file->open() || !m_image.save(file.get(), "png")) {
        QApplication::restoreOverrideCursor();
        m_info->setText(tr("Fehler: %1").arg(QStringLiteral("temporary file")));
        m_rerun->setEnabled(true);
        m_language->setEnabled(true);
        return;
    }
    file->close();
    OcrWorker::Job job;
    job.imageFile = file->fileName();
    job.options.languages = m_language->currentData().toString();
    auto result = std::make_shared<Ocr::Result>();
    m_cancel = false;
    m_thread = QThread::create([this, job, file, result] { *result = OcrWorker::run(job, &m_cancel); });
    connect(m_thread, &QThread::finished, this, [this, result] {
        QApplication::restoreOverrideCursor();
        m_thread->deleteLater();
        m_thread = nullptr;
        m_rerun->setEnabled(true);
        m_language->setEnabled(true);
        if (!result->ok) {
            m_info->setText(tr("Fehler: %1").arg(result->error));
            return;
        }
        m_text->setPlainText(result->text);
        if (result->text.trimmed().isEmpty()) {
            m_info->setText(tr("Kein Text gefunden. Liegt die Markierung auf dem Text?"));
            return;
        }
        QStringList langs;
        for (const QString &l : result->languages.split(QLatin1Char('+')))
            langs << QStringLiteral("%1 (%2)").arg(Ocr::languageName(l), l);
        QString info = tr("Sprache: %1 · Sicherheit: %2 %").arg(langs.join(QStringLiteral(" + "))).arg(qRound(result->confidence));
        if (result->rotation)
            info += tr(" · Seite um %1° gedreht erkannt").arg(result->rotation);
        if (!result->textLanguage.isEmpty() && !result->languages.contains(result->textLanguage))
            info += tr("\nDer Text wirkt %1, das passende Sprachpaket fehlt aber (%2).")
                        .arg(Ocr::languageName(result->textLanguage), Ocr::packageName(result->textLanguage));
        for (const QString &h : result->hints)
            if (h.startsWith(QLatin1String("low confidence")))
                info += tr("\nGeringe Sicherheit: Der Scan ist vielleicht unscharf oder zu niedrig aufgelöst.");
        m_info->setText(info);
    });
    m_thread->start();
}

void OcrDialog::saveText()
{
    const QString file = QFileDialog::getSaveFileName(this, tr("Text speichern"), m_suggestedName,
                                                      tr("Textdateien (*.txt)"));
    if (file.isEmpty())
        return;
    QFile f(file);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(m_text->toPlainText().toUtf8());
}

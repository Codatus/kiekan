// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SettingsDialog.h"

#include "AiGuard.h"
#include "AiLogDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

namespace Settings {

QString language() { return QSettings().value(QStringLiteral("language")).toString(); }
void setLanguage(const QString &code) { QSettings().setValue(QStringLiteral("language"), code); }
bool autoRotate() { return QSettings().value(QStringLiteral("autoRotate"), true).toBool(); }
void setAutoRotate(bool on) { QSettings().setValue(QStringLiteral("autoRotate"), on); }
int jpegQuality() { return QSettings().value(QStringLiteral("jpegQuality"), 90).toInt(); }
void setJpegQuality(int q) { QSettings().setValue(QStringLiteral("jpegQuality"), q); }
bool escapeQuits() { return QSettings().value(QStringLiteral("escapeQuits"), true).toBool(); }
void setEscapeQuits(bool on) { QSettings().setValue(QStringLiteral("escapeQuits"), on); }
bool aiViewerAccess() { return QSettings().value(QStringLiteral("ai/viewerAccess"), true).toBool(); }
void setAiViewerAccess(bool on) { QSettings().setValue(QStringLiteral("ai/viewerAccess"), on); }

} // namespace Settings

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , m_initialAutoRotate(Settings::autoRotate())
    , m_initialAiAccess(Settings::aiViewerAccess())
{
    setWindowTitle(tr("Einstellungen"));

    m_language = new QComboBox(this);
    m_language->addItem(tr("Systemsprache"), QString());
    m_language->addItem(QStringLiteral("Deutsch"), QStringLiteral("de"));
    m_language->addItem(QStringLiteral("English"), QStringLiteral("en"));
    m_language->setCurrentIndex(std::max(0, m_language->findData(Settings::language())));

    m_autoRotate = new QCheckBox(tr("Bilder automatisch drehen (EXIF-Ausrichtung)"), this);
    m_autoRotate->setChecked(m_initialAutoRotate);
    m_autoRotate->setToolTip(tr("Kameras speichern hochkant aufgenommene Fotos oft quer und vermerken "
                                "die Drehung nur in den EXIF-Daten. Ist dies aktiv, werden solche "
                                "Fotos richtig herum angezeigt."));

    m_quality = new QSpinBox(this);
    m_quality->setRange(1, 100);
    m_quality->setValue(Settings::jpegQuality());

    m_escape = new QCheckBox(tr("Esc beendet das Programm (wie IrfanView)"), this);
    m_escape->setChecked(Settings::escapeQuits());

    auto *general = new QGroupBox(tr("Allgemein"), this);
    auto *gForm = new QFormLayout(general);
    gForm->addRow(tr("Sprache:"), m_language);
    auto *note = new QLabel(tr("Eine geänderte Sprache gilt nach einem Neustart."), this);
    note->setStyleSheet(QStringLiteral("color: gray"));
    gForm->addRow(QString(), note);
    gForm->addRow(m_escape);

    auto *images = new QGroupBox(tr("Bilder"), this);
    auto *iForm = new QFormLayout(images);
    iForm->addRow(m_autoRotate);
    iForm->addRow(tr("JPEG-/WebP-Qualität beim Speichern:"), m_quality);
    m_ocrJobs = new QSpinBox(this);
    m_ocrJobs->setRange(0, 64);
    m_ocrJobs->setSpecialValueText(tr("automatisch (%1)").arg(std::clamp(QThread::idealThreadCount(), 1, 8)));
    m_ocrJobs->setValue(QSettings().value(QStringLiteral("ocr/jobs"), 0).toInt());
    m_ocrJobs->setToolTip(tr("Texterkennung: so viele Seiten werden gleichzeitig gelesen, je Seite ein "
                             "einfädiger Tesseract-Prozess (wie bei ocrmypdf)."));
    iForm->addRow(tr("Texterkennung, Seiten gleichzeitig:"), m_ocrJobs);

    // KI-Schnittstelle (MCP)
    auto *aiPage = new QWidget(this);
    auto *aiLayout = new QVBoxLayout(aiPage);
    m_aiAccess = new QCheckBox(tr("KI-Assistenten dürfen das offene Fenster abfragen und steuern"), this);
    m_aiAccess->setChecked(m_initialAiAccess);
    m_aiAccess->setToolTip(tr("Erlaubt einem verbundenen Assistenten wie Claude, das angezeigte Bild und "
                              "die Auswahl abzufragen, Dateien anzuzeigen und Bereiche zu markieren."));
    aiLayout->addWidget(m_aiAccess);

    auto dirEditor = [this, aiLayout](const QString &label, const QStringList &dirs, const QString &dialogTitle) {
        auto *caption = new QLabel(label, this);
        caption->setWordWrap(true);
        aiLayout->addWidget(caption);
        auto *edit = new QPlainTextEdit(dirs.join(QLatin1Char('\n')), this);
        edit->setFixedHeight(edit->fontMetrics().lineSpacing() * 3 + 12);
        aiLayout->addWidget(edit);
        auto *add = new QPushButton(tr("Ordner hinzufügen …"), this);
        connect(add, &QPushButton::clicked, this, [this, edit, dialogTitle] {
            const QString dir = QFileDialog::getExistingDirectory(this, dialogTitle);
            if (!dir.isEmpty())
                edit->appendPlainText(dir);
        });
        auto *row = new QHBoxLayout;
        row->addWidget(add);
        row->addStretch(1);
        aiLayout->addLayout(row);
        connect(edit, &QPlainTextEdit::textChanged, this, &SettingsDialog::updateOverlapNote);
        return edit;
    };
    m_aiReadDirs = dirEditor(tr("Lesen: Ordner, deren Bilder und Dokumente die KI öffnen darf "
                                "(ein Ordner pro Zeile; leer = keine):"),
                             AiGuard::readDirs(), tr("Ordner, den die KI lesen darf"));
    m_aiWriteDirs = dirEditor(tr("Schreiben: Ordner, in denen die KI neue Dateien anlegen darf "
                                 "(leer = die KI darf nichts speichern). Diese Ordner darf sie auch lesen:"),
                              AiGuard::writeDirs(), tr("Ordner für Dateien der KI"));

    m_aiOverwrite = new QCheckBox(tr("KI darf vorhandene Dateien ersetzen und Metadaten in Dateien ändern "
                                     "(nur in den Schreibordnern)"), this);
    m_aiOverwrite->setChecked(AiGuard::allowOverwrite());
    m_aiOverwrite->setToolTip(tr("Aus: Die KI legt nur neue Dateien an und verändert nichts Vorhandenes."));
    aiLayout->addWidget(m_aiOverwrite);
    connect(m_aiOverwrite, &QCheckBox::toggled, this, &SettingsDialog::updateOverlapNote);

    m_overlapNote = new QLabel(this);
    m_overlapNote->setWordWrap(true);
    m_overlapNote->setStyleSheet(QStringLiteral("color: #b35900"));
    aiLayout->addWidget(m_overlapNote);

    m_aiTrustedDirs = dirEditor(tr("Vertrauenswürdig: Leseordner, die nur Ihre eigenen Dateien enthalten. Dort warnt "
                                   "KiekAn! nicht bei verdächtigem Text; Kennzeichnung, Protokoll und Rechte bleiben "
                                   "(nicht für Downloads, Mail-Anhänge oder eingegangene Post):"),
                                AiGuard::trustedDirs(), tr("Ordner mit ausschließlich eigenen Dateien"));
    m_trustNote = new QLabel(this);
    m_trustNote->setWordWrap(true);
    m_trustNote->setStyleSheet(QStringLiteral("color: #b35900"));
    aiLayout->addWidget(m_trustNote);

    auto *aiNote = new QLabel(tr("Text aus Dateien (Texterkennung, PDF-Text, Metadaten, Dateinamen) wird der KI als "
                               "fremder Inhalt gekennzeichnet und auf eingeschleuste Anweisungen geprüft. Jeder "
                               "Zugriff steht im KI-Protokoll."), this);
    aiNote->setWordWrap(true);
    aiNote->setStyleSheet(QStringLiteral("color: gray"));
    aiLayout->addWidget(aiNote);

    auto *aiButtons = new QHBoxLayout;
    auto *logButton = new QPushButton(tr("KI-Protokoll anzeigen …"), this);
    connect(logButton, &QPushButton::clicked, this, [this] {
        AiLogDialog dlg(this);
        dlg.exec();
    });
    auto *copyCmd = new QPushButton(tr("Befehl für Claude kopieren"), this);
    copyCmd->setToolTip(tr("Kopiert den Befehl, mit dem Claude Code KiekAn! als MCP-Server einbindet."));
    connect(copyCmd, &QPushButton::clicked, this, [] {
        QGuiApplication::clipboard()->setText(QStringLiteral("claude mcp add --scope user kiekan -- \"%1\" --mcp")
                                                  .arg(QCoreApplication::applicationFilePath()));
    });
    aiButtons->addWidget(logButton);
    aiButtons->addWidget(copyCmd);
    aiButtons->addStretch(1);
    aiLayout->addLayout(aiButtons);
    aiLayout->addStretch(1);
    updateOverlapNote();

    auto *generalPage = new QWidget(this);
    auto *gLayout = new QVBoxLayout(generalPage);
    gLayout->addWidget(general);
    gLayout->addWidget(images);
    gLayout->addStretch(1);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(generalPage, tr("Allgemein"));
    auto *aiScroll = new QScrollArea(this);
    aiScroll->setWidget(aiPage);
    aiScroll->setWidgetResizable(true);
    aiScroll->setFrameShape(QFrame::NoFrame);
    aiScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tabs->addTab(aiScroll, tr("KI-Schnittstelle"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

namespace {

QStringList dirLines(const QPlainTextEdit *edit)
{
    QStringList dirs;
    for (const QString &line : edit->toPlainText().split(QLatin1Char('\n')))
        if (!line.trimmed().isEmpty())
            dirs << line.trimmed();
    return dirs;
}

} // namespace

// Hinweis, wenn die KI dort schreiben darf, wo auch die Originale liegen.
void SettingsDialog::updateOverlapNote()
{
    if (m_trustNote && m_aiTrustedDirs) {
        QStringList concerns = AiGuard::trustConcerns(dirLines(m_aiTrustedDirs), dirLines(m_aiReadDirs), dirLines(m_aiWriteDirs));
        if (m_aiOverwrite->isChecked()) {
            for (const QString &pair : AiGuard::overlaps(dirLines(m_aiTrustedDirs), dirLines(m_aiWriteDirs)))
                concerns << tr("%1: vertrauenswürdig und zugleich beschreibbar mit Ersetzen – die riskanteste "
                               "Kombination").arg(pair);
        }
        m_trustNote->setText(concerns.isEmpty() ? QString() : tr("Hinweis zu vertrauenswürdigen Ordnern:\n• ") + concerns.join(QStringLiteral("\n• ")));
        m_trustNote->setVisible(!concerns.isEmpty());
    }

    const QStringList pairs = AiGuard::overlaps(dirLines(m_aiReadDirs), dirLines(m_aiWriteDirs));
    if (pairs.isEmpty()) {
        m_overlapNote->clear();
        m_overlapNote->hide();
        return;
    }
    QString text = tr("Hinweis: Schreib- und Leseordner überschneiden sich (%1). Die KI kann dann neue Dateien "
                      "zwischen Ihre Originale legen")
                       .arg(pairs.join(QStringLiteral("; ")));
    text += m_aiOverwrite->isChecked()
        ? tr(" und – weil Ersetzen erlaubt ist – auch Ihre Originale verändern. Eine eingeschleuste Anweisung in "
             "einer Datei könnte das ausnutzen.")
        : tr(". Vorhandene Dateien bleiben unverändert, solange Ersetzen nicht erlaubt ist.");
    m_overlapNote->setText(text);
    m_overlapNote->show();
}

bool SettingsDialog::apply()
{
    const QString lang = m_language->currentData().toString();
    const bool langChanged = lang != Settings::language();
    Settings::setLanguage(lang);
    Settings::setAutoRotate(m_autoRotate->isChecked());
    Settings::setJpegQuality(m_quality->value());
    Settings::setEscapeQuits(m_escape->isChecked());
    Settings::setAiViewerAccess(m_aiAccess->isChecked());
    AiGuard::setReadDirs(dirLines(m_aiReadDirs));
    AiGuard::setWriteDirs(dirLines(m_aiWriteDirs));
    AiGuard::setAllowOverwrite(m_aiOverwrite->isChecked());
    AiGuard::setTrustedDirs(dirLines(m_aiTrustedDirs));
    QSettings().setValue(QStringLiteral("ocr/jobs"), m_ocrJobs->value());
    return langChanged;
}

bool SettingsDialog::aiAccessChanged() const
{
    return m_aiAccess->isChecked() != m_initialAiAccess;
}

bool SettingsDialog::autoRotateChanged() const
{
    return m_autoRotate->isChecked() != m_initialAutoRotate;
}

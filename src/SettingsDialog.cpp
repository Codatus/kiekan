// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SettingsDialog.h"

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
#include <QSpinBox>
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
QStringList aiAllowedDirs() { return QSettings().value(QStringLiteral("ai/allowedDirs")).toStringList(); }
void setAiAllowedDirs(const QStringList &dirs) { QSettings().setValue(QStringLiteral("ai/allowedDirs"), dirs); }

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
    auto *ai = new QGroupBox(tr("KI-Schnittstelle (MCP)"), this);
    auto *aiLayout = new QVBoxLayout(ai);
    m_aiAccess = new QCheckBox(tr("KI-Assistenten dürfen das offene Fenster abfragen und steuern"), this);
    m_aiAccess->setChecked(m_initialAiAccess);
    m_aiAccess->setToolTip(tr("Erlaubt einem verbundenen Assistenten wie Claude, das angezeigte Bild und "
                              "die Auswahl abzufragen, Dateien anzuzeigen und Bereiche zu markieren."));
    aiLayout->addWidget(m_aiAccess);
    aiLayout->addWidget(new QLabel(tr("Dateizugriff nur in diesen Ordnern (ein Ordner pro Zeile, leer = überall):"), this));
    m_aiDirs = new QPlainTextEdit(Settings::aiAllowedDirs().join(QLatin1Char('\n')), this);
    m_aiDirs->setFixedHeight(m_aiDirs->fontMetrics().lineSpacing() * 4 + 12);
    aiLayout->addWidget(m_aiDirs);
    auto *aiButtons = new QHBoxLayout;
    auto *addDir = new QPushButton(tr("Ordner hinzufügen …"), this);
    connect(addDir, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Ordner für KI-Zugriff"));
        if (!dir.isEmpty())
            m_aiDirs->appendPlainText(dir);
    });
    auto *copyCmd = new QPushButton(tr("Befehl für Claude kopieren"), this);
    copyCmd->setToolTip(tr("Kopiert den Befehl, mit dem Claude Code Kiek an als MCP-Server einbindet."));
    connect(copyCmd, &QPushButton::clicked, this, [] {
        QGuiApplication::clipboard()->setText(QStringLiteral("claude mcp add --scope user kiekan -- \"%1\" --mcp")
                                                  .arg(QCoreApplication::applicationFilePath()));
    });
    aiButtons->addWidget(addDir);
    aiButtons->addWidget(copyCmd);
    aiButtons->addStretch(1);
    aiLayout->addLayout(aiButtons);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(general);
    layout->addWidget(images);
    layout->addWidget(ai);
    layout->addWidget(buttons);
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
    QStringList dirs;
    for (const QString &line : m_aiDirs->toPlainText().split(QLatin1Char('\n'))) {
        if (!line.trimmed().isEmpty())
            dirs << line.trimmed();
    }
    Settings::setAiAllowedDirs(dirs);
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

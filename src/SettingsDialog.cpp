// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SettingsDialog.h"

#include "AiGuard.h"
#include "AiLogDialog.h"
#include "FileAssoc.h"
#include "ViewerBridge.h"

#include <QHash>
#include <QLineEdit>
#include <QTreeWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QThread>
#include <QVBoxLayout>

namespace Settings {

QString language() { return QSettings().value(QStringLiteral("language")).toString(); }
void setLanguage(const QString &code) { QSettings().setValue(QStringLiteral("language"), code); }
bool autoRotate() { return QSettings().value(QStringLiteral("autoRotate"), true).toBool(); }
void setAutoRotate(bool on) { QSettings().setValue(QStringLiteral("autoRotate"), on); }
int jpegQuality() { return QSettings().value(QStringLiteral("jpegQuality"), 90).toInt(); }
void setJpegQuality(int q) { QSettings().setValue(QStringLiteral("jpegQuality"), q); }
bool losslessJpeg() { return QSettings().value(QStringLiteral("losslessJpeg"), true).toBool(); }
void setLosslessJpeg(bool on) { QSettings().setValue(QStringLiteral("losslessJpeg"), on); }
bool escapeQuits() { return QSettings().value(QStringLiteral("escapeQuits"), true).toBool(); }
void setEscapeQuits(bool on) { QSettings().setValue(QStringLiteral("escapeQuits"), on); }
bool aiViewerAccess() { return QSettings().value(QStringLiteral("ai/viewerAccess"), false).toBool(); }
bool aiViewerAccessDecided() { return QSettings().contains(QStringLiteral("ai/viewerAccess")); }
void setAiViewerAccess(bool on) { QSettings().setValue(QStringLiteral("ai/viewerAccess"), on); }

} // namespace Settings

namespace {

// Öffnet KiekAn! wenigstens einen Typ der Gruppe?
bool partlyDefault(const FileAssoc::Group &g)
{
    for (const QString &m : g.mimeTypes)
        if (FileAssoc::isDefault(m))
            return true;
    return false;
}

// Beschriftung, die beim Anklicken ihr Kästchen umschaltet.
class CheckCaption : public QLabel
{
public:
    CheckCaption(const QString &text, QCheckBox *box, QWidget *parent)
        : QLabel(text, parent), m_box(box)
    {
        setWordWrap(true);
        setBuddy(box);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_box->isEnabled())
            m_box->toggle();
    }

private:
    QCheckBox *m_box;
};

// Kontrollkästchen mit umbrechendem Text: Lange Texte sollen sich der
// Fensterbreite anpassen, statt das Fenster zu verbreitern.
QCheckBox *wrappingCheckBox(const QString &text, QWidget *parent, QWidget **row)
{
    auto *box = new QCheckBox(parent);
    auto *caption = new CheckCaption(text, box, parent);
    auto *w = new QWidget(parent);
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(box, 0, Qt::AlignTop);
    l->addWidget(caption, 1);
    *row = w;
    return box;
}

} // namespace

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
    // Über den Text vergleichen: findData unterscheidet je nach Qt-Version
    // zwischen leerer und Null-Zeichenkette.
    for (int i = 0; i < m_language->count(); ++i)
        if (m_language->itemData(i).toString() == Settings::language())
            m_language->setCurrentIndex(i);

    QWidget *autoRotateRow = nullptr;
    m_autoRotate = wrappingCheckBox(tr("Bilder automatisch drehen (EXIF-Ausrichtung)"), this, &autoRotateRow);
    m_autoRotate->setChecked(m_initialAutoRotate);
    m_autoRotate->setToolTip(tr("Kameras speichern hochkant aufgenommene Fotos oft quer und vermerken "
                                "die Drehung nur in den EXIF-Daten. Ist dies aktiv, werden solche "
                                "Fotos richtig herum angezeigt."));

    m_quality = new QSpinBox(this);
    m_quality->setRange(1, 100);
    m_quality->setValue(Settings::jpegQuality());

    QWidget *escapeRow = nullptr;
    m_escape = wrappingCheckBox(tr("Esc beendet das Programm (wie IrfanView)"), this, &escapeRow);
    m_escape->setChecked(Settings::escapeQuits());

    auto *general = new QGroupBox(tr("Allgemein"), this);
    auto *gForm = new QFormLayout(general);
    gForm->addRow(tr("Sprache:"), m_language);
    auto *note = new QLabel(tr("Eine geänderte Sprache gilt nach einem Neustart."), this);
    note->setStyleSheet(QStringLiteral("color: gray"));
    gForm->addRow(QString(), note);
    gForm->addRow(escapeRow);

    auto *images = new QGroupBox(tr("Bilder"), this);
    auto *iForm = new QFormLayout(images);
    iForm->addRow(autoRotateRow);
    QWidget *losslessRow = nullptr;
    m_lossless = wrappingCheckBox(tr("JPEGs verlustfrei drehen und spiegeln (ohne Neukodierung)"), this, &losslessRow);
    m_lossless->setChecked(Settings::losslessJpeg());
    m_lossless->setToolTip(tr("Wurde ein JPEG nur gedreht oder gespiegelt, ordnet KiekAn! beim Speichern die "
                              "komprimierten Bildblöcke um, statt neu zu kodieren. Passen die Bildmaße nicht "
                              "zu den Blöcken, wird nur die EXIF-Ausrichtung geändert – auch das ist verlustfrei."));
    iForm->addRow(losslessRow);
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
    QWidget *aiAccessRow = nullptr;
    m_aiAccess = wrappingCheckBox(tr("KI-Assistenten dürfen das offene Fenster abfragen und steuern"), this, &aiAccessRow);
    m_aiAccess->setChecked(m_initialAiAccess);
    m_aiAccess->setToolTip(tr("Erlaubt einem verbundenen Assistenten wie Claude, das angezeigte Bild und "
                              "die Auswahl abzufragen, Dateien anzuzeigen und Bereiche zu markieren."));
    aiLayout->addWidget(aiAccessRow);
    {
        auto *row = new QHBoxLayout;
        auto *label = new QLabel(tr("Bei mehreren Fenstern sieht die KI:"), this);
        m_aiWindow = new QComboBox(this);
        m_aiWindow->addItem(tr("das zuletzt benutzte Fenster"), true);
        m_aiWindow->addItem(tr("das zuerst geöffnete Fenster"), false);
        m_aiWindow->setCurrentIndex(ViewerBridge::preferLastUsed() ? 0 : 1);
#ifdef Q_OS_WIN
        m_aiWindow->setCurrentIndex(1);
        m_aiWindow->setEnabled(false);
        m_aiWindow->setToolTip(tr("Unter Windows immer das zuerst geöffnete Fenster."));
#endif
        label->setBuddy(m_aiWindow);
        row->addWidget(label);
        row->addWidget(m_aiWindow);
        row->addStretch(1);
        aiLayout->addLayout(row);
        m_aiWindow->setEnabled(m_aiWindow->isEnabled() && m_aiAccess->isChecked());
        connect(m_aiAccess, &QCheckBox::toggled, m_aiWindow, [this](bool on) {
#ifndef Q_OS_WIN
            m_aiWindow->setEnabled(on);
#endif
        });
    }

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
                                 "(leer = die KI darf nichts speichern). Diese Ordner darf sie auch lesen – üblich ist "
                                 "ein eigener, leerer Unterordner eines Leseordners:"),
                              AiGuard::writeDirs(), tr("Ordner für Dateien der KI"));

    QWidget *overwriteRow = nullptr;
    m_aiOverwrite = wrappingCheckBox(tr("KI darf vorhandene Dateien ersetzen und Metadaten in Dateien ändern "
                                        "(nur in den Schreibordnern)"), this, &overwriteRow);
    m_aiOverwrite->setChecked(AiGuard::allowOverwrite());
    m_aiOverwrite->setToolTip(tr("Aus: Die KI legt nur neue Dateien an und verändert nichts Vorhandenes."));
    aiLayout->addWidget(overwriteRow);
    connect(m_aiOverwrite, &QCheckBox::toggled, this, &SettingsDialog::updateOverlapNote);

    m_overlapNote = new QLabel(this);
    m_overlapNote->setWordWrap(true);
    m_overlapNote->setStyleSheet(QStringLiteral("color: #b35900"));
    aiLayout->addWidget(m_overlapNote);

    auto *trustCaption = new QLabel(tr("Vertrauenswürdig: Haken bei den Leseordnern, die nur Ihre eigenen Dateien "
                                       "enthalten. Dort warnt KiekAn! nicht bei verdächtigem Text; Kennzeichnung, "
                                       "Protokoll und Rechte bleiben. Was die KI in Schreibordnern ablegt, gilt nur als "
                                       "vertrauenswürdig, wenn es aus einem angehakten Ordner stammt, seitdem unverändert "
                                       "ist und die KI keine eigenen Metadaten hineingeschrieben hat. Nicht für Downloads, "
                                       "Mail-Anhänge oder eingegangene Post:"), this);
    trustCaption->setWordWrap(true);
    aiLayout->addWidget(trustCaption);
    m_aiTrusted = new QListWidget(this);
    m_aiTrusted->setFixedHeight(m_aiTrusted->fontMetrics().lineSpacing() * 4 + 12);
    m_aiTrusted->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    aiLayout->addWidget(m_aiTrusted);
    connect(m_aiTrusted, &QListWidget::itemChanged, this, &SettingsDialog::updateOverlapNote);
    connect(m_aiReadDirs, &QPlainTextEdit::textChanged, this, &SettingsDialog::refreshTrustedList);
    refreshTrustedList();
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
        // Im Flatpak ist der Programmpfad nur in der Sandbox gültig.
        const QString flatpakId = qEnvironmentVariable("FLATPAK_ID");
        const QString command = flatpakId.isEmpty()
            ? QStringLiteral("\"%1\"").arg(QCoreApplication::applicationFilePath())
            : QStringLiteral("flatpak run %1").arg(flatpakId);
        QGuiApplication::clipboard()->setText(QStringLiteral("claude mcp add --scope user kiekan -- %1 --mcp").arg(command));
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
    tabs->addTab(createAssocPage(), tr("Dateizuordnung"));

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

// Auswahl vertrauenswürdiger Ordner: nur aus der aktuellen Leseliste.
void SettingsDialog::refreshTrustedList()
{
    if (!m_aiTrusted)
        return;
    QStringList checked = m_aiTrusted->count() ? checkedTrusted() : AiGuard::trustedDirs();
    const QSignalBlocker block(m_aiTrusted);
    m_aiTrusted->clear();
    for (const QString &dir : dirLines(m_aiReadDirs)) {
        auto *item = new QListWidgetItem(dir, m_aiTrusted);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(checked.contains(dir) ? Qt::Checked : Qt::Unchecked);
    }
    if (m_aiTrusted->count() == 0) {
        auto *item = new QListWidgetItem(tr("(erst Leseordner eintragen)"), m_aiTrusted);
        item->setFlags(Qt::NoItemFlags);
    }
    updateOverlapNote();
}

QStringList SettingsDialog::checkedTrusted() const
{
    QStringList out;
    for (int i = 0; i < m_aiTrusted->count(); ++i)
        if (m_aiTrusted->item(i)->checkState() == Qt::Checked)
            out << m_aiTrusted->item(i)->text();
    return out;
}

// Hinweise: wenn Originale im Schreibbereich liegen, wenn ersetzbare Dateien
// im Schreibordner liegen, und zu riskanten vertrauenswürdigen Ordnern.
void SettingsDialog::updateOverlapNote()
{
    if (m_trustNote && m_aiTrusted) {
        const QStringList concerns = AiGuard::trustConcerns(checkedTrusted(), dirLines(m_aiReadDirs), dirLines(m_aiWriteDirs));
        m_trustNote->setText(concerns.isEmpty() ? QString() : tr("Hinweis zu vertrauenswürdigen Ordnern:\n• ") + concerns.join(QStringLiteral("\n• ")));
        m_trustNote->setVisible(!concerns.isEmpty());
    }

    QStringList notes;
    const QStringList pairs = AiGuard::overlaps(dirLines(m_aiReadDirs), dirLines(m_aiWriteDirs));
    if (!pairs.isEmpty()) {
        QString text = tr("Leseordner liegen im Schreibbereich (%1). Die KI kann dann neue Dateien zwischen Ihre "
                          "Originale legen")
                           .arg(pairs.join(QStringLiteral("; ")));
        text += m_aiOverwrite->isChecked()
            ? tr(" und – weil Ersetzen erlaubt ist – auch Ihre Originale verändern. Eine eingeschleuste Anweisung "
                 "in einer Datei könnte das ausnutzen. Besser: einen eigenen, leeren Unterordner zum Schreiben wählen.")
            : tr(". Vorhandene Dateien bleiben unverändert, solange Ersetzen nicht erlaubt ist.");
        notes << text;
    } else if (m_aiOverwrite->isChecked()) {
        const QStringList filled = AiGuard::nonEmptyWriteDirs(dirLines(m_aiWriteDirs));
        if (!filled.isEmpty())
            notes << tr("Im Schreibordner liegen bereits Dateien, die die KI ersetzen dürfte: %1")
                         .arg(filled.join(QStringLiteral("; ")));
    }
    m_overlapNote->setText(notes.isEmpty() ? QString() : tr("Hinweis: ") + notes.join(QLatin1Char(' ')));
    m_overlapNote->setVisible(!notes.isEmpty());
}

bool SettingsDialog::apply()
{
    const QString lang = m_language->currentData().toString();
    const bool langChanged = lang != Settings::language();
    Settings::setLanguage(lang);
    Settings::setAutoRotate(m_autoRotate->isChecked());
    Settings::setJpegQuality(m_quality->value());
    Settings::setEscapeQuits(m_escape->isChecked());
    Settings::setLosslessJpeg(m_lossless->isChecked());
    Settings::setAiViewerAccess(m_aiAccess->isChecked());
    QSettings().setValue(QStringLiteral("ai/viewerLastUsed"), m_aiWindow->currentData().toBool());
    AiGuard::setReadDirs(dirLines(m_aiReadDirs));
    AiGuard::setWriteDirs(dirLines(m_aiWriteDirs));
    AiGuard::setAllowOverwrite(m_aiOverwrite->isChecked());
    AiGuard::setTrustedDirs(checkedTrusted());
    QSettings().setValue(QStringLiteral("ocr/jobs"), m_ocrJobs->value());
    return langChanged;
}

// --- Dateizuordnung ----------------------------------------------------------

QWidget *SettingsDialog::createAssocPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    if (!FileAssoc::supported()) {
#ifdef Q_OS_MACOS
        // macOS: Zuordnung im Finder, je Dateityp über „Alle ändern …“.
        auto *text = new QLabel(tr("So öffnen Sie Bilder mit KiekAn!:\n\n"
                                   "1. Im Finder ein Bild auswählen und „Informationen“ öffnen (⌘I).\n"
                                   "2. Unter „Öffnen mit“ KiekAn! auswählen.\n"
                                   "3. „Alle ändern …“ klicken und bestätigen.\n\n"
                                   "Das gilt jeweils für einen Dateityp (z. B. .jpg). PDFs und Videos "
                                   "bleiben bei ihren bisherigen Programmen, solange Sie dort nichts ändern."), page);
        text->setWordWrap(true);
        layout->addWidget(text);
#else
        // Windows erlaubt Programmen nicht, sich selbst als Standard einzutragen.
        auto *text = new QLabel(tr("So öffnen Sie Bilder mit KiekAn!:\n\n"
                                   "1. Rechtsklick auf ein Bild → „Öffnen mit“ → „Andere App auswählen“.\n"
                                   "2. „Weitere Apps“ → „Auf diesem PC nach einer anderen App suchen“ und kiekan.exe wählen.\n"
                                   "3. „Immer diese App verwenden“ ankreuzen.\n\n"
                                   "Das gilt jeweils für einen Dateityp (z. B. .jpg). PDFs und Videos "
                                   "bleiben bei ihren bisherigen Programmen, solange Sie dort nichts ändern."), page);
        text->setWordWrap(true);
        layout->addWidget(text);
        auto *open = new QPushButton(tr("Standard-Apps öffnen …"), page);
        connect(open, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps")));
        });
        auto *row = new QHBoxLayout;
        row->addWidget(open);
        row->addStretch(1);
        layout->addLayout(row);
#endif
        layout->addStretch(1);
        return page;
    }

    auto *intro = new QLabel(tr("Diese Dateitypen per Doppelklick mit KiekAn! öffnen:"), page);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // Vorauswahl: der jetzige Stand; ist KiekAn! noch nirgends eingetragen,
    // die Vorschläge (Bilder und RAW, nicht PDF und Videos).
    const QList<FileAssoc::Group> groups = FileAssoc::groups();
    bool anySet = false;
    for (const FileAssoc::Group &g : groups)
        anySet = anySet || FileAssoc::isDefault(g);
    auto *grid = new QFormLayout;
    for (const FileAssoc::Group &g : groups) {
        QWidget *row = nullptr;
        QCheckBox *box = wrappingCheckBox(g.label, page, &row);
        box->setChecked(anySet ? FileAssoc::isDefault(g) : g.suggested);
        if (anySet && !FileAssoc::isDefault(g) && partlyDefault(g)) {
            // In den erweiterten Einstellungen nur teilweise gewählt
            box->setTristate(true);
            box->setCheckState(Qt::PartiallyChecked);
        }
        auto *current = new QLabel(page);
        current->setStyleSheet(QStringLiteral("color: gray"));
        current->setIndent(box->sizeHint().width() + 6);
        auto *cell = new QWidget(page);
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 4);
        cellLayout->setSpacing(0);
        cellLayout->addWidget(row);
        cellLayout->addWidget(current);
        grid->addRow(cell);
        m_assocBoxes << qMakePair(g.key, box);
        m_assocCurrent << qMakePair(g.key, current);
    }
    layout->addLayout(grid);

    auto *apply = new QPushButton(tr("Zuordnung übernehmen"), page);
    connect(apply, &QPushButton::clicked, this, &SettingsDialog::applyAssoc);
    auto *details = new QPushButton(tr("Erweiterte Einstellungen …"), page);
    details->setToolTip(tr("Jeden Dateityp (jede Dateiendung) einzeln festlegen"));
    connect(details, &QPushButton::clicked, this, &SettingsDialog::openAssocDetails);
    auto *row = new QHBoxLayout;
    row->addWidget(apply);
    row->addWidget(details);
    row->addStretch(1);
    layout->addLayout(row);

    m_assocResult = new QLabel(page);
    m_assocResult->setWordWrap(true);
    layout->addWidget(m_assocResult);

    auto *note = new QLabel(tr("Nicht angekreuzte Typen öffnen wieder mit dem Programm, das das System "
                               "vorsieht. Geändert wird nur Ihre eigene Zuordnung (%1), nicht die "
                               "anderer Benutzer.").arg(FileAssoc::userListFiles().join(QStringLiteral(", "))), page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray"));
    layout->addWidget(note);
    layout->addStretch(1);

    refreshAssocStatus();
    return page;
}

void SettingsDialog::refreshAssocStatus()
{
    const QList<FileAssoc::Group> groups = FileAssoc::groups();
    for (const auto &[key, label] : m_assocCurrent) {
        for (const FileAssoc::Group &g : groups) {
            if (g.key != key)
                continue;
            const QString app = FileAssoc::currentDefault(g.mimeTypes.first());
            label->setText(app.isEmpty() ? tr("derzeit: Vorgabe des Systems")
                                         : tr("derzeit: %1").arg(app));
        }
    }
}

// Erweiterte Einstellungen: jeder MIME-Typ (mit seinen Dateiendungen) einzeln.
void SettingsDialog::openAssocDetails()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Dateizuordnung – erweiterte Einstellungen"));
    dlg.resize(760, 560);
    auto *layout = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(tr("Angekreuzte Dateitypen öffnen per Doppelklick mit KiekAn!. "
                                "Nicht angekreuzte öffnen mit dem Programm, das das System vorsieht."), &dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *filter = new QLineEdit(&dlg);
    filter->setPlaceholderText(tr("Suchen, z. B. jpg oder heic"));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);

    auto *tree = new QTreeWidget(&dlg);
    tree->setHeaderLabels({tr("Dateityp"), tr("Öffnet derzeit")});
    tree->setRootIsDecorated(true);
    layout->addWidget(tree, 1);

    QHash<QTreeWidgetItem *, QString> mimeOf;
    auto fill = [&] {
        tree->clear();
        mimeOf.clear();
        for (const FileAssoc::Group &g : FileAssoc::groups()) {
            auto *top = new QTreeWidgetItem(tree, {g.label});
            top->setFlags(top->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            for (const QString &m : g.mimeTypes) {
                const QString app = FileAssoc::currentDefault(m);
                auto *item = new QTreeWidgetItem(top, {FileAssoc::typeLabel(m),
                                                       app.isEmpty() ? tr("Vorgabe des Systems") : app});
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(0, FileAssoc::isDefault(m) ? Qt::Checked : Qt::Unchecked);
                item->setToolTip(0, m);
                mimeOf.insert(item, m);
            }
        }
        tree->expandAll();
        tree->resizeColumnToContents(0);
    };
    fill();

    connect(filter, &QLineEdit::textChanged, &dlg, [&](const QString &text) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *top = tree->topLevelItem(i);
            bool any = false;
            for (int c = 0; c < top->childCount(); ++c) {
                QTreeWidgetItem *item = top->child(c);
                const bool show = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive)
                                  || mimeOf.value(item).contains(text, Qt::CaseInsensitive);
                item->setHidden(!show);
                any = any || show;
            }
            top->setHidden(!any);
        }
    });

    auto *result = new QLabel(&dlg);
    result->setWordWrap(true);
    layout->addWidget(result);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    auto *applyButton = buttons->addButton(tr("Übernehmen"), QDialogButtonBox::ApplyRole);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(applyButton, &QPushButton::clicked, &dlg, [&] {
        QStringList on, off;
        for (auto it = mimeOf.constBegin(); it != mimeOf.constEnd(); ++it) {
            const bool want = it.key()->checkState(0) == Qt::Checked;
            if (want != FileAssoc::isDefault(it.value()))
                (want ? on : off) << it.value();
        }
        const QString error = (on.isEmpty() && off.isEmpty()) ? QString() : FileAssoc::applyTypes(on, off);
        result->setStyleSheet(error.isEmpty() ? QString() : QStringLiteral("color: #b00020"));
        result->setText(error.isEmpty() ? tr("Übernommen: %n Dateityp(en) geändert.", nullptr, int(on.size() + off.size()))
                                        : tr("Konnte die Zuordnung nicht speichern: %1").arg(error));
        const QString search = filter->text();
        fill();
        filter->setText(QString());
        filter->setText(search);
    });
    layout->addWidget(buttons);
    dlg.exec();

    // Hauptseite an den neuen Stand anpassen
    for (const auto &[key, box] : m_assocBoxes) {
        for (const FileAssoc::Group &g : FileAssoc::groups()) {
            if (g.key != key)
                continue;
            const bool all = FileAssoc::isDefault(g);
            const bool partial = !all && partlyDefault(g);
            box->setTristate(partial);
            box->setCheckState(all ? Qt::Checked : partial ? Qt::PartiallyChecked : Qt::Unchecked);
        }
    }
    refreshAssocStatus();
}

void SettingsDialog::applyAssoc()
{
    QStringList on, off;
    for (const auto &[key, box] : m_assocBoxes) {
        if (box->checkState() == Qt::PartiallyChecked)
            continue;   // Einzelauswahl aus den erweiterten Einstellungen beibehalten
        (box->isChecked() ? on : off) << key;
    }
    const QString error = FileAssoc::apply(on, off);
    m_assocResult->setStyleSheet(error.isEmpty() ? QString() : QStringLiteral("color: #b00020"));
    m_assocResult->setText(error.isEmpty() ? tr("Übernommen. Neu geöffnete Dateien starten jetzt im gewählten Programm.")
                                           : tr("Konnte die Zuordnung nicht speichern: %1").arg(error));
    refreshAssocStatus();
}

bool SettingsDialog::aiAccessChanged() const
{
    return m_aiAccess->isChecked() != m_initialAiAccess;
}

bool SettingsDialog::autoRotateChanged() const
{
    return m_autoRotate->isChecked() != m_initialAutoRotate;
}

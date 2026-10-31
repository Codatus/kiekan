// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BatchDialog.h"

#include "BatchConvert.h"
#include "Media.h"
#include "NaturalSort.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QSet>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString problemText(const QString &code)
{
    if (code == QLatin1String("empty"))
        return BatchDialog::tr("leerer Name");
    if (code == QLatin1String("invalid"))
        return BatchDialog::tr("unzulässige Zeichen");
    if (code == QLatin1String("exists"))
        return BatchDialog::tr("Name gehört schon einer anderen Datei");
    if (code == QLatin1String("duplicate"))
        return BatchDialog::tr("Name kommt mehrfach vor");
    return code;
}

} // namespace

BatchDialog::BatchDialog(const QStringList &files, const QString &current, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Stapelverarbeitung"));
    QSettings settings;

    // --- Dateiliste ---
    auto *filesBox = new QGroupBox(tr("Dateien"), this);
    auto *filesLayout = new QVBoxLayout(filesBox);
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setAccessibleName(tr("Dateien"));
    m_list->setMinimumWidth(260);
    filesLayout->addWidget(m_list, 1);
    m_count = new QLabel(this);
    filesLayout->addWidget(m_count);
    auto *addButton = new QPushButton(tr("Dateien hinzufügen …"), this);
    auto *addDirButton = new QPushButton(tr("Ordner hinzufügen …"), this);
    auto *removeButton = new QPushButton(tr("Auswahl entfernen"), this);
    auto *clearButton = new QPushButton(tr("Liste leeren"), this);
    auto *fileButtons = new QGridLayout;
    fileButtons->addWidget(addButton, 0, 0);
    fileButtons->addWidget(addDirButton, 0, 1);
    fileButtons->addWidget(removeButton, 1, 0);
    fileButtons->addWidget(clearButton, 1, 1);
    filesLayout->addLayout(fileButtons);

    const QString startDir = current.isEmpty() ? QDir::homePath() : QFileInfo(current).absolutePath();
    connect(addButton, &QPushButton::clicked, this, [this, startDir] {
        const QStringList chosen = QFileDialog::getOpenFileNames(
            this, tr("Bilder hinzufügen"), startDir,
            tr("Bilder (%1)").arg(Media::imageNameFilters().join(QLatin1Char(' '))));
        addFiles(chosen);
    });
    connect(addDirButton, &QPushButton::clicked, this, [this, startDir] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Ordner hinzufügen"), startDir);
        if (dir.isEmpty())
            return;
        QStringList found;
        QDirIterator it(dir, Media::imageNameFilters(), QDir::Files | QDir::Readable);
        while (it.hasNext())
            found << it.next();
        std::sort(found.begin(), found.end(), [](const QString &a, const QString &b) { return naturalCompare(a, b) < 0; });
        addFiles(found);
    });
    connect(removeButton, &QPushButton::clicked, this, [this] {
        qDeleteAll(m_list->selectedItems());
        updateConvertState();
        updatePreview();
    });
    connect(clearButton, &QPushButton::clicked, this, [this] {
        m_list->clear();
        updateConvertState();
        updatePreview();
    });

    // --- Reiter ---
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(createConvertPage(), tr("Umwandeln"));
    m_tabs->addTab(createRenamePage(), tr("Umbenennen"));
    m_tabs->setCurrentIndex(settings.value(QStringLiteral("batch/tab"), 0).toInt());

    auto *top = new QHBoxLayout;
    top->addWidget(filesBox, 2);
    top->addWidget(m_tabs, 3);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    m_run = new QPushButton(this);
    m_run->setDefault(true);
    m_close = new QPushButton(tr("Schließen"), this);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_progress, 1);
    bottom->addStretch();
    bottom->addWidget(m_run);
    bottom->addWidget(m_close);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top, 1);
    layout->addLayout(bottom);

    connect(m_close, &QPushButton::clicked, this, &BatchDialog::reject);
    connect(m_run, &QPushButton::clicked, this, [this] {
        if (m_worker) {
            m_cancel = true;
            m_run->setEnabled(false);
        } else if (m_tabs->currentIndex() == 0) {
            startConvert();
        } else {
            startRename();
        }
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        QSettings().setValue(QStringLiteral("batch/tab"), index);
        if (!m_worker)
            m_progress->setVisible(false);
        updateConvertState();
        updatePreview();
    });

    if (m_outDir->text().isEmpty())
        m_outDir->setText(QDir::toNativeSeparators(QDir(startDir).filePath(tr("konvertiert"))));
    addFiles(files);
    resize(980, 640);
}

BatchDialog::~BatchDialog()
{
    if (m_worker) {
        m_cancel = true;
        m_worker->wait();
    }
}

void BatchDialog::reject()
{
    if (m_worker) {
        m_cancel = true;
        m_worker->wait();
    }
    QDialog::reject();
}

QWidget *BatchDialog::createConvertPage()
{
    QSettings settings;
    auto *page = new QWidget(this);
    auto *form = new QFormLayout;

    m_format = new QComboBox(page);
    const QHash<QString, QString> names{
        {QStringLiteral("jpg"), tr("JPEG (.jpg)")}, {QStringLiteral("png"), tr("PNG (.png, verlustfrei)")},
        {QStringLiteral("webp"), tr("WebP (.webp)")}, {QStringLiteral("tif"), tr("TIFF (.tif, verlustfrei)")},
        {QStringLiteral("bmp"), tr("BMP (.bmp)")}};
    for (const QString &f : BatchConvert::writableFormats())
        m_format->addItem(names.value(f, f), f);
    m_format->setCurrentIndex(std::max(0, m_format->findData(settings.value(QStringLiteral("batch/format"), QStringLiteral("jpg")))));
    form->addRow(tr("Zielformat:"), m_format);

    m_quality = new QSpinBox(page);
    m_quality->setRange(1, 100);
    m_quality->setValue(settings.value(QStringLiteral("batch/quality"), settings.value(QStringLiteral("jpegQuality"), 90)).toInt());
    form->addRow(tr("Qualität (JPEG, WebP):"), m_quality);

    m_resize = new QCheckBox(tr("Verkleinern auf höchstens"), page);
    m_resize->setChecked(settings.value(QStringLiteral("batch/resize"), false).toBool());
    m_maxSize = new QSpinBox(page);
    m_maxSize->setRange(16, 100000);
    m_maxSize->setSuffix(tr(" Pixel (längste Seite)"));
    m_maxSize->setValue(settings.value(QStringLiteral("batch/maxSize"), 1920).toInt());
    m_maxSize->setAccessibleName(tr("Längste Seite in Pixeln"));
    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(m_resize);
    sizeRow->addWidget(m_maxSize, 1);
    form->addRow(tr("Größe:"), sizeRow);

    m_autoLevels = new QCheckBox(tr("Automatische Farbkorrektur"), page);
    m_autoLevels->setChecked(settings.value(QStringLiteral("batch/autoLevels"), false).toBool());
    m_grayscale = new QCheckBox(tr("Graustufen"), page);
    m_grayscale->setChecked(settings.value(QStringLiteral("batch/grayscale"), false).toBool());
    auto *ops = new QVBoxLayout;
    ops->addWidget(m_autoLevels);
    ops->addWidget(m_grayscale);
    form->addRow(tr("Bearbeiten:"), ops);

    m_keepMeta = new QCheckBox(tr("Metadaten (EXIF, IPTC, XMP) übernehmen"), page);
    m_keepMeta->setChecked(settings.value(QStringLiteral("batch/keepMeta"), true).toBool());
    m_overwrite = new QCheckBox(tr("Vorhandene Dateien ersetzen"), page);
    m_overwrite->setChecked(false);   // bewusst jedes Mal neu entscheiden
    m_useRenamePattern = new QCheckBox(tr("Dateinamen nach dem Muster im Reiter „Umbenennen“"), page);
    m_useRenamePattern->setChecked(settings.value(QStringLiteral("batch/usePattern"), false).toBool());
    auto *opts = new QVBoxLayout;
    opts->addWidget(m_keepMeta);
    opts->addWidget(m_overwrite);
    opts->addWidget(m_useRenamePattern);
    form->addRow(tr("Optionen:"), opts);

    m_outDir = new QLineEdit(QDir::toNativeSeparators(settings.value(QStringLiteral("batch/outDir")).toString()), page);
    m_outDir->setAccessibleName(tr("Zielordner"));
    auto *browse = new QPushButton(tr("…"), page);
    browse->setAccessibleName(tr("Zielordner wählen"));
    browse->setFixedWidth(browse->fontMetrics().horizontalAdvance(QStringLiteral("…")) + 24);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Zielordner"), m_outDir->text());
        if (!dir.isEmpty())
            m_outDir->setText(QDir::toNativeSeparators(dir));
    });
    auto *dirRow = new QHBoxLayout;
    dirRow->addWidget(m_outDir, 1);
    dirRow->addWidget(browse);
    form->addRow(tr("Zielordner:"), dirRow);

    m_log = new QPlainTextEdit(page);
    m_log->setReadOnly(true);
    m_log->setAccessibleName(tr("Protokoll"));
    m_log->setPlaceholderText(tr("Die Originale bleiben unverändert; die neuen Dateien landen im Zielordner."));

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(m_log, 1);

    connect(m_format, &QComboBox::currentIndexChanged, this, &BatchDialog::updateConvertState);
    connect(m_resize, &QCheckBox::toggled, this, &BatchDialog::updateConvertState);
    return page;
}

QWidget *BatchDialog::createRenamePage()
{
    QSettings settings;
    auto *page = new QWidget(this);
    auto *form = new QFormLayout;

    m_pattern = new QLineEdit(settings.value(QStringLiteral("batch/pattern"), QStringLiteral("$D_###")).toString(), page);
    form->addRow(tr("Muster:"), m_pattern);
    auto *help = new QLabel(tr("### laufende Nummer (Stellen = Rauten) · $N bisheriger Name · $D Aufnahmedatum · "
                               "$T Uhrzeit · $F Ordnername. Die Endung bleibt."), page);
    help->setWordWrap(true);
    help->setStyleSheet(QStringLiteral("color: gray"));
    form->addRow(QString(), help);

    m_start = new QSpinBox(page);
    m_start->setRange(0, 999999);
    m_start->setValue(settings.value(QStringLiteral("batch/start"), 1).toInt());
    m_step = new QSpinBox(page);
    m_step->setRange(1, 1000);
    m_step->setValue(settings.value(QStringLiteral("batch/step"), 1).toInt());
    auto *numbers = new QHBoxLayout;
    numbers->addWidget(new QLabel(tr("Beginnen bei"), page));
    numbers->addWidget(m_start);
    numbers->addWidget(new QLabel(tr("Schrittweite"), page));
    numbers->addWidget(m_step);
    numbers->addStretch();
    form->addRow(tr("Nummer:"), numbers);

    m_sort = new QComboBox(page);
    m_sort->addItem(tr("nach Name"), int(BatchRename::Sort::Name));
    m_sort->addItem(tr("nach Aufnahmedatum"), int(BatchRename::Sort::DateTaken));
    m_sort->addItem(tr("nach Änderungsdatum"), int(BatchRename::Sort::Modified));
    m_sort->setCurrentIndex(settings.value(QStringLiteral("batch/sort"), 0).toInt());
    form->addRow(tr("Reihenfolge:"), m_sort);

    m_lowerSuffix = new QCheckBox(tr("Endung kleinschreiben (.JPG → .jpg)"), page);
    m_lowerSuffix->setChecked(settings.value(QStringLiteral("batch/lowerSuffix"), true).toBool());
    form->addRow(QString(), m_lowerSuffix);

    m_preview = new QTableWidget(0, 2, page);
    m_preview->setHorizontalHeaderLabels({tr("Bisher"), tr("Neu")});
    m_preview->setAccessibleName(tr("Vorschau der neuen Namen"));
    m_preview->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_preview->verticalHeader()->setVisible(false);
    m_preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_preview->setSelectionMode(QAbstractItemView::NoSelection);
    m_previewNote = new QLabel(page);
    m_previewNote->setWordWrap(true);
    m_undo = new QPushButton(tr("Letzte Umbenennung rückgängig"), page);
    m_undo->setEnabled(false);
    connect(m_undo, &QPushButton::clicked, this, &BatchDialog::undoRename);
    auto *undoRow = new QHBoxLayout;
    undoRow->addWidget(m_previewNote, 1);
    undoRow->addWidget(m_undo);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(m_preview, 1);
    layout->addLayout(undoRow);

    // Vorschau verzögert aktualisieren (Aufnahmedaten lesen kostet Zeit)
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, &BatchDialog::updatePreview);
    auto later = [timer] { timer->start(); };
    connect(m_pattern, &QLineEdit::textChanged, this, later);
    connect(m_start, &QSpinBox::valueChanged, this, later);
    connect(m_step, &QSpinBox::valueChanged, this, later);
    connect(m_sort, &QComboBox::currentIndexChanged, this, later);
    connect(m_lowerSuffix, &QCheckBox::toggled, this, later);
    return page;
}

void BatchDialog::addFiles(const QStringList &files)
{
    QSet<QString> present;
    for (int i = 0; i < m_list->count(); ++i)
        present.insert(m_list->item(i)->data(Qt::UserRole).toString());
    for (const QString &f : files) {
        const QString path = QFileInfo(f).absoluteFilePath();
        if (present.contains(path) || Media::isVideo(path))
            continue;
        present.insert(path);
        auto *item = new QListWidgetItem(QFileInfo(path).fileName(), m_list);
        item->setData(Qt::UserRole, path);
        item->setToolTip(QDir::toNativeSeparators(path));
    }
    updateConvertState();
    updatePreview();
}

QStringList BatchDialog::files() const
{
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i)
        out << m_list->item(i)->data(Qt::UserRole).toString();
    return out;
}

void BatchDialog::updateConvertState()
{
    m_count->setText(tr("%n Datei(en)", nullptr, m_list->count()));
    const QString f = m_format->currentData().toString();
    m_quality->setEnabled(f == QLatin1String("jpg") || f == QLatin1String("webp"));
    m_maxSize->setEnabled(m_resize->isChecked());
    if (!m_worker)
        m_run->setText(m_tabs->currentIndex() == 0 ? tr("Umwandeln") : tr("Umbenennen"));
    m_run->setEnabled(m_list->count() > 0 || m_worker);
}

static BatchRename::Options renameOptions(const QLineEdit *pattern, const QSpinBox *start, const QSpinBox *step,
                                   const QComboBox *sort, const QCheckBox *lower)
{
    BatchRename::Options o;
    o.pattern = pattern->text();
    o.start = start->value();
    o.step = step->value();
    o.sort = BatchRename::Sort(sort->currentData().toInt());
    o.lowercaseSuffix = lower->isChecked();
    return o;
}

void BatchDialog::updatePreview()
{
    if (m_tabs->currentIndex() != 1)
        return;
    const QList<BatchRename::Entry> entries =
        BatchRename::preview(files(), renameOptions(m_pattern, m_start, m_step, m_sort, m_lowerSuffix));
    m_preview->setRowCount(int(entries.size()));
    int problems = 0, changes = 0;
    for (int row = 0; row < entries.size(); ++row) {
        const BatchRename::Entry &e = entries.at(row);
        auto *from = new QTableWidgetItem(QFileInfo(e.source).fileName());
        auto *to = new QTableWidgetItem(e.problem.isEmpty() ? QFileInfo(e.target).fileName()
                                                            : tr("⚠ %1").arg(problemText(e.problem)));
        if (!e.problem.isEmpty()) {
            to->setForeground(QColor(0xb0, 0x00, 0x20));
            ++problems;
        } else if (e.unchanged()) {
            to->setForeground(Qt::gray);
        } else {
            ++changes;
        }
        from->setToolTip(QDir::toNativeSeparators(e.source));
        m_preview->setItem(row, 0, from);
        m_preview->setItem(row, 1, to);
    }
    QString note = tr("%n Datei(en) werden umbenannt.", nullptr, changes);
    if (problems)
        note += QLatin1Char(' ') + tr("%n Datei(en) mit Problem werden übersprungen.", nullptr, problems);
    m_previewNote->setText(note);
}

void BatchDialog::startConvert()
{
    BatchConvert::Options options;
    options.format = m_format->currentData().toString();
    options.outputDir = QDir::fromNativeSeparators(m_outDir->text().trimmed());
    options.quality = m_quality->value();
    options.maxSize = m_resize->isChecked() ? m_maxSize->value() : 0;
    options.overwrite = m_overwrite->isChecked();
    options.keepMetadata = m_keepMeta->isChecked();
    if (m_autoLevels->isChecked())
        options.operations.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("auto_levels")}});
    if (m_grayscale->isChecked())
        options.operations.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("grayscale")}});
    if (options.outputDir.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Bitte einen Zielordner angeben."));
        return;
    }

    QSettings s;
    s.setValue(QStringLiteral("batch/format"), options.format);
    s.setValue(QStringLiteral("batch/quality"), options.quality);
    s.setValue(QStringLiteral("batch/resize"), m_resize->isChecked());
    s.setValue(QStringLiteral("batch/maxSize"), m_maxSize->value());
    s.setValue(QStringLiteral("batch/autoLevels"), m_autoLevels->isChecked());
    s.setValue(QStringLiteral("batch/grayscale"), m_grayscale->isChecked());
    s.setValue(QStringLiteral("batch/keepMeta"), m_keepMeta->isChecked());
    s.setValue(QStringLiteral("batch/usePattern"), m_useRenamePattern->isChecked());
    s.setValue(QStringLiteral("batch/outDir"), options.outputDir);

    // Optional neue Namen nach dem Umbenennen-Muster (ohne Endung)
    QStringList inputs = files();
    QStringList names;
    if (m_useRenamePattern->isChecked()) {
        const auto entries = BatchRename::preview(inputs, renameOptions(m_pattern, m_start, m_step, m_sort, m_lowerSuffix));
        inputs.clear();
        for (const BatchRename::Entry &e : entries) {
            inputs << e.source;
            names << (e.problem.isEmpty() ? QFileInfo(e.target).completeBaseName() : QString());
        }
    }

    const QList<BatchConvert::Item> items = BatchConvert::plan(inputs, options, names);
    m_log->clear();
    m_progress->setRange(0, int(items.size()));
    m_progress->setValue(0);
    m_progress->setVisible(true);
    m_cancel = false;
    m_run->setText(tr("Abbrechen"));
    m_tabs->setEnabled(false);

    m_worker = QThread::create([this, items, options] {
        int done = 0, skipped = 0, failed = 0;
        for (int i = 0; i < items.size() && !m_cancel; ++i) {
            const BatchConvert::Item &item = items.at(i);
            const BatchConvert::Outcome o = BatchConvert::convert(item, options);
            QString line;
            switch (o.status) {
            case BatchConvert::Outcome::Converted:
                ++done;
                line = tr("✓ %1 → %2 (%3 × %4)").arg(QFileInfo(item.input).fileName(), QFileInfo(item.output).fileName())
                           .arg(o.size.width()).arg(o.size.height());
                break;
            case BatchConvert::Outcome::Skipped:
                ++skipped;
                line = tr("– %1: übersprungen (%2)").arg(QFileInfo(item.output).fileName(),
                    o.message == QLatin1String("exists") ? tr("gibt es schon") : tr("wäre das Original"));
                break;
            case BatchConvert::Outcome::Failed:
                ++failed;
                line = tr("✗ %1: %2").arg(QFileInfo(item.input).fileName(), o.message);
                break;
            }
            QMetaObject::invokeMethod(this, [this, line, i] {
                m_log->appendPlainText(line);
                m_progress->setValue(i + 1);
            }, Qt::QueuedConnection);
        }
        const QString summary = tr("Fertig: %1 umgewandelt, %2 übersprungen, %3 fehlgeschlagen%4.")
                                    .arg(done).arg(skipped).arg(failed)
                                    .arg(m_cancel ? tr(" (abgebrochen)") : QString());
        QMetaObject::invokeMethod(this, [this, summary] { m_log->appendPlainText(summary); }, Qt::QueuedConnection);
    });
    connect(m_worker, &QThread::finished, this, &BatchDialog::finishConvert);
    m_worker->start();
}

void BatchDialog::finishConvert()
{
    m_worker->deleteLater();
    m_worker = nullptr;
    m_tabs->setEnabled(true);
    m_run->setEnabled(true);
    updateConvertState();
}

void BatchDialog::startRename()
{
    const BatchRename::Options options = renameOptions(m_pattern, m_start, m_step, m_sort, m_lowerSuffix);
    QSettings s;
    s.setValue(QStringLiteral("batch/pattern"), options.pattern);
    s.setValue(QStringLiteral("batch/start"), options.start);
    s.setValue(QStringLiteral("batch/step"), options.step);
    s.setValue(QStringLiteral("batch/sort"), m_sort->currentIndex());
    s.setValue(QStringLiteral("batch/lowerSuffix"), options.lowercaseSuffix);

    const QList<BatchRename::Entry> entries = BatchRename::preview(files(), options);
    int changes = 0;
    for (const BatchRename::Entry &e : entries)
        changes += e.problem.isEmpty() && !e.unchanged();
    if (changes == 0) {
        QMessageBox::information(this, windowTitle(), tr("Nichts umzubenennen."));
        return;
    }
    if (QMessageBox::question(this, windowTitle(), tr("%n Datei(en) umbenennen?", nullptr, changes))
        != QMessageBox::Yes)
        return;

    m_lastRename = BatchRename::run(entries);
    // Liste auf die neuen Namen umstellen
    QHash<QString, QString> map;
    for (const auto &[from, to] : m_lastRename.renamed)
        map.insert(from, to);
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        const QString to = map.value(item->data(Qt::UserRole).toString());
        if (!to.isEmpty()) {
            item->setData(Qt::UserRole, to);
            item->setText(QFileInfo(to).fileName());
            item->setToolTip(QDir::toNativeSeparators(to));
        }
    }
    m_undo->setEnabled(!m_lastRename.renamed.isEmpty());
    Q_EMIT filesRenamed(m_lastRename.renamed);
    updatePreview();
    if (!m_lastRename.errors.isEmpty())
        QMessageBox::warning(this, windowTitle(), tr("Nicht umbenannt:\n%1").arg(m_lastRename.errors.join(QLatin1Char('\n'))));
    else
        m_previewNote->setText(tr("%n Datei(en) umbenannt.", nullptr, int(m_lastRename.renamed.size())));
}

void BatchDialog::undoRename()
{
    const BatchRename::Result back = BatchRename::undo(m_lastRename);
    QHash<QString, QString> map;
    for (const auto &[from, to] : back.renamed)
        map.insert(from, to);
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        const QString to = map.value(item->data(Qt::UserRole).toString());
        if (!to.isEmpty()) {
            item->setData(Qt::UserRole, to);
            item->setText(QFileInfo(to).fileName());
            item->setToolTip(QDir::toNativeSeparators(to));
        }
    }
    Q_EMIT filesRenamed(back.renamed);
    m_lastRename = {};
    m_undo->setEnabled(false);
    updatePreview();
    if (!back.errors.isEmpty())
        QMessageBox::warning(this, windowTitle(), tr("Nicht zurückbenannt:\n%1").arg(back.errors.join(QLatin1Char('\n'))));
    else
        m_previewNote->setText(tr("Umbenennung zurückgenommen."));
}

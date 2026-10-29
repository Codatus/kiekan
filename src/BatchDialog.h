// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "BatchRename.h"

#include <QDialog>
#include <atomic>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QThread;

// „Stapelverarbeitung“: viele Bilder umwandeln (Format, Größe, Korrekturen)
// oder nach einem Muster umbenennen – mit Vorschau der neuen Namen.
class BatchDialog : public QDialog
{
    Q_OBJECT

public:
    // `files`: Vorauswahl (z. B. die Bilder des aktuellen Ordners)
    explicit BatchDialog(const QStringList &files, const QString &current, QWidget *parent = nullptr);
    ~BatchDialog() override;

Q_SIGNALS:
    // Dateien wurden umbenannt (alt → neu), damit das Hauptfenster folgen kann
    void filesRenamed(const QList<QPair<QString, QString>> &renamed);

protected:
    void reject() override;

private:
    QWidget *createConvertPage();
    QWidget *createRenamePage();
    void addFiles(const QStringList &files);
    QStringList files() const;
    void updateConvertState();
    void updatePreview();
    void startConvert();
    void startRename();
    void undoRename();
    void finishConvert();

    QListWidget *m_list;
    QLabel *m_count;
    QTabWidget *m_tabs;

    // Konvertieren
    QComboBox *m_format;
    QSpinBox *m_quality;
    QCheckBox *m_resize;
    QSpinBox *m_maxSize;
    QCheckBox *m_autoLevels;
    QCheckBox *m_grayscale;
    QCheckBox *m_keepMeta;
    QCheckBox *m_overwrite;
    QCheckBox *m_useRenamePattern;
    QLineEdit *m_outDir;
    QPlainTextEdit *m_log;

    // Umbenennen
    QLineEdit *m_pattern;
    QSpinBox *m_start;
    QSpinBox *m_step;
    QComboBox *m_sort;
    QCheckBox *m_lowerSuffix;
    QTableWidget *m_preview;
    QLabel *m_previewNote;
    QPushButton *m_undo;
    BatchRename::Result m_lastRename;

    QProgressBar *m_progress;
    QPushButton *m_run;
    QPushButton *m_close;
    QThread *m_worker = nullptr;
    std::atomic_bool m_cancel{false};
};

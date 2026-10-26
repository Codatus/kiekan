// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QThread;

// „PDF teilen“: nach Größe (z. B. für Upload-Grenzen) oder Seitenzahl,
// mit Teilungsprotokoll.
class PdfSplitDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PdfSplitDialog(const QString &pdfPath, QWidget *parent = nullptr);
    ~PdfSplitDialog() override;

private:
    void start();

    QString m_path;
    QRadioButton *m_bySize;
    QRadioButton *m_byPages;
    QDoubleSpinBox *m_megabytes;
    QSpinBox *m_pages;
    QCheckBox *m_bookmarks;
    QLineEdit *m_outDir;
    QPushButton *m_startButton;
    QPushButton *m_openFolder;
    QPushButton *m_openProtocol;
    QLabel *m_status;
    QPlainTextEdit *m_result;
    QThread *m_thread = nullptr;
    QString m_manifestMarkdown;
};

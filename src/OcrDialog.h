// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QImage>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QThread;

// Zeigt den erkannten Text eines Bildes (oder der markierten Fläche).
// Die Erkennung läuft im Hintergrund; die Sprache wird automatisch gewählt
// und lässt sich bei Bedarf festlegen.
class OcrDialog : public QDialog
{
    Q_OBJECT

public:
    // `pdfText`: vorhandene Textebene einer PDF-Seite (wird dann direkt gezeigt).
    OcrDialog(const QImage &image, const QString &pdfText, const QString &suggestedName, QWidget *parent = nullptr);
    ~OcrDialog() override;

private:
    void startRecognition();
    void saveText();

    QImage m_image;
    QString m_suggestedName;
    QComboBox *m_language;
    QPushButton *m_rerun;
    QLabel *m_info;
    QPlainTextEdit *m_text;
    QThread *m_thread = nullptr;
};

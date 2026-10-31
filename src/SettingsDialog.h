// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLayout;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;

// Zentrale Stelle für alle Einstellungen (QSettings-Schlüssel + Standardwerte).
namespace Settings {
QString language();          // "" = Systemsprache, sonst "de" / "en"
void setLanguage(const QString &code);
bool autoRotate();           // EXIF-Ausrichtung beim Laden anwenden
void setAutoRotate(bool on);
int jpegQuality();
void setJpegQuality(int q);
bool losslessJpeg();          // JPEG nur gedreht/gespiegelt: ohne Neukodierung speichern
void setLosslessJpeg(bool on);
bool escapeQuits();
void setEscapeQuits(bool on);
bool askBeforeDiscard();      // Nachfrage „Änderungen speichern?“ (Standard: nein)
void setAskBeforeDiscard(bool on);
bool aiViewerAccess();        // KI-Schnittstelle darf das offene Fenster abfragen/steuern (Standard: nein)
bool aiViewerAccessDecided(); // Benutzer hat schon entschieden (Erststart-Frage)
void setAiViewerAccess(bool on);
// Ordnerfreigaben für die KI: siehe AiGuard
}

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    // Speichert die Werte; gibt zurück, ob sich die Sprache geändert hat.
    bool apply();
    bool autoRotateChanged() const;
    bool aiAccessChanged() const;

private:
    QComboBox *m_language;
    QCheckBox *m_autoRotate;
    QSpinBox *m_quality;
    QCheckBox *m_escape;
    QCheckBox *m_askDiscard;
    QCheckBox *m_lossless;
    QComboBox *m_aiWindow = nullptr;
    QCheckBox *m_aiAccess;
    void updateOverlapNote();

    QPlainTextEdit *m_aiReadDirs;
    QPlainTextEdit *m_aiWriteDirs;
    QCheckBox *m_aiOverwrite;
    QLabel *m_overlapNote;
    void refreshTrustedList();
    QStringList checkedTrusted() const;

    QListWidget *m_aiTrusted = nullptr;
    QLabel *m_trustNote = nullptr;
    QSpinBox *m_ocrJobs;

    QWidget *createAssocPage();
    void applyAssoc();
    void openAssocDetails();
    void refreshAssocStatus();
    QList<QPair<QString, QCheckBox *>> m_assocBoxes;
    QList<QPair<QString, QLabel *>> m_assocCurrent;
    QLabel *m_assocResult = nullptr;

    bool m_initialAutoRotate;
    bool m_initialAiAccess;
};

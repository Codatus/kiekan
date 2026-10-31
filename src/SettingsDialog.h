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
QString saveRecognizedText();  // "ask", "always", "never"
void setSaveRecognizedText(const QString &mode);
bool askBeforeDiscard();      // Nachfrage „Änderungen speichern?“ (Standard: nein)
void setAskBeforeDiscard(bool on);
// Bildschirmleser: was beim Blättern angesagt wird
bool announceName();
bool announcePosition();
bool announceSize();
bool announceDescription();
bool announceOcrProgress();     // Fortschritt der Texterkennung ansagen
QString caretKeys();            // Pfeiltasten im Textmodus: "auto" (bei Bildschirmleser), "always", "never"
void setAnnounce(const QString &what, bool on);   // "name", "position", "size", "description", "ocrProgress"
void setCaretKeys(const QString &mode);
// KI-Bildbeschreibungen über das Fenster
QString describeAhead();        // "ask" (beim ersten Mal fragen), "on", "off"
void setDescribeAhead(const QString &mode);
int describeAheadCount();       // so viele folgende Bilder vorab beschreiben
void setDescribeAheadCount(int n);
bool storeDescriptions();       // Beschreibungen der KI in den Bilddateien speichern
void setStoreDescriptions(bool on);
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
    QComboBox *m_saveText = nullptr;
    QCheckBox *m_lossless;
    QComboBox *m_aiWindow = nullptr;
    QComboBox *m_describeAhead = nullptr;
    QSpinBox *m_describeCount = nullptr;
    QCheckBox *m_storeDescriptions = nullptr;
    QCheckBox *m_srName = nullptr;
    QCheckBox *m_srPosition = nullptr;
    QCheckBox *m_srSize = nullptr;
    QCheckBox *m_srDescription = nullptr;
    QCheckBox *m_srOcrProgress = nullptr;
    QComboBox *m_caretKeys = nullptr;
    QWidget *createScreenReaderPage();
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

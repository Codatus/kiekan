// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
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
bool escapeQuits();
void setEscapeQuits(bool on);
bool aiViewerAccess();        // KI-Schnittstelle darf das offene Fenster abfragen/steuern
void setAiViewerAccess(bool on);
QStringList aiAllowedDirs();  // leer = keine Beschränkung
void setAiAllowedDirs(const QStringList &dirs);
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
    QCheckBox *m_aiAccess;
    QPlainTextEdit *m_aiDirs;
    QSpinBox *m_ocrJobs;
    bool m_initialAutoRotate;
    bool m_initialAiAccess;
};

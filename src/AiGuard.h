// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// Schutzmaßnahmen für die KI-Schnittstelle (MCP): Ordnerfreigaben zum Lesen
// und Schreiben, Kennzeichnung und Prüfung von Text aus Dateien (Prompt
// Injection) und das KI-Protokoll.
namespace AiGuard {

// --- Freigaben (Einstellungen) ---------------------------------------------------

QStringList readDirs();               // leer = KI darf keine Dateien lesen
void setReadDirs(const QStringList &dirs);
QStringList writeDirs();              // leer = KI darf nichts speichern
void setWriteDirs(const QStringList &dirs);
bool allowOverwrite();                // vorhandene Dateien ersetzen, Metadaten ändern
void setAllowOverwrite(bool on);
// Vertrauenswürdige Ordner: nur eigene Dateien. Dort keine Warnungen bei
// verdächtigem Text; Kennzeichnung, Protokoll und Rechte bleiben unverändert.
// Wirksam nur für Ordner aus der Leseliste. Dateien in Schreibordnern gelten
// nur über ihre Herkunft als vertrauenswürdig (siehe recordProvenance).
QStringList trustedDirs();
void setTrustedDirs(const QStringList &dirs);

// Wirksame, bereinigte Ordner (Einstellungen + KIEKAN_MCP_ROOTS /
// KIEKAN_MCP_WRITE_ROOTS). Lesen umfasst auch die Schreibordner.
QStringList effectiveReadRoots();
QStringList effectiveWriteRoots();
bool effectiveOverwrite();            // auch KIEKAN_MCP_ALLOW_OVERWRITE=1
QStringList effectiveTrustedRoots();  // auch KIEKAN_MCP_TRUSTED_ROOTS
// Sind alle Pfade vertrauenswürdig? (leer = nein) Dateien in Schreibordnern:
// nur, wenn das Herkunftsregister vertrauenswürdige Quellen belegt, die Datei
// seither unverändert ist und die KI keine eigenen Metadaten geschrieben hat.
bool isTrusted(const QStringList &resolvedPaths);

// Herkunftsregister (ki-herkunft.json im Datenordner): die KI hat `output`
// aus `sources` erzeugt bzw. eigene Metadaten in `path` geschrieben.
void recordProvenance(const QString &output, const QStringList &sources);
void markAiMetadata(const QString &path);

QString normalized(const QString &path);
bool isInside(const QString &resolvedPath, const QStringList &roots);

// Leseordner, die in einem Schreibordner liegen oder ihm gleichen – dort
// könnte die KI zwischen Originalen schreiben (Hinweis in den Einstellungen):
// Paare „Schreibordner ↔ Leseordner“. Ein Schreib-Unterordner zählt nicht.
QStringList overlaps(const QStringList &read, const QStringList &write);
// Schreibordner, in denen schon Dateien liegen (die die KI ersetzen dürfte).
QStringList nonEmptyWriteDirs(const QStringList &write);

// Bedenken zu vertrauenswürdigen Ordnern für den Einstellungsdialog:
// Download-, Temp- und Mail-Ordner, das ganze Home-Verzeichnis.
QStringList trustConcerns(const QStringList &trusted, const QStringList &read, const QStringList &write);

// --- Text aus Dateien ------------------------------------------------------------

struct Finding
{
    QString kind;      // kurze Art, z. B. "override instructions"
    QString excerpt;   // Fundstelle mit etwas Umgebung
};

// Sucht nach typischen Mustern eingeschleuster Anweisungen (deutsch/englisch),
// nach unsichtbaren Zeichen und versteckten Unicode-Tag-Zeichen.
QList<Finding> scan(const QString &text);

// Macht unsichtbare Steuerzeichen sichtbar ([U+200B]) und entschlüsselt
// Unicode-Tag-Zeichen („ASCII smuggling“) zu lesbarem Text.
QString revealHidden(const QString &text);

// Pfad oder Dateiname für Kopfzeilen und Hinweise außerhalb der Markierungen:
// eine Zeile, versteckte Zeichen sichtbar, keine Marker-Zeichen, gekürzt.
QString safeLabel(const QString &text);

// Text aus einer Datei für die KI verpacken: zwischen Markierungen mit
// zufälliger Kennung (nicht fälschbar), versteckte Zeichen sichtbar gemacht.
// Gefundene Auffälligkeiten werden an *findings angehängt.
QString wrapUntrusted(const QString &text, const QString &source, QList<Finding> *findings, bool trusted = false);

// Hinweisblock für die KI, wenn etwas gefunden wurde (sonst leer).
QString warningText(const QList<Finding> &findings, const QString &source);

// --- Protokoll ---------------------------------------------------------------------

QString logPath();
void appendLog(const QJsonObject &entry);     // Zeit wird ergänzt
QList<QJsonObject> readLog(int maxEntries = 5000);

} // namespace AiGuard

// Kiek an – schneller Bildbetrachter
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

// Wirksame, bereinigte Ordner (Einstellungen + KIEKAN_MCP_ROOTS /
// KIEKAN_MCP_WRITE_ROOTS). Lesen umfasst auch die Schreibordner.
QStringList effectiveReadRoots();
QStringList effectiveWriteRoots();
bool effectiveOverwrite();            // auch KIEKAN_MCP_ALLOW_OVERWRITE=1

QString normalized(const QString &path);
bool isInside(const QString &resolvedPath, const QStringList &roots);

// Schreibordner, die zugleich Leseordner sind oder sich mit ihnen überschneiden
// (für den Hinweis in den Einstellungen): Paare „Schreibordner → Leseordner“.
QStringList overlaps(const QStringList &read, const QStringList &write);

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

// Text aus einer Datei für die KI verpacken: zwischen Markierungen mit
// zufälliger Kennung (nicht fälschbar), versteckte Zeichen sichtbar gemacht.
// Gefundene Auffälligkeiten werden an *findings angehängt.
QString wrapUntrusted(const QString &text, const QString &source, QList<Finding> *findings);

// Hinweisblock für die KI, wenn etwas gefunden wurde (sonst leer).
QString warningText(const QList<Finding> &findings, const QString &source);

// --- Protokoll ---------------------------------------------------------------------

QString logPath();
void appendLog(const QJsonObject &entry);     // Zeit wird ergänzt
QList<QJsonObject> readLog(int maxEntries = 5000);

} // namespace AiGuard

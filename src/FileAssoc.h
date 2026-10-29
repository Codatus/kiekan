// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

// Dateizuordnungen: KiekAn! als Standardprogramm für Dateitypen eintragen.
// Unter Linux nach der freedesktop-Spezifikation „mime-apps“ (mimeapps.list),
// unter Windows nur Anleitung (Standard-Apps setzt dort nur der Benutzer).
namespace FileAssoc {

struct Group {
    QString key;           // "images", "raw", "pdf", "video"
    QString label;         // übersetzte Beschriftung
    QStringList mimeTypes;
    bool suggested;        // beim ersten Mal vorausgewählt
};

QList<Group> groups();

// Nur unter Linux/BSD kann KiekAn! die Zuordnung selbst setzen.
bool supported();

// Desktop-Datei von KiekAn! („kiekan.desktop“, im Flatpak die App-ID).
QString desktopId();

// Name des Programms, das den Typ derzeit öffnet (leer = unbekannt/keins).
QString currentDefault(const QString &mimeType);

// Öffnet KiekAn! alle Typen der Gruppe?
bool isDefault(const Group &group);

// Setzt KiekAn! für die Gruppen in `on` als Standard und nimmt es bei den
// Gruppen in `off` zurück (dort gilt dann wieder die Vorgabe des Systems).
// Gibt bei Erfolg eine leere Zeichenkette zurück, sonst die Fehlermeldung.
QString apply(const QStringList &on, const QStringList &off);

// Benutzerdateien, die geändert werden (für Anzeige und Tests).
QStringList userListFiles();

} // namespace FileAssoc

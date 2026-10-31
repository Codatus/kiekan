// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

class ImageView;
class QAccessibleInterface;

// Unterstützung für Bildschirmleser (Orca, NVDA, VoiceOver).
//
// Die Bildfläche hat genau ein „Element“, das der Bildschirmleser als
// Fokus sieht: das aktuelle Bild (Name, Beschreibung), den erkannten Text
// (Lesetext mit Cursor) oder den Fortschritt der Texterkennung. Wechselt das
// Element, bekommt das neue den Fokus – so lesen auch ältere Bildschirmleser
// (Orca 42 unter Qt 6.2) die Ansage vor, ganz ohne Ansage-Schnittstelle.
namespace Accessibility {

// Schnittstellen anmelden; einmal beim Start, vor dem ersten Fenster.
void install();

// Läuft ein Bildschirmleser (bzw. eine andere Hilfstechnik)?
bool screenReaderActive();

enum class Item { Image, Text, Progress };

// Neues Element zeigen und (wenn die Bildfläche den Fokus hat) ansagen.
void present(ImageView *view, Item item);
// Fortschritt 0..100 des Fortschritt-Elements melden.
void progress(ImageView *view, int percent);
// Textcursor und Markierung des Text-Elements melden.
void caretMoved(ImageView *view, int offset);
void selectionChanged(ImageView *view, int start, int end, int cursor);
// Das Text-Element hat neuen Inhalt.
void textReplaced(ImageView *view, const QString &oldText, const QString &newText);
// Die Bildfläche hat den Fokus bekommen: Fokus an das Element weitergeben.
void focusIn(ImageView *view);
// Element verwerfen (Bildfläche wird zerstört).
void forget(ImageView *view);

} // namespace Accessibility

// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QList>

class QAction;
class QLabel;
class QLineEdit;
class QTableWidget;

// Tastenkürzel frei belegen. Gespeichert wird nur, was vom Standard abweicht
// (QSettings: shortcuts/<Kennung der Aktion>).
class ShortcutDialog : public QDialog
{
    Q_OBJECT

public:
    ShortcutDialog(const QList<QAction *> &actions, const QHash<QString, QList<QKeySequence>> &defaults,
                   QWidget *parent = nullptr);

    // Belegung aus den Einstellungen auf die Aktionen anwenden.
    static void apply(const QList<QAction *> &actions, const QHash<QString, QList<QKeySequence>> &defaults);

    void accept() override;

private:
    void fill();
    void markConflicts();
    QList<QKeySequence> rowKeys(int row) const;

    QList<QAction *> m_actions;
    QHash<QString, QList<QKeySequence>> m_defaults;
    QHash<QString, QList<QKeySequence>> m_current;   // Bearbeitungsstand
    QTableWidget *m_table;
    QLineEdit *m_filter;
    QLabel *m_conflicts;
};

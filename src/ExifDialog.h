// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QLineEdit;
class QTableWidget;

// Zeigt EXIF/IPTC/XMP einer Datei: oben die wichtigsten Aufnahmedaten,
// darunter alle Einträge mit Suchfeld.
class ExifDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExifDialog(const QString &path, QWidget *parent = nullptr);

private:
    void applyFilter(const QString &text);

    QTableWidget *m_table;
    QLineEdit *m_search;
};

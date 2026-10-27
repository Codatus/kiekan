// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QComboBox;
class QLabel;
class QTableWidget;

// Zeigt das KI-Protokoll: welche Werkzeuge eine KI über die MCP-Schnittstelle
// aufgerufen hat, was sie gelesen und geschrieben hat und welche verdächtigen
// Texte (Prompt Injection) dabei auffielen.
class AiLogDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AiLogDialog(QWidget *parent = nullptr);

    enum Filter { All, Changes, Warnings };
    void setFilter(Filter filter);

private:
    void reload();

    QComboBox *m_filter;
    QTableWidget *m_table;
    QLabel *m_info;
};

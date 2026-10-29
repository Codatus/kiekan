// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShortcutDialog.h"

#include <QAction>
#include <QCollator>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

const QString kGroup = QStringLiteral("shortcuts/");

QString cleanText(const QAction *a)
{
    QString t = a->text();
    t.remove(QLatin1Char('&'));
    if (t.endsWith(QLatin1String(" …")))
        t.chop(2);
    return t;
}

QString keysText(const QList<QKeySequence> &keys)
{
    QStringList parts;
    for (const QKeySequence &k : keys)
        parts << k.toString(QKeySequence::NativeText);
    return parts.join(QStringLiteral(", "));
}

// Gespeicherte Belegung; std::nullopt-Ersatz: `found` = false heißt Standard.
QList<QKeySequence> stored(const QString &id, bool *found)
{
    QSettings s;
    *found = s.contains(kGroup + id);
    QList<QKeySequence> keys;
    if (*found)
        for (const QString &k : s.value(kGroup + id).toString().split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            keys << QKeySequence(k, QKeySequence::PortableText);
    return keys;
}

} // namespace

void ShortcutDialog::apply(const QList<QAction *> &actions, const QHash<QString, QList<QKeySequence>> &defaults)
{
    for (QAction *a : actions) {
        bool found = false;
        const QList<QKeySequence> keys = stored(a->objectName(), &found);
        a->setShortcuts(found ? keys : defaults.value(a->objectName()));
    }
}

ShortcutDialog::ShortcutDialog(const QList<QAction *> &actions, const QHash<QString, QList<QKeySequence>> &defaults,
                               QWidget *parent)
    : QDialog(parent), m_defaults(defaults)
{
    setWindowTitle(tr("Tastenkürzel anpassen"));
    // Alphabetisch nach Beschriftung
    m_actions = actions;
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(m_actions.begin(), m_actions.end(),
              [&](const QAction *a, const QAction *b) { return collator.compare(cleanText(a), cleanText(b)) < 0; });
    for (QAction *a : m_actions)
        m_current.insert(a->objectName(), a->shortcuts());

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Suchen …"));
    m_filter->setClearButtonEnabled(true);

    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels({tr("Aktion"), tr("Taste"), tr("Standard"), QString()});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->setColumnWidth(1, 170);

    m_conflicts = new QLabel(this);
    m_conflicts->setWordWrap(true);
    m_conflicts->setStyleSheet(QStringLiteral("color: #b00020"));

    auto *note = new QLabel(tr("Feld anklicken und die neue Taste drücken; Entf oder Rücktaste im leeren Feld "
                               "entfernt die Belegung. Die Taste ersetzt alle bisherigen Tasten der Aktion."), this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *resetAll = buttons->addButton(tr("Alle auf Standard"), QDialogButtonBox::ResetRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &ShortcutDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(resetAll, &QPushButton::clicked, this, [this] {
        for (QAction *a : m_actions)
            m_current.insert(a->objectName(), m_defaults.value(a->objectName()));
        fill();
    });
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            const QString hay = m_table->item(row, 0)->text() + QLatin1Char(' ') + m_table->item(row, 2)->text();
            m_table->setRowHidden(row, !text.isEmpty() && !hay.contains(text, Qt::CaseInsensitive));
        }
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_filter);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_conflicts);
    layout->addWidget(note);
    layout->addWidget(buttons);
    resize(640, 600);
    fill();
}

void ShortcutDialog::fill()
{
    m_table->setRowCount(int(m_actions.size()));
    for (int row = 0; row < m_actions.size(); ++row) {
        QAction *a = m_actions.at(row);
        const QString id = a->objectName();
        auto *name = new QTableWidgetItem(a->icon(), cleanText(a));
        name->setData(Qt::UserRole, id);
        m_table->setItem(row, 0, name);

        auto *edit = new QKeySequenceEdit(m_table);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
#endif
        const QList<QKeySequence> keys = m_current.value(id);
        edit->setKeySequence(keys.value(0));
        edit->setToolTip(keysText(keys));
        connect(edit, &QKeySequenceEdit::editingFinished, this, [this, id, edit] {
            const QKeySequence k = edit->keySequence();
            m_current.insert(id, k.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{k});
            edit->setToolTip(keysText(m_current.value(id)));
            markConflicts();
        });
        m_table->setCellWidget(row, 1, edit);

        auto *def = new QTableWidgetItem(keysText(m_defaults.value(id)));
        def->setForeground(Qt::gray);
        m_table->setItem(row, 2, def);

        auto *reset = new QToolButton(m_table);
        reset->setText(QStringLiteral("↺"));
        reset->setToolTip(tr("Standard wiederherstellen"));
        connect(reset, &QToolButton::clicked, this, [this, id, edit] {
            m_current.insert(id, m_defaults.value(id));
            edit->setKeySequence(m_defaults.value(id).value(0));
            edit->setToolTip(keysText(m_defaults.value(id)));
            markConflicts();
        });
        m_table->setCellWidget(row, 3, reset);
    }
    markConflicts();
}

void ShortcutDialog::markConflicts()
{
    QHash<QKeySequence, QStringList> owners;
    for (QAction *a : m_actions)
        for (const QKeySequence &k : m_current.value(a->objectName()))
            if (!k.isEmpty())
                owners[k] << cleanText(a);
    QStringList lines;
    for (auto it = owners.cbegin(); it != owners.cend(); ++it)
        if (it.value().size() > 1)
            lines << tr("%1 ist mehrfach belegt: %2").arg(it.key().toString(QKeySequence::NativeText),
                                                         it.value().join(QStringLiteral(", ")));
    lines.sort();
    m_conflicts->setText(lines.join(QLatin1Char('\n')));
    m_conflicts->setVisible(!lines.isEmpty());
}

void ShortcutDialog::accept()
{
    if (m_conflicts->isVisible()) {
        QMessageBox::warning(this, windowTitle(), tr("Bitte zuerst die doppelt belegten Tasten auflösen."));
        return;
    }
    QSettings s;
    for (QAction *a : m_actions) {
        const QString id = a->objectName();
        const QList<QKeySequence> keys = m_current.value(id);
        if (keys == m_defaults.value(id)) {
            s.remove(kGroup + id);
        } else {
            QStringList parts;
            for (const QKeySequence &k : keys)
                parts << k.toString(QKeySequence::PortableText);
            s.setValue(kGroup + id, parts.join(QLatin1Char('\n')));
        }
    }
    apply(m_actions, m_defaults);
    QDialog::accept();
}

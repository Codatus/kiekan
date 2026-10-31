// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Accessibility.h"

#include "ImageView.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWindow>

namespace Accessibility {

namespace {

bool isCurrent(const QAccessibleInterface *item, const QObject *view);

// Gemeinsame Grundlage der Elemente: gehören zur Bildfläche, ohne eigenes QObject.
class ItemBase : public QAccessibleInterface
{
public:
    explicit ItemBase(ImageView *view)
        : m_view(view)
        , m_name(view->accessibleInfoName())
        , m_description(view->accessibleInfoDescription())
        , m_serial(++s_serial)
    {
    }

    bool isValid() const override { return !m_view.isNull(); }
    QObject *object() const override { return nullptr; }
    QWindow *window() const override { return m_view ? m_view->window()->windowHandle() : nullptr; }
    QAccessibleInterface *parent() const override { return m_view ? QAccessible::queryAccessibleInterface(m_view) : nullptr; }
    QAccessibleInterface *child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface *) const override { return -1; }
    QAccessibleInterface *childAt(int, int) const override { return nullptr; }
    void setText(QAccessible::Text, const QString &) override {}
    // Orca hält Elemente mit gleicher Rolle, gleichem Namen und gleicher Lage
    // für dasselbe Objekt und schweigt dann – etwa, wenn zum selben Bild eine
    // neue Beschreibung kommt. Darum wechselt die Breite um ein Pixel.
    QRect rect() const override
    {
        return m_view ? QRect(m_view->mapToGlobal(QPoint(0, 0)), m_view->size()).adjusted(0, 0, -int(m_serial % 2), 0)
                      : QRect();
    }
    QAccessible::State state() const override
    {
        QAccessible::State s;
        if (!m_view)
            return s;
        s.focusable = true;
        // Ein abgelöstes Element lebt noch kurz weiter, hat aber keinen Fokus.
        s.focused = m_view->hasFocus() && isCurrent(this, m_view);
        s.readOnly = true;
        return s;
    }

protected:
    QPointer<ImageView> m_view;
    // Name und Beschreibung zum Zeitpunkt der Ansage: Ein abgelöstes Element
    // darf nicht den Namen des neuen Bildes melden.
    const QString m_name;
    const QString m_description;
    const quint64 m_serial;
    static inline quint64 s_serial = 0;
};

class ImageItem : public ItemBase
{
public:
    using ItemBase::ItemBase;
    QAccessible::Role role() const override { return QAccessible::Graphic; }
    QString text(QAccessible::Text t) const override
    {
        if (t == QAccessible::Name)
            return m_name;
        if (t == QAccessible::Description)
            return m_description;
        return {};
    }
};

class ProgressItem : public ItemBase, public QAccessibleValueInterface
{
public:
    using ItemBase::ItemBase;
    int value = 0;
    QAccessible::Role role() const override { return QAccessible::ProgressBar; }
    QString text(QAccessible::Text t) const override
    {
        if (t == QAccessible::Name)
            return QCoreApplication::translate("Accessibility", "Texterkennung");
        if (t == QAccessible::Value)
            return QStringLiteral("%1 %").arg(value);
        return {};
    }
    void *interface_cast(QAccessible::InterfaceType t) override
    {
        return t == QAccessible::ValueInterface ? static_cast<QAccessibleValueInterface *>(this) : nullptr;
    }
    QVariant currentValue() const override { return value; }
    void setCurrentValue(const QVariant &) override {}
    QVariant maximumValue() const override { return 100; }
    QVariant minimumValue() const override { return 0; }
    QVariant minimumStepSize() const override { return 1; }
};

// Erkannter Text als mehrzeiliger, schreibgeschützter Lesetext.
class TextItem : public ItemBase, public QAccessibleTextInterface
{
public:
    using ItemBase::ItemBase;
    QAccessible::Role role() const override { return QAccessible::EditableText; }
    QAccessible::State state() const override
    {
        QAccessible::State s = ItemBase::state();
        s.multiLine = true;
        s.selectableText = true;
        return s;
    }
    QString text(QAccessible::Text t) const override
    {
        if (!m_view)
            return {};
        if (t == QAccessible::Name)
            return QCoreApplication::translate("Accessibility", "Erkannter Text");
        if (t == QAccessible::Description)
            return m_description;
        if (t == QAccessible::Value)
            return m_view->fullText();
        return {};
    }
    void *interface_cast(QAccessible::InterfaceType t) override
    {
        return t == QAccessible::TextInterface ? static_cast<QAccessibleTextInterface *>(this) : nullptr;
    }

    void selection(int index, int *start, int *end) const override
    {
        *start = *end = 0;
        if (m_view && index == 0)
            m_view->selectionOffsets(start, end);
    }
    int selectionCount() const override
    {
        if (!m_view)
            return 0;
        int s, e;
        m_view->selectionOffsets(&s, &e);
        return e > s ? 1 : 0;
    }
    void addSelection(int start, int end) override { setSelection(0, start, end); }
    void removeSelection(int) override
    {
        if (m_view)
            m_view->setSelectionOffsets(0, 0);
    }
    void setSelection(int, int start, int end) override
    {
        if (m_view)
            m_view->setSelectionOffsets(std::min(start, end), std::max(start, end));
    }
    int cursorPosition() const override { return m_view ? m_view->caretOffset() : 0; }
    void setCursorPosition(int position) override
    {
        if (m_view)
            m_view->setCaretOffset(position);
    }
    QString text(int start, int end) const override
    {
        return m_view ? m_view->fullText().mid(start, std::max(0, end - start)) : QString();
    }
    int characterCount() const override { return m_view ? int(m_view->fullText().size()) : 0; }
    QRect characterRect(int offset) const override
    {
        if (!m_view)
            return {};
        const QRect r = m_view->characterRect(offset);
        return QRect(m_view->mapToGlobal(r.topLeft()), r.size());
    }
    int offsetAtPoint(const QPoint &point) const override
    {
        return m_view ? m_view->offsetAt(m_view->mapFromGlobal(point)) : -1;
    }
    void scrollToSubstring(int, int) override {}
    QString attributes(int offset, int *startOffset, int *endOffset) const override
    {
        *startOffset = 0;
        *endOffset = std::max(offset, characterCount());
        return {};
    }
};

// Die Bildfläche selbst: Behälter des aktuellen Elements.
class ViewAccessible : public QAccessibleWidget
{
public:
    explicit ViewAccessible(QWidget *w) : QAccessibleWidget(w, QAccessible::Pane) {}
    int childCount() const override;
    QAccessibleInterface *child(int index) const override;
    int indexOfChild(const QAccessibleInterface *child) const override;
    QAccessibleInterface *focusChild() const override { return child(0); }
    QAccessible::State state() const override
    {
        QAccessible::State s = QAccessibleWidget::state();
        if (childCount() > 0)
            s.focused = false;   // der Fokus liegt beim Element
        return s;
    }
    QString text(QAccessible::Text t) const override
    {
        // Ohne Namen gilt die Fläche als reine Hülle und wird nicht mit
        // angesagt – vorgelesen wird das Element darin.
        if (t == QAccessible::Name)
            return QString();
        return QAccessibleWidget::text(t);
    }
};

struct State
{
    QAccessible::Id id = 0;
    Item kind = Item::Image;
};
QHash<const QObject *, State> g_items;

QAccessibleInterface *current(const QObject *view)
{
    const auto it = g_items.constFind(view);
    return it == g_items.constEnd() ? nullptr : QAccessible::accessibleInterface(it->id);
}

bool isCurrent(const QAccessibleInterface *item, const QObject *view)
{
    return item && item == current(view);
}

int ViewAccessible::childCount() const { return current(object()) ? 1 : 0; }
QAccessibleInterface *ViewAccessible::child(int index) const { return index == 0 ? current(object()) : nullptr; }
int ViewAccessible::indexOfChild(const QAccessibleInterface *child) const
{
    return child && child == current(object()) ? 0 : -1;
}

QAccessibleInterface *factory(const QString &className, QObject *object)
{
    if (className == QLatin1String("ImageView") && object && object->isWidgetType())
        return new ViewAccessible(static_cast<QWidget *>(object));
    return nullptr;
}

} // namespace

void install()
{
    QAccessible::installFactory(factory);
}

bool screenReaderActive()
{
    return QAccessible::isActive();
}

void present(ImageView *view, Item kind)
{
    if (!view || !QAccessible::isActive())
        return;
    QAccessibleInterface *item = nullptr;
    switch (kind) {
    case Item::Image:
        item = new ImageItem(view);
        break;
    case Item::Text:
        item = new TextItem(view);
        break;
    case Item::Progress:
        item = new ProgressItem(view);
        break;
    }
    // Altes Element erst nach dem Fokuswechsel verwerfen.
    const QAccessible::Id old = g_items.value(view).id;
    const QAccessible::Id id = QAccessible::registerAccessibleInterface(item);
    if (!g_items.contains(view))
        QObject::connect(view, &QObject::destroyed, [view] { forget(view); });
    g_items.insert(view, {id, kind});
    {
        QAccessibleEvent ev(QAccessible::queryAccessibleInterface(view), QAccessible::ObjectReorder);
        QAccessible::updateAccessibility(&ev);
    }
    if (view->hasFocus()) {
        QAccessibleEvent ev(item, QAccessible::Focus);
        QAccessible::updateAccessibility(&ev);
    }
    // Das alte Element noch kurz leben lassen: Der Bildschirmleser fragt beim
    // Fokuswechsel nach dessen Vorfahren; ohne sie sagt er den ganzen Pfad an.
    if (old)
        QTimer::singleShot(5000, [old] {
            if (QAccessible::accessibleInterface(old))
                QAccessible::deleteAccessibleInterface(old);
        });
}

void progress(ImageView *view, int percent)
{
    const auto it = g_items.constFind(view);
    if (it == g_items.constEnd() || it->kind != Item::Progress)
        return;
    auto *item = static_cast<ProgressItem *>(QAccessible::accessibleInterface(it->id));
    if (!item || item->value == percent)
        return;
    item->value = percent;
    QAccessibleValueChangeEvent ev(item, percent);
    QAccessible::updateAccessibility(&ev);
}

void caretMoved(ImageView *view, int offset)
{
    const auto it = g_items.constFind(view);
    if (it == g_items.constEnd() || it->kind != Item::Text)
        return;
    QAccessibleTextCursorEvent ev(QAccessible::accessibleInterface(it->id), offset);
    QAccessible::updateAccessibility(&ev);
}

void selectionChanged(ImageView *view, int start, int end, int cursor)
{
    const auto it = g_items.constFind(view);
    if (it == g_items.constEnd() || it->kind != Item::Text)
        return;
    QAccessibleTextSelectionEvent ev(QAccessible::accessibleInterface(it->id), start, end);
    ev.setCursorPosition(cursor);
    QAccessible::updateAccessibility(&ev);
}

void textReplaced(ImageView *view, const QString &oldText, const QString &newText)
{
    const auto it = g_items.constFind(view);
    if (it == g_items.constEnd() || it->kind != Item::Text)
        return;
    QAccessibleTextUpdateEvent ev(QAccessible::accessibleInterface(it->id), 0, oldText, newText);
    QAccessible::updateAccessibility(&ev);
}

void focusIn(ImageView *view)
{
    const auto it = g_items.constFind(view);
    if (it == g_items.constEnd() || !QAccessible::isActive())
        return;
    if (QAccessibleInterface *item = QAccessible::accessibleInterface(it->id)) {
        QAccessibleEvent ev(item, QAccessible::Focus);
        QAccessible::updateAccessibility(&ev);
    }
}

void forget(ImageView *view)
{
    const auto it = g_items.find(view);
    if (it == g_items.end())
        return;
    const QAccessible::Id id = it->id;
    g_items.erase(it);
    if (id)
        QAccessible::deleteAccessibleInterface(id);
}

} // namespace Accessibility

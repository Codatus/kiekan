// Kiek an – DjVu (über DjVuLibre)
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mehrseitige DjVu-Dokumente (eingescannte Bücher und Akten). Seiten werden
// mit höchstens 300 dpi gerendert – genug für Anzeige und Texterkennung,
// ohne 600-dpi-Scans unnötig groß zu machen. Die Seitendrehung aus dem
// Dokument wird übernommen.

#include <QFile>
#include <QImage>
#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QIODevice>

#include <libdjvu/ddjvuapi.h>

#include <cmath>
#include <memory>

namespace {

constexpr double kMaxDpi = 300.0;

bool isDjvu(const QByteArray &head)
{
    // „AT&TFORM“ + Länge + „DJVU“ (einseitig) oder „DJVM“ (mehrseitig)
    return head.size() >= 16 && head.startsWith("AT&TFORM") && (head.mid(12, 4) == "DJVU" || head.mid(12, 4) == "DJVM");
}

class Document
{
public:
    ~Document()
    {
        if (m_doc)
            ddjvu_document_release(m_doc);
        if (m_ctx)
            ddjvu_context_release(m_ctx);
    }

    bool open(QIODevice *device)
    {
        m_ctx = ddjvu_context_create("kiekan");
        if (!m_ctx)
            return false;
        if (auto *file = qobject_cast<QFile *>(device); file && !file->fileName().isEmpty()) {
            // Über den Dateinamen – so findet DjVuLibre auch Seiten
            // „indirekter“ Dokumente, die in Nachbardateien liegen.
            m_doc = ddjvu_document_create_by_filename_utf8(m_ctx, file->fileName().toUtf8().constData(), TRUE);
        } else {
            m_data = device->readAll();
            m_doc = ddjvu_document_create(m_ctx, nullptr, TRUE);
            if (m_doc) {
                ddjvu_stream_write(m_doc, 0, m_data.constData(), ulong(m_data.size()));
                ddjvu_stream_close(m_doc, 0, FALSE);
            }
        }
        if (!m_doc)
            return false;
        while (!ddjvu_document_decoding_done(m_doc))
            pump(true);
        return !ddjvu_document_decoding_error(m_doc);
    }

    int pageCount() const { return m_doc ? ddjvu_document_get_pagenum(m_doc) : 0; }

    QSize pageSize(int pageNo)
    {
        ddjvu_pageinfo_t info;
        ddjvu_status_t st;
        while ((st = ddjvu_document_get_pageinfo(m_doc, pageNo, &info)) < DDJVU_JOB_OK)
            pump(true);
        if (st != DDJVU_JOB_OK)
            return {};
        const double scale = std::min(1.0, kMaxDpi / std::max(1, info.dpi));
        QSize s(int(std::lround(info.width * scale)), int(std::lround(info.height * scale)));
        if (info.rotation % 2 == 1)   // 90° oder 270°
            s.transpose();
        return s;
    }

    QImage render(int pageNo)
    {
        ddjvu_page_t *page = ddjvu_page_create_by_pageno(m_doc, pageNo);
        if (!page)
            return {};
        while (!ddjvu_page_decoding_done(page))
            pump(true);
        if (ddjvu_page_decoding_error(page)) {
            ddjvu_page_release(page);
            return {};
        }
        ddjvu_page_set_rotation(page, ddjvu_page_get_initial_rotation(page));
        int w = ddjvu_page_get_width(page), h = ddjvu_page_get_height(page);
        const int dpi = std::max(1, ddjvu_page_get_resolution(page));
        const ddjvu_page_rotation_t rot = ddjvu_page_get_rotation(page);
        if (rot == DDJVU_ROTATE_90 || rot == DDJVU_ROTATE_270)
            std::swap(w, h);
        const double scale = std::min(1.0, kMaxDpi / dpi);
        const int tw = std::max(1, int(std::lround(w * scale))), th = std::max(1, int(std::lround(h * scale)));

        QImage img(tw, th, QImage::Format_RGB888);
        ddjvu_format_t *fmt = ddjvu_format_create(DDJVU_FORMAT_RGB24, 0, nullptr);
        ddjvu_format_set_row_order(fmt, 1);   // oberste Zeile zuerst
        ddjvu_rect_t rect{0, 0, uint(tw), uint(th)};
        if (!ddjvu_page_render(page, DDJVU_RENDER_COLOR, &rect, &rect, fmt, ulong(img.bytesPerLine()),
                               reinterpret_cast<char *>(img.bits())))
            img.fill(Qt::white);   // leere Seite
        ddjvu_format_release(fmt);
        ddjvu_page_release(page);

        // Auflösung vermerken (wichtig für die Texterkennung)
        const int dpm = int(std::lround(dpi * scale / 0.0254));
        img.setDotsPerMeterX(dpm);
        img.setDotsPerMeterY(dpm);
        return img;
    }

private:
    void pump(bool wait)
    {
        if (wait)
            ddjvu_message_wait(m_ctx);
        while (ddjvu_message_peek(m_ctx))
            ddjvu_message_pop(m_ctx);
    }

    ddjvu_context_t *m_ctx = nullptr;
    ddjvu_document_t *m_doc = nullptr;
    QByteArray m_data;
};

class DjvuHandler : public QImageIOHandler
{
public:
    bool canRead() const override
    {
        if (device() && isDjvu(device()->peek(16))) {
            setFormat("djvu");
            return true;
        }
        return false;
    }

    bool read(QImage *image) override
    {
        if (!ensureOpen() || m_page >= m_doc->pageCount())
            return false;
        *image = m_doc->render(m_page);
        return !image->isNull();
    }

    int imageCount() const override { return const_cast<DjvuHandler *>(this)->ensureOpen() ? m_doc->pageCount() : 0; }
    int currentImageNumber() const override { return m_page; }
    bool jumpToImage(int n) override
    {
        if (!ensureOpen() || n < 0 || n >= m_doc->pageCount())
            return false;
        m_page = n;
        return true;
    }
    bool jumpToNextImage() override { return jumpToImage(m_page + 1); }

    QVariant option(ImageOption option) const override
    {
        if (option == Size && const_cast<DjvuHandler *>(this)->ensureOpen())
            return m_doc->pageSize(m_page);
        return {};
    }

    bool supportsOption(ImageOption option) const override { return option == Size; }

private:
    bool ensureOpen()
    {
        if (m_doc)
            return m_ok;
        m_doc = std::make_unique<Document>();
        m_ok = device() && m_doc->open(device());
        return m_ok;
    }

    std::unique_ptr<Document> m_doc;
    bool m_ok = false;
    int m_page = 0;
};

} // namespace

class djvu : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QImageIOHandlerFactoryInterface_iid FILE "djvu.json")

public:
    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override
    {
        if (format == "djvu" || format == "djv")
            return CanRead;
        if (!format.isEmpty())
            return {};
        return device && isDjvu(device->peek(16)) ? CanRead : Capabilities();
    }

    QImageIOHandler *create(QIODevice *device, const QByteArray &format) const override
    {
        auto *h = new DjvuHandler;
        h->setDevice(device);
        h->setFormat(format);
        return h;
    }
};

#include "DjvuPlugin.moc"

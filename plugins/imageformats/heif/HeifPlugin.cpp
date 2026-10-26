// Kiek an – Bildformate HEIC/HEIF und AVIF (über libheif)
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HEIC (iPhone-Fotos, HEVC) und AVIF (AV1) sind beide HEIF-Container;
// libheif bringt die Decoder mit (libde265, dav1d/aom). Drehung und
// Spiegelung aus dem Container (irot/imir) wendet libheif selbst an, das
// Bild kommt also aufrecht an.

#include <QColorSpace>
#include <QImage>
#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QIODevice>

#include <libheif/heif.h>

#include <algorithm>
#include <memory>

namespace {

// Marken im „ftyp“-Kasten, die auf HEIF/AVIF hinweisen
bool isHeif(const QByteArray &head)
{
    if (head.size() < 12 || head.mid(4, 4) != "ftyp")
        return false;
    static const char *brands[] = {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1", "avif", "avis"};
    const QByteArray major = head.mid(8, 4);
    for (const char *b : brands)
        if (major == b)
            return true;
    // Kompatible Marken im Rest des Kastens
    const quint32 boxSize = (quint32(uchar(head[0])) << 24) | (quint32(uchar(head[1])) << 16)
                          | (quint32(uchar(head[2])) << 8) | quint32(uchar(head[3]));
    const QByteArray box = head.left(int(std::min<quint32>(boxSize, quint32(head.size()))));
    for (int i = 16; i + 4 <= box.size(); i += 4)
        for (const char *b : brands)
            if (box.mid(i, 4) == b)
                return true;
    return false;
}

struct ContextDeleter { void operator()(heif_context *c) const { heif_context_free(c); } };
struct HandleDeleter { void operator()(heif_image_handle *h) const { heif_image_handle_release(h); } };
struct ImageDeleter { void operator()(heif_image *i) const { heif_image_release(i); } };

class HeifHandler : public QImageIOHandler
{
public:
    bool canRead() const override
    {
        if (device() && isHeif(device()->peek(64))) {
            setFormat("heif");
            return true;
        }
        return false;
    }

    bool read(QImage *image) override
    {
        const QByteArray data = device()->readAll();
        std::unique_ptr<heif_context, ContextDeleter> ctx(heif_context_alloc());
        if (heif_context_read_from_memory_without_copy(ctx.get(), data.constData(), size_t(data.size()), nullptr).code
            != heif_error_Ok)
            return false;
        heif_image_handle *rawHandle = nullptr;
        if (heif_context_get_primary_image_handle(ctx.get(), &rawHandle).code != heif_error_Ok)
            return false;
        std::unique_ptr<heif_image_handle, HandleDeleter> handle(rawHandle);

        const bool alpha = heif_image_handle_has_alpha_channel(handle.get());
        heif_image *rawImg = nullptr;
        if (heif_decode_image(handle.get(), &rawImg, heif_colorspace_RGB,
                              alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB, nullptr).code
            != heif_error_Ok)
            return false;
        std::unique_ptr<heif_image, ImageDeleter> img(rawImg);

        int stride = 0;
        const uint8_t *plane = heif_image_get_plane_readonly(img.get(), heif_channel_interleaved, &stride);
        const int w = heif_image_get_width(img.get(), heif_channel_interleaved);
        const int h = heif_image_get_height(img.get(), heif_channel_interleaved);
        if (!plane || w <= 0 || h <= 0)
            return false;
        *image = QImage(plane, w, h, stride, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888).copy();

        // Farbprofil übernehmen, falls eingebettet (z. B. Display P3 beim iPhone)
        const size_t iccSize = heif_image_handle_get_raw_color_profile_size(handle.get());
        if (iccSize > 0) {
            QByteArray icc(int(iccSize), Qt::Uninitialized);
            if (heif_image_handle_get_raw_color_profile(handle.get(), icc.data()).code == heif_error_Ok)
                image->setColorSpace(QColorSpace::fromIccProfile(icc));
        }
        return !image->isNull();
    }

    QVariant option(ImageOption option) const override
    {
        if (option != Size || !device())
            return {};
        // Die Maße stehen im „meta“-Kasten, meist am Dateianfang: erst 1 MB
        // versuchen, nur wenn das nicht reicht die ganze Datei.
        for (qint64 amount : {qint64(1024 * 1024), device()->size()}) {
            if (amount <= 0)
                continue;
            const QByteArray data = device()->peek(amount);
            std::unique_ptr<heif_context, ContextDeleter> ctx(heif_context_alloc());
            if (heif_context_read_from_memory_without_copy(ctx.get(), data.constData(), size_t(data.size()), nullptr).code
                != heif_error_Ok)
                continue;
            heif_image_handle *rawHandle = nullptr;
            if (heif_context_get_primary_image_handle(ctx.get(), &rawHandle).code != heif_error_Ok)
                continue;
            std::unique_ptr<heif_image_handle, HandleDeleter> handle(rawHandle);
            return QSize(heif_image_handle_get_width(handle.get()), heif_image_handle_get_height(handle.get()));
        }
        return {};
    }

    bool supportsOption(ImageOption option) const override { return option == Size; }
};

} // namespace

class heif : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QImageIOHandlerFactoryInterface_iid FILE "heif.json")

public:
    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override
    {
        if (format == "heic" || format == "heif" || format == "hif" || format == "avif")
            return CanRead;
        if (!format.isEmpty())
            return {};
        return device && isHeif(device->peek(64)) ? CanRead : Capabilities();
    }

    QImageIOHandler *create(QIODevice *device, const QByteArray &format) const override
    {
        auto *h = new HeifHandler;
        h->setDevice(device);
        h->setFormat(format);
        return h;
    }
};

#include "HeifPlugin.moc"

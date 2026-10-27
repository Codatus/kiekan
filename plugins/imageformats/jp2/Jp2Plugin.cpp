// KiekAn! – JPEG 2000 (über OpenJPEG)
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later
//
// JP2-Container und rohe J2K-Codestreams; Graustufen, RGB, RGBA und sYCC,
// beliebige Bittiefe (auf 8 Bit gebracht), Unterabtastung der Farbkanäle.

#include <QImage>
#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QIODevice>
#include <QThread>

#include <openjpeg.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace {

const QByteArray kJp2Magic("\x00\x00\x00\x0c\x6a\x50\x20\x20\x0d\x0a\x87\x0a", 12);
const QByteArray kJ2kMagic("\xff\x4f\xff\x51", 4);

enum class Kind { None, Jp2, J2k };

Kind detect(const QByteArray &head)
{
    if (head.startsWith(kJp2Magic))
        return Kind::Jp2;
    if (head.startsWith(kJ2kMagic))
        return Kind::J2k;
    return Kind::None;
}

// OpenJPEG liest über Rückruffunktionen aus dem Speicher.
struct Memory
{
    const uchar *data;
    OPJ_SIZE_T size;
    OPJ_SIZE_T pos = 0;
};

OPJ_SIZE_T readFn(void *buffer, OPJ_SIZE_T n, void *user)
{
    auto *m = static_cast<Memory *>(user);
    if (m->pos >= m->size)
        return OPJ_SIZE_T(-1);
    const OPJ_SIZE_T count = std::min(n, m->size - m->pos);
    memcpy(buffer, m->data + m->pos, count);
    m->pos += count;
    return count;
}

OPJ_OFF_T skipFn(OPJ_OFF_T n, void *user)
{
    auto *m = static_cast<Memory *>(user);
    const OPJ_OFF_T target = std::clamp<OPJ_OFF_T>(OPJ_OFF_T(m->pos) + n, 0, OPJ_OFF_T(m->size));
    const OPJ_OFF_T moved = target - OPJ_OFF_T(m->pos);
    m->pos = OPJ_SIZE_T(target);
    return moved;
}

OPJ_BOOL seekFn(OPJ_OFF_T n, void *user)
{
    auto *m = static_cast<Memory *>(user);
    if (n < 0 || OPJ_SIZE_T(n) > m->size)
        return OPJ_FALSE;
    m->pos = OPJ_SIZE_T(n);
    return OPJ_TRUE;
}

struct CodecDeleter { void operator()(opj_codec_t *c) const { opj_destroy_codec(c); } };
struct StreamDeleter { void operator()(opj_stream_t *s) const { opj_stream_destroy(s); } };
struct ImageDeleter { void operator()(opj_image_t *i) const { opj_image_destroy(i); } };

struct Decoder
{
    Memory mem;
    std::unique_ptr<opj_codec_t, CodecDeleter> codec;
    std::unique_ptr<opj_stream_t, StreamDeleter> stream;
    opj_image_t *header = nullptr;   // gehört dem Aufrufer über `image`
    std::unique_ptr<opj_image_t, ImageDeleter> image;

    bool open(const QByteArray &data)
    {
        const Kind kind = detect(data);
        if (kind == Kind::None)
            return false;
        mem = {reinterpret_cast<const uchar *>(data.constData()), OPJ_SIZE_T(data.size())};
        codec.reset(opj_create_decompress(kind == Kind::Jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K));
        opj_dparameters_t params;
        opj_set_default_decoder_parameters(&params);
        if (!codec || !opj_setup_decoder(codec.get(), &params))
            return false;
        // OpenJPEG hat einen eigenen Thread-Pool (kein OpenMP); große Archivscans profitieren.
        opj_codec_set_threads(codec.get(), std::clamp(QThread::idealThreadCount(), 1, 4));
        stream.reset(opj_stream_default_create(OPJ_TRUE));
        opj_stream_set_user_data(stream.get(), &mem, nullptr);
        opj_stream_set_user_data_length(stream.get(), mem.size);
        opj_stream_set_read_function(stream.get(), readFn);
        opj_stream_set_skip_function(stream.get(), skipFn);
        opj_stream_set_seek_function(stream.get(), seekFn);
        if (!opj_read_header(stream.get(), codec.get(), &header))
            return false;
        image.reset(header);
        return true;
    }

    bool decode()
    {
        return opj_decode(codec.get(), stream.get(), image.get()) && opj_end_decompress(codec.get(), stream.get());
    }
};

uchar sample8(const opj_image_comp_t &c, int x, int y)
{
    const int sx = std::min(int(c.w) - 1, x / int(std::max<OPJ_UINT32>(1, c.dx)));
    const int sy = std::min(int(c.h) - 1, y / int(std::max<OPJ_UINT32>(1, c.dy)));
    int v = c.data[sy * int(c.w) + sx];
    if (c.sgnd)
        v += 1 << (c.prec - 1);
    if (c.prec > 8)
        v >>= (c.prec - 8);
    else if (c.prec < 8)
        v <<= (8 - c.prec);
    return uchar(std::clamp(v, 0, 255));
}

QImage convert(const opj_image_t *img)
{
    const int n = int(img->numcomps);
    if (n < 1)
        return {};
    const int w = int(img->x1 - img->x0), h = int(img->y1 - img->y0);
    if (w <= 0 || h <= 0)
        return {};
    const bool gray = n < 3;
    const bool alpha = n == 2 || n >= 4;
    const bool sycc = img->color_space == OPJ_CLRSPC_SYCC
                   || (img->color_space == OPJ_CLRSPC_UNSPECIFIED && n >= 3 && img->comps[1].dx > 1);
    QImage out(w, h, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    if (out.isNull())
        return {};
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            int r, g, b;
            if (gray) {
                r = g = b = sample8(img->comps[0], x, y);
            } else if (sycc) {
                const double Y = sample8(img->comps[0], x, y);
                const double Cb = sample8(img->comps[1], x, y) - 128.0;
                const double Cr = sample8(img->comps[2], x, y) - 128.0;
                r = std::clamp(int(Y + 1.402 * Cr), 0, 255);
                g = std::clamp(int(Y - 0.344136 * Cb - 0.714136 * Cr), 0, 255);
                b = std::clamp(int(Y + 1.772 * Cb), 0, 255);
            } else {
                r = sample8(img->comps[0], x, y);
                g = sample8(img->comps[1], x, y);
                b = sample8(img->comps[2], x, y);
            }
            const int a = alpha ? sample8(img->comps[gray ? 1 : 3], x, y) : 255;
            line[x] = qRgba(r, g, b, a);
        }
    }
    return out;
}

class Jp2Handler : public QImageIOHandler
{
public:
    bool canRead() const override
    {
        if (device() && detect(device()->peek(12)) != Kind::None) {
            setFormat("jp2");
            return true;
        }
        return false;
    }

    bool read(QImage *image) override
    {
        const QByteArray data = device()->readAll();
        Decoder d;
        if (!d.open(data) || !d.decode())
            return false;
        *image = convert(d.image.get());
        return !image->isNull();
    }

    QVariant option(ImageOption option) const override
    {
        if (option != Size || !device())
            return {};
        // Der Kopf (SIZ-Marke) steht am Anfang; ein kleiner Ausschnitt genügt.
        const QByteArray head = device()->peek(64 * 1024);
        Decoder d;
        if (!d.open(head))
            return {};
        return QSize(int(d.image->x1 - d.image->x0), int(d.image->y1 - d.image->y0));
    }

    bool supportsOption(ImageOption option) const override { return option == Size; }
};

} // namespace

class jp2 : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QImageIOHandlerFactoryInterface_iid FILE "jp2.json")

public:
    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override
    {
        if (format == "jp2" || format == "j2k" || format == "j2c" || format == "jpf" || format == "jpx")
            return CanRead;
        if (!format.isEmpty())
            return {};
        return device && detect(device->peek(12)) != Kind::None ? CanRead : Capabilities();
    }

    QImageIOHandler *create(QIODevice *device, const QByteArray &format) const override
    {
        auto *h = new Jp2Handler;
        h->setDevice(device);
        h->setFormat(format);
        return h;
    }
};

#include "Jp2Plugin.moc"

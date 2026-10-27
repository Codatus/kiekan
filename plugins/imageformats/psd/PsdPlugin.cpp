// KiekAn! – Bildformat PSD/PSB (Photoshop)
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Liest das zusammengesetzte Gesamtbild („Image Data“-Abschnitt), das
// Photoshop bei „Kompatibilität maximieren“ mitspeichert. Ebenen werden
// nicht einzeln ausgewertet. Unterstützt: Bitmap, Graustufen, Duplex,
// indizierte Farben, RGB, CMYK, Lab, Mehrkanal; 1, 8, 16 und 32 Bit;
// unkomprimiert und RLE.

#include <QImage>
#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QIODevice>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

class Reader
{
public:
    explicit Reader(const QByteArray &d) : m_d(d) {}

    bool ok() const { return m_ok; }
    qint64 pos() const { return m_pos; }
    void seek(qint64 p) { if (p < 0 || p > m_d.size()) m_ok = false; else m_pos = p; }
    void skip(qint64 n) { seek(m_pos + n); }
    const uchar *ptr() const { return reinterpret_cast<const uchar *>(m_d.constData()) + m_pos; }
    qint64 left() const { return m_d.size() - m_pos; }

    quint64 be(int bytes)
    {
        if (left() < bytes) {
            m_ok = false;
            return 0;
        }
        quint64 v = 0;
        for (int i = 0; i < bytes; ++i)
            v = (v << 8) | uchar(m_d.at(m_pos + i));
        m_pos += bytes;
        return v;
    }

private:
    const QByteArray &m_d;
    qint64 m_pos = 0;
    bool m_ok = true;
};

struct Header
{
    int version = 0;
    int channels = 0;
    quint32 width = 0;
    quint32 height = 0;
    int depth = 0;
    int mode = 0;
};

bool readHeader(Reader &r, Header &h)
{
    if (r.be(4) != 0x38425053)   // "8BPS"
        return false;
    h.version = int(r.be(2));
    r.skip(6);
    h.channels = int(r.be(2));
    h.height = quint32(r.be(4));
    h.width = quint32(r.be(4));
    h.depth = int(r.be(2));
    h.mode = int(r.be(2));
    return r.ok() && (h.version == 1 || h.version == 2) && h.channels > 0 && h.width > 0 && h.height > 0
        && (h.depth == 1 || h.depth == 8 || h.depth == 16 || h.depth == 32);
}

// PackBits
bool unpackRow(const uchar *src, qint64 srcLen, uchar *dst, qint64 dstLen)
{
    qint64 i = 0, o = 0;
    while (i < srcLen && o < dstLen) {
        const int n = static_cast<signed char>(src[i++]);
        if (n >= 0) {
            const qint64 count = qint64(n) + 1;
            if (i + count > srcLen || o + count > dstLen)
                return false;
            std::copy(src + i, src + i + count, dst + o);
            i += count;
            o += count;
        } else if (n != -128) {
            const qint64 count = 1 - qint64(n);
            if (i >= srcLen || o + count > dstLen)
                return false;
            std::fill(dst + o, dst + o + count, src[i++]);
            o += count;
        }
    }
    return o == dstLen;
}

uchar to8(const uchar *p, int depth)
{
    if (depth == 8)
        return p[0];
    if (depth == 16)
        return p[0];   // höherwertiges Byte
    // 32 Bit: Gleitkomma 0..1 (linear) – für die Anzeige gammakorrigieren
    quint32 bits = (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3];
    float f;
    memcpy(&f, &bits, 4);
    f = std::clamp(f, 0.0f, 1.0f);
    return uchar(std::lround(std::pow(f, 1.0f / 2.2f) * 255.0f));
}

uchar clamp8(double v)
{
    return uchar(std::clamp(int(std::lround(v)), 0, 255));
}

QImage decode(const QByteArray &data)
{
    Reader r(data);
    Header h;
    if (!readHeader(r, h))
        return {};
    const bool psb = h.version == 2;

    const quint64 colorLen = r.be(4);
    const qint64 colorPos = r.pos();
    r.skip(qint64(colorLen));
    r.skip(qint64(r.be(4)));   // Bildressourcen

    // Ebenen-Abschnitt: nur, ob das Gesamtbild einen Alphakanal hat
    // (negative Ebenenzahl).
    bool mergedAlpha = false;
    const quint64 layerLen = r.be(psb ? 8 : 4);
    const qint64 layerEnd = r.pos() + qint64(layerLen);
    if (layerLen > 0) {
        const quint64 infoLen = r.be(psb ? 8 : 4);
        if (infoLen > 0) {
            const qint16 count = qint16(r.be(2));
            mergedAlpha = count < 0;
        }
    }
    r.seek(layerEnd);
    if (!r.ok())
        return {};

    const int compression = int(r.be(2));
    const qint64 w = h.width, ht = h.height;
    const int bytesPerSample = h.depth == 1 ? 0 : h.depth / 8;
    const qint64 rowBytes = h.depth == 1 ? (w + 7) / 8 : w * bytesPerSample;

    // Benötigte Kanäle je Farbmodus
    int needed = 1;
    switch (h.mode) {
    case 3: needed = 3; break;          // RGB
    case 4: needed = 4; break;          // CMYK
    case 9: needed = 3; break;          // Lab
    default: needed = 1; break;         // Bitmap, Grau, Index, Duplex, Mehrkanal
    }
    // Zusätzliche Kanäle sind nur dann Transparenz, wenn Photoshop das für das
    // Gesamtbild vermerkt hat; sonst sind es Masken oder Volltonfarben.
    const bool alpha = h.channels > needed && mergedAlpha && h.mode != 4;
    const int use = std::min(h.channels, needed + (alpha ? 1 : 0));
    if (h.channels < needed)
        return {};

    std::vector<std::vector<uchar>> planes(size_t(use), std::vector<uchar>(size_t(rowBytes * ht)));
    if (compression == 0) {
        for (int c = 0; c < use; ++c) {
            if (r.left() < rowBytes * ht)
                return {};
            std::copy(r.ptr(), r.ptr() + rowBytes * ht, planes[size_t(c)].begin());
            r.skip(rowBytes * ht);
        }
    } else if (compression == 1) {
        const int countBytes = psb ? 4 : 2;
        std::vector<quint32> counts(size_t(h.channels * ht));
        for (auto &n : counts)
            n = quint32(r.be(countBytes));
        if (!r.ok())
            return {};
        for (int c = 0; c < h.channels; ++c) {
            for (qint64 y = 0; y < ht; ++y) {
                const quint32 n = counts[size_t(c * ht + y)];
                if (r.left() < n)
                    return {};
                if (c < use && !unpackRow(r.ptr(), n, planes[size_t(c)].data() + y * rowBytes, rowBytes))
                    return {};
                r.skip(n);
            }
        }
    } else {
        return {};   // ZIP-Kompression kommt beim Gesamtbild praktisch nicht vor
    }

    QImage img(int(w), int(ht), alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    if (img.isNull())
        return {};

    // Palette für indizierte Bilder (planar: 256 Rot, 256 Grün, 256 Blau)
    const uchar *palette = colorLen >= 768 ? reinterpret_cast<const uchar *>(data.constData()) + colorPos : nullptr;

    for (qint64 y = 0; y < ht; ++y) {
        QRgb *out = reinterpret_cast<QRgb *>(img.scanLine(int(y)));
        auto sample = [&](int c, qint64 x) -> uchar {
            const uchar *row = planes[size_t(c)].data() + y * rowBytes;
            if (h.depth == 1)
                return (row[x / 8] & (0x80 >> (x % 8))) ? 0 : 255;   // 1 = schwarz
            return to8(row + x * bytesPerSample, h.depth);
        };
        for (qint64 x = 0; x < w; ++x) {
            int r8 = 0, g8 = 0, b8 = 0;
            switch (h.mode) {
            case 3:
                r8 = sample(0, x); g8 = sample(1, x); b8 = sample(2, x);
                break;
            case 4: {   // CMYK, gespeichert invertiert (255 = keine Farbe)
                const int c = sample(0, x), m = sample(1, x), ye = sample(2, x), k = sample(3, x);
                r8 = c * k / 255; g8 = m * k / 255; b8 = ye * k / 255;
                break;
            }
            case 2: {
                const uchar i = planes[0][size_t(y * rowBytes + x)];
                if (palette) {
                    r8 = palette[i]; g8 = palette[256 + i]; b8 = palette[512 + i];
                } else {
                    r8 = g8 = b8 = i;
                }
                break;
            }
            case 9: {   // Lab → sRGB (D65)
                const double L = sample(0, x) * 100.0 / 255.0;
                const double a = sample(1, x) - 128.0, b = sample(2, x) - 128.0;
                const double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
                auto finv = [](double t) { return t > 0.206893 ? t * t * t : (t - 16.0 / 116) / 7.787; };
                const double X = 0.95047 * finv(fx), Y = finv(fy), Z = 1.08883 * finv(fz);
                auto gamma = [](double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055; };
                r8 = clamp8(255 * gamma(3.2406 * X - 1.5372 * Y - 0.4986 * Z));
                g8 = clamp8(255 * gamma(-0.9689 * X + 1.8758 * Y + 0.0415 * Z));
                b8 = clamp8(255 * gamma(0.0557 * X - 0.2040 * Y + 1.0570 * Z));
                break;
            }
            default:
                r8 = g8 = b8 = sample(0, x);
            }
            const int a8 = alpha ? sample(needed, x) : 255;
            out[x] = qRgba(r8, g8, b8, a8);
        }
    }
    return img;
}

class PsdHandler : public QImageIOHandler
{
public:
    bool canRead() const override
    {
        if (canRead(device())) {
            setFormat("psd");
            return true;
        }
        return false;
    }

    static bool canRead(QIODevice *device)
    {
        return device && device->peek(4) == "8BPS";
    }

    bool read(QImage *image) override
    {
        const QImage img = decode(device()->readAll());
        if (img.isNull())
            return false;
        *image = img;
        return true;
    }

    QVariant option(ImageOption option) const override
    {
        if (option == Size && device()) {
            const QByteArray head = device()->peek(26);
            Reader r(head);
            Header h;
            if (readHeader(r, h))
                return QSize(int(h.width), int(h.height));
        }
        return {};
    }

    bool supportsOption(ImageOption option) const override { return option == Size; }
};

} // namespace

class psd : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QImageIOHandlerFactoryInterface_iid FILE "psd.json")

public:
    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override
    {
        if (format == "psd" || format == "psb")
            return CanRead;
        if (!format.isEmpty())
            return {};
        return PsdHandler::canRead(device) ? CanRead : Capabilities();
    }

    QImageIOHandler *create(QIODevice *device, const QByteArray &format) const override
    {
        auto *h = new PsdHandler;
        h->setDevice(device);
        h->setFormat(format);
        return h;
    }
};

#include "PsdPlugin.moc"

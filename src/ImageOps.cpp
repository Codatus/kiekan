// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageOps.h"

#include <cmath>

namespace ImageOps {

namespace {

inline uint8_t clamp8(float v)
{
    return uint8_t(v <= 0.f ? 0 : v >= 255.f ? 255 : int(v + 0.5f));
}

// Neues Bild gleicher Größe/Format. Zeiger werden vor dem Parallelteil
// geholt, damit kein Thread ein implizit geteiltes Bild abkoppeln muss.
struct Planes
{
    const uchar *src;
    uchar *dst;
    qsizetype srcBpl;
    qsizetype dstBpl;
};

Planes planes(const QImage &in, QImage &out)
{
    return {in.constBits(), out.bits(), in.bytesPerLine(), out.bytesPerLine()};
}

template<typename PixelFn>
QImage mapPixels(const QImage &image, PixelFn fn)
{
    QImage out(image.size(), image.format());
    const Planes p = planes(image, out);
    const int w = image.width();
    parallelRows(image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(p.src + y * p.srcBpl);
            QRgb *d = reinterpret_cast<QRgb *>(p.dst + y * p.dstBpl);
            for (int x = 0; x < w; ++x)
                d[x] = fn(s[x]);
        }
    });
    return out;
}

// Mittelwert ohne Division: acc * (2^16 / n) >> 16. Divisionen pro Pixel
// und Kanal wären hier der mit Abstand teuerste Teil.
inline int reciprocal(int n)
{
    return (65536 + n / 2) / n;
}

inline uint8_t average(int acc, int inv)
{
    return uint8_t(std::min(255, (acc * inv + 32768) >> 16));
}

// Gleitender Mittelwert über 2r+1 Pixel, waagerecht.
void boxH(const uchar *src, uchar *dst, int w, int h, qsizetype bpl, int r)
{
    parallelRows(h, [=](int y0, int y1) {
        const int inv = reciprocal(2 * r + 1);
        for (int y = y0; y < y1; ++y) {
            const uchar *s = src + y * bpl;
            uchar *d = dst + y * bpl;
            int acc[4] = {0, 0, 0, 0};
            for (int i = -r; i <= r; ++i) {
                const int xi = std::clamp(i, 0, w - 1) * 4;
                for (int c = 0; c < 4; ++c)
                    acc[c] += s[xi + c];
            }
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 4; ++c)
                    d[x * 4 + c] = average(acc[c], inv);
                const int add = std::min(x + r + 1, w - 1) * 4;
                const int rem = std::max(x - r, 0) * 4;
                for (int c = 0; c < 4; ++c)
                    acc[c] += s[add + c] - s[rem + c];
            }
        }
    });
}

// Senkrecht; läuft zeilenweise mit einem Akkumulator pro Spalte, damit der
// Speicher in Leserichtung durchlaufen wird.
void boxV(const uchar *src, uchar *dst, int w, int h, qsizetype bpl, int r)
{
    parallelRows(w, [=](int x0, int x1) {
        const int inv = reciprocal(2 * r + 1);
        const int n = (x1 - x0) * 4;
        std::vector<int> acc(n, 0);
        for (int i = -r; i <= r; ++i) {
            const uchar *s = src + std::clamp(i, 0, h - 1) * bpl + x0 * 4;
            for (int k = 0; k < n; ++k)
                acc[k] += s[k];
        }
        for (int y = 0; y < h; ++y) {
            uchar *d = dst + y * bpl + x0 * 4;
            for (int k = 0; k < n; ++k)
                d[k] = average(acc[k], inv);
            const uchar *add = src + std::min(y + r + 1, h - 1) * bpl + x0 * 4;
            const uchar *rem = src + std::max(y - r, 0) * bpl + x0 * 4;
            for (int k = 0; k < n; ++k)
                acc[k] += add[k] - rem[k];
        }
    });
}

// Drei Boxfilter nähern eine Gaußsche Unschärfe an (Kovesi).
std::array<int, 3> boxesForGauss(double sigma)
{
    const int n = 3;
    const double wIdeal = std::sqrt(12.0 * sigma * sigma / n + 1);
    int wl = int(std::floor(wIdeal));
    if (wl % 2 == 0)
        --wl;
    const int wu = wl + 2;
    const double mIdeal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4);
    const int m = int(std::lround(mIdeal));
    std::array<int, 3> sizes{};
    for (int i = 0; i < n; ++i)
        sizes[i] = i < m ? wl : wu;
    return sizes;
}

} // namespace

QImage forProcessing(const QImage &image)
{
    const QImage::Format f = image.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32;
    return image.format() == f ? image : image.convertToFormat(f);
}

QImage forDisplay(const QImage &image)
{
    const QImage::Format f = image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                     : QImage::Format_RGB32;
    return image.format() == f ? image : image.convertToFormat(f);
}

Histogram histogram(const QImage &input)
{
    const QImage image = forProcessing(input);
    Histogram h;
    // Bei sehr großen Bildern genügt eine Stichprobe für die Darstellung.
    const qint64 total = qint64(image.width()) * image.height();
    const int step = total > 8'000'000 ? 2 : 1;
    for (int y = 0; y < image.height(); y += step) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += step) {
            const QRgb px = row[x];
            if (qAlpha(px) == 0)
                continue;
            const int r = qRed(px), g = qGreen(px), b = qBlue(px);
            ++h.red[r];
            ++h.green[g];
            ++h.blue[b];
            ++h.luma[luma(r, g, b)];
            ++h.pixels;
        }
    }
    return h;
}

Lut identityLut()
{
    Lut l{};
    for (int i = 0; i < 256; ++i)
        l[i] = uint8_t(i);
    return l;
}

// Monotone kubische Interpolation (Fritsch-Carlson): glatt, ohne Überschwinger.
Lut curveLut(const QVector<QPointF> &pts)
{
    Lut lut{};
    const int n = int(pts.size());
    if (n == 0)
        return identityLut();

    std::vector<double> d(std::max(0, n - 1)), m(n, 0.0);
    for (int k = 0; k < n - 1; ++k)
        d[k] = (pts[k + 1].y() - pts[k].y()) / std::max(1e-9, pts[k + 1].x() - pts[k].x());
    if (n >= 2) {
        m[0] = d[0];
        m[n - 1] = d[n - 2];
        for (int k = 1; k < n - 1; ++k)
            m[k] = (d[k - 1] * d[k] <= 0) ? 0.0 : (d[k - 1] + d[k]) / 2.0;
        for (int k = 0; k < n - 1; ++k) {
            if (d[k] == 0) {
                m[k] = m[k + 1] = 0;
                continue;
            }
            const double a = m[k] / d[k], b = m[k + 1] / d[k];
            const double s = a * a + b * b;
            if (s > 9) {
                const double t = 3.0 / std::sqrt(s);
                m[k] = t * a * d[k];
                m[k + 1] = t * b * d[k];
            }
        }
    }

    int seg = 0;
    for (int x = 0; x < 256; ++x) {
        double y;
        if (n == 1 || x <= pts.first().x()) {
            y = pts.first().y();
        } else if (x >= pts.last().x()) {
            y = pts.last().y();
        } else {
            while (seg < n - 2 && x > pts[seg + 1].x())
                ++seg;
            const double x0 = pts[seg].x(), x1 = pts[seg + 1].x();
            const double h = x1 - x0;
            const double t = (x - x0) / h;
            const double t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * pts[seg].y() + (t3 - 2 * t2 + t) * h * m[seg]
                + (-2 * t3 + 3 * t2) * pts[seg + 1].y() + (t3 - t2) * h * m[seg + 1];
        }
        lut[x] = uint8_t(std::clamp(int(std::lround(y)), 0, 255));
    }
    return lut;
}

QImage applyLuts(const QImage &image, const Lut &r, const Lut &g, const Lut &b)
{
    return mapPixels(image, [&](QRgb px) {
        return qRgba(r[qRed(px)], g[qGreen(px)], b[qBlue(px)], qAlpha(px));
    });
}

Lut equalizationLut(const Histogram &h)
{
    Lut lut = identityLut();
    if (h.pixels == 0)
        return lut;
    uint64_t cdf = 0, cdfMin = 0;
    for (int i = 0; i < 256; ++i) {
        if (h.luma[i]) {
            cdfMin = h.luma[i];
            break;
        }
    }
    if (h.pixels == cdfMin)
        return lut;   // einfarbiges Bild
    for (int i = 0; i < 256; ++i) {
        cdf += h.luma[i];
        const double v = double(cdf > cdfMin ? cdf - cdfMin : 0) * 255.0 / double(h.pixels - cdfMin);
        lut[i] = clamp8(float(v));
    }
    return lut;
}

void autoLevelsLuts(const Histogram &h, double clip, Lut &r, Lut &g, Lut &b)
{
    auto stretch = [&](const std::array<uint32_t, 256> &hist, Lut &lut) {
        const uint64_t limit = uint64_t(h.pixels * clip);
        uint64_t acc = 0;
        int lo = 0, hi = 255;
        for (int i = 0; i < 256; ++i) {
            acc += hist[i];
            if (acc > limit) { lo = i; break; }
        }
        acc = 0;
        for (int i = 255; i >= 0; --i) {
            acc += hist[i];
            if (acc > limit) { hi = i; break; }
        }
        if (hi <= lo) {
            lut = identityLut();
            return;
        }
        for (int i = 0; i < 256; ++i)
            lut[i] = clamp8(float((i - lo) * 255.0 / (hi - lo)));
    };
    stretch(h.red, r);
    stretch(h.green, g);
    stretch(h.blue, b);
}

QImage grayscale(const QImage &image)
{
    return mapPixels(image, [](QRgb px) {
        const int l = luma(qRed(px), qGreen(px), qBlue(px));
        return qRgba(l, l, l, qAlpha(px));
    });
}

QImage invert(const QImage &image)
{
    return mapPixels(image, [](QRgb px) { return px ^ 0x00ffffffu; });
}

QImage blackWhite(const QImage &image, int threshold, bool dither)
{
    if (!dither) {
        return mapPixels(image, [threshold](QRgb px) {
            const int v = luma(qRed(px), qGreen(px), qBlue(px)) >= threshold ? 255 : 0;
            return qRgba(v, v, v, qAlpha(px));
        });
    }

    // Floyd-Steinberg; zeilenweise abhängig, daher nicht parallel.
    const int w = image.width(), h = image.height();
    QImage out(image.size(), image.format());
    std::vector<int> cur(w + 2, 0), next(w + 2, 0);
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        std::fill(next.begin(), next.end(), 0);
        for (int x = 0; x < w; ++x) {
            const int old = luma(qRed(s[x]), qGreen(s[x]), qBlue(s[x])) + cur[x + 1] / 16;
            const int v = old >= threshold ? 255 : 0;
            const int err = old - v;
            cur[x + 2] += err * 7;
            next[x] += err * 3;
            next[x + 1] += err * 5;
            next[x + 2] += err * 1;
            d[x] = qRgba(v, v, v, qAlpha(s[x]));
        }
        std::swap(cur, next);
    }
    return out;
}

QImage colorMatrix(const QImage &image, const float m[9])
{
    return mapPixels(image, [m](QRgb px) {
        const float r = float(qRed(px)), g = float(qGreen(px)), b = float(qBlue(px));
        return qRgba(clamp8(m[0] * r + m[1] * g + m[2] * b),
                     clamp8(m[3] * r + m[4] * g + m[5] * b),
                     clamp8(m[6] * r + m[7] * g + m[8] * b),
                     qAlpha(px));
    });
}

QImage gaussianBlur(const QImage &input, double sigma)
{
    if (sigma < 0.3 || input.isNull())
        return input;

    // Mit Transparenz im vormultiplizierten Raum weichzeichnen, sonst
    // bluten transparente (schwarze) Pixel als dunkle Ränder ein.
    const bool alpha = input.hasAlphaChannel();
    QImage a = input.convertToFormat(alpha ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    a.detach();
    QImage b(a.size(), a.format());
    const int w = a.width(), h = a.height();
    uchar *pa = a.bits();
    uchar *pb = b.bits();
    const qsizetype bpl = a.bytesPerLine();

    for (int size : boxesForGauss(sigma)) {
        const int r = (size - 1) / 2;
        if (r <= 0)
            continue;
        boxH(pa, pb, w, h, bpl, r);
        boxV(pb, pa, w, h, bpl, r);
    }
    return alpha ? a.convertToFormat(QImage::Format_ARGB32) : a;
}

QImage unsharpMask(const QImage &image, double amount, double sigma, int threshold)
{
    const QImage blurred = gaussianBlur(image, sigma);
    QImage out(image.size(), image.format());
    const uchar *src = image.constBits();
    const uchar *blr = blurred.constBits();
    uchar *dst = out.bits();
    const qsizetype bpl = image.bytesPerLine();
    const qsizetype bplB = blurred.bytesPerLine();
    const qsizetype bplO = out.bytesPerLine();
    const int w = image.width();
    const float k = float(amount);

    parallelRows(image.height(), [=](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(src + y * bpl);
            const QRgb *b = reinterpret_cast<const QRgb *>(blr + y * bplB);
            QRgb *d = reinterpret_cast<QRgb *>(dst + y * bplO);
            for (int x = 0; x < w; ++x) {
                auto sharpen = [&](int sv, int bv) {
                    const int diff = sv - bv;
                    if (std::abs(diff) < threshold)
                        return uint8_t(sv);
                    return clamp8(float(sv) + k * float(diff));
                };
                d[x] = qRgba(sharpen(qRed(s[x]), qRed(b[x])),
                             sharpen(qGreen(s[x]), qGreen(b[x])),
                             sharpen(qBlue(s[x]), qBlue(b[x])),
                             qAlpha(s[x]));
            }
        }
    });
    return out;
}

QImage convolve3x3(const QImage &image, const float kernel[9], float bias, bool gray)
{
    QImage out(image.size(), image.format());
    const uchar *src = image.constBits();
    uchar *dst = out.bits();
    const qsizetype bpl = image.bytesPerLine();
    const qsizetype bplO = out.bytesPerLine();
    const int w = image.width(), h = image.height();

    parallelRows(h, [=](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const QRgb *rows[3];
            for (int i = 0; i < 3; ++i)
                rows[i] = reinterpret_cast<const QRgb *>(src + std::clamp(y + i - 1, 0, h - 1) * bpl);
            QRgb *d = reinterpret_cast<QRgb *>(dst + y * bplO);
            for (int x = 0; x < w; ++x) {
                float r = 0, g = 0, b = 0;
                for (int j = 0; j < 3; ++j) {
                    for (int i = 0; i < 3; ++i) {
                        const QRgb px = rows[j][std::clamp(x + i - 1, 0, w - 1)];
                        const float k = kernel[j * 3 + i];
                        if (gray) {
                            r += k * float(luma(qRed(px), qGreen(px), qBlue(px)));
                        } else {
                            r += k * float(qRed(px));
                            g += k * float(qGreen(px));
                            b += k * float(qBlue(px));
                        }
                    }
                }
                const int a = qAlpha(rows[1][x]);
                if (gray) {
                    const uint8_t v = clamp8(r + bias);
                    d[x] = qRgba(v, v, v, a);
                } else {
                    d[x] = qRgba(clamp8(r + bias), clamp8(g + bias), clamp8(b + bias), a);
                }
            }
        }
    });
    return out;
}

} // namespace ImageOps

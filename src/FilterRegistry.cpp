// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FilterRegistry.h"

#include "ImageOps.h"

#include <QCoreApplication>
#include <QDir>
#include <QPluginLoader>
#include <QStandardPaths>

#include <cmath>
#include <functional>

using Kiekan::Filter;
using Kiekan::FilterParameter;

namespace {

// Übersetzungskontext für die Namen der eingebauten Filter.
struct Filters
{
    Q_DECLARE_TR_FUNCTIONS(Filters)
};

FilterParameter number(const QString &id, const QString &label, double min, double max, double def,
                       int decimals = 0, const QString &suffix = QString(), bool scales = false)
{
    FilterParameter p;
    p.id = id;
    p.label = label;
    p.minimum = min;
    p.maximum = max;
    p.defaultValue = def;
    p.decimals = decimals;
    p.suffix = suffix;
    p.scalesWithImage = scales;
    return p;
}

FilterParameter toggle(const QString &id, const QString &label, bool def)
{
    FilterParameter p;
    p.id = id;
    p.label = label;
    p.type = FilterParameter::Toggle;
    p.minimum = 0;
    p.maximum = 1;
    p.defaultValue = def ? 1 : 0;
    return p;
}

class BuiltinFilter : public Filter
{
public:
    using Fn = std::function<QImage(const QImage &, const QVariantMap &, double)>;

    BuiltinFilter(QString id, QString name, QList<FilterParameter> params, Fn fn)
        : m_id(std::move(id)), m_name(std::move(name)), m_params(std::move(params)), m_fn(std::move(fn))
    {}

    QString id() const override { return m_id; }
    QString name() const override { return m_name; }
    QList<FilterParameter> parameters() const override { return m_params; }
    QImage apply(const QImage &image, const QVariantMap &params, double scale) const override
    {
        return m_fn(image, params, scale);
    }

private:
    QString m_id;
    QString m_name;
    QList<FilterParameter> m_params;
    Fn m_fn;
};

double val(const QVariantMap &p, const char *key)
{
    return p.value(QString::fromLatin1(key)).toDouble();
}

// --- Bildkorrekturen -------------------------------------------------------

QImage adjustColors(const QImage &image, const QVariantMap &p, double)
{
    const double brightness = val(p, "helligkeit") / 200.0;
    const double c = val(p, "kontrast");
    const double contrast = c >= 0 ? 1.0 + c / 50.0 : 1.0 + c / 100.0;
    const double gamma = std::max(0.05, val(p, "gamma"));
    const double offsets[3] = {val(p, "rot") / 200.0, val(p, "gruen") / 200.0, val(p, "blau") / 200.0};

    ImageOps::Lut luts[3];
    for (int ch = 0; ch < 3; ++ch) {
        for (int i = 0; i < 256; ++i) {
            double v = i / 255.0 + brightness;
            v = (v - 0.5) * contrast + 0.5;
            v = std::clamp(v, 0.0, 1.0);
            v = std::pow(v, 1.0 / gamma) + offsets[ch];
            luts[ch][i] = uint8_t(std::clamp(int(std::lround(v * 255.0)), 0, 255));
        }
    }
    QImage out = ImageOps::applyLuts(image, luts[0], luts[1], luts[2]);

    const double sat = 1.0 + val(p, "saettigung") / 100.0;
    const double hue = val(p, "farbton") * M_PI / 180.0;
    if (sat == 1.0 && hue == 0.0)
        return out;

    // Farbton-Drehung (helligkeitserhaltend) und Sättigung als 3×3-Matrix.
    const double cs = std::cos(hue), sn = std::sin(hue);
    const double h[9] = {
        0.213 + cs * 0.787 - sn * 0.213, 0.715 - cs * 0.715 - sn * 0.715, 0.072 - cs * 0.072 + sn * 0.928,
        0.213 - cs * 0.213 + sn * 0.143, 0.715 + cs * 0.285 + sn * 0.140, 0.072 - cs * 0.072 - sn * 0.283,
        0.213 - cs * 0.213 - sn * 0.787, 0.715 - cs * 0.715 + sn * 0.715, 0.072 + cs * 0.928 + sn * 0.072,
    };
    const double lr = 0.2126 * (1 - sat), lg = 0.7152 * (1 - sat), lb = 0.0722 * (1 - sat);
    const double s[9] = {lr + sat, lg, lb, lr, lg + sat, lb, lr, lg, lb + sat};
    float m[9];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            m[r * 3 + k] = float(s[r * 3 + 0] * h[0 * 3 + k] + s[r * 3 + 1] * h[1 * 3 + k] + s[r * 3 + 2] * h[2 * 3 + k]);
    return ImageOps::colorMatrix(out, m);
}

// --- Effekte ---------------------------------------------------------------

QImage sepia(const QImage &image, const QVariantMap &p, double)
{
    const float k = float(val(p, "staerke") / 100.0);
    const float m[9] = {
        (1 - k) + k * 0.393f, k * 0.769f, k * 0.189f,
        k * 0.349f, (1 - k) + k * 0.686f, k * 0.168f,
        k * 0.272f, k * 0.534f, (1 - k) + k * 0.131f,
    };
    return ImageOps::colorMatrix(image, m);
}

QImage posterize(const QImage &image, const QVariantMap &p, double)
{
    const int levels = std::clamp(int(val(p, "stufen")), 2, 256);
    ImageOps::Lut lut{};
    for (int i = 0; i < 256; ++i) {
        const int step = int(std::lround(i * (levels - 1) / 255.0));
        lut[i] = uint8_t(std::lround(step * 255.0 / (levels - 1)));
    }
    return ImageOps::applyLuts(image, lut, lut, lut);
}

QImage pixelate(const QImage &image, const QVariantMap &p, double scale)
{
    const int block = std::max(1, int(std::lround(val(p, "groesse") * scale)));
    if (block <= 1)
        return image;
    QImage out(image.size(), image.format());
    const uchar *src = image.constBits();
    uchar *dst = out.bits();
    const qsizetype bpl = image.bytesPerLine(), bplO = out.bytesPerLine();
    const int w = image.width(), h = image.height();
    const int blockRows = (h + block - 1) / block;

    ImageOps::parallelRows(blockRows, [=](int b0, int b1) {
        for (int by = b0; by < b1; ++by) {
            const int y0 = by * block, y1 = std::min(h, y0 + block);
            for (int x0 = 0; x0 < w; x0 += block) {
                const int x1 = std::min(w, x0 + block);
                uint64_t r = 0, g = 0, b = 0, a = 0;
                for (int y = y0; y < y1; ++y) {
                    const QRgb *row = reinterpret_cast<const QRgb *>(src + y * bpl);
                    for (int x = x0; x < x1; ++x) {
                        r += qRed(row[x]); g += qGreen(row[x]); b += qBlue(row[x]); a += qAlpha(row[x]);
                    }
                }
                const uint64_t n = uint64_t(y1 - y0) * uint64_t(x1 - x0);
                const QRgb avg = qRgba(int(r / n), int(g / n), int(b / n), int(a / n));
                for (int y = y0; y < y1; ++y) {
                    QRgb *row = reinterpret_cast<QRgb *>(dst + y * bplO);
                    std::fill(row + x0, row + x1, avg);
                }
            }
        }
    });
    return out;
}

QImage vignette(const QImage &image, const QVariantMap &p, double)
{
    const float strength = float(val(p, "staerke") / 100.0);
    const float inner = float(val(p, "groesse") / 100.0);
    QImage out(image.size(), image.format());
    const uchar *src = image.constBits();
    uchar *dst = out.bits();
    const qsizetype bpl = image.bytesPerLine(), bplO = out.bytesPerLine();
    const int w = image.width(), h = image.height();
    const float cx = w / 2.f, cy = h / 2.f;
    const float radius = std::sqrt(cx * cx + cy * cy);

    ImageOps::parallelRows(h, [=](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(src + y * bpl);
            QRgb *d = reinterpret_cast<QRgb *>(dst + y * bplO);
            const float dy = (y - cy) / radius;
            for (int x = 0; x < w; ++x) {
                const float dx = (x - cx) / radius;
                const float dist = std::sqrt(dx * dx + dy * dy);
                float t = inner >= 1.f ? 0.f : std::clamp((dist - inner) / (1.f - inner), 0.f, 1.f);
                t = t * t * (3 - 2 * t);   // weicher Übergang
                const float f = 1.f - strength * t;
                d[x] = qRgba(int(qRed(s[x]) * f), int(qGreen(s[x]) * f), int(qBlue(s[x]) * f), qAlpha(s[x]));
            }
        }
    });
    return out;
}

QImage noise(const QImage &image, const QVariantMap &p, double)
{
    const float amount = float(val(p, "menge") * 1.28);
    const bool color = val(p, "farbig") > 0.5;
    QImage out(image.size(), image.format());
    const uchar *src = image.constBits();
    uchar *dst = out.bits();
    const qsizetype bpl = image.bytesPerLine(), bplO = out.bytesPerLine();
    const int w = image.width();

    ImageOps::parallelRows(image.height(), [=](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            // Pro Zeile gleich initialisiert, damit das Ergebnis nicht von
            // der Thread-Aufteilung abhängt.
            uint32_t state = 0x9e3779b9u ^ uint32_t(y * 2654435761u);
            auto rnd = [&state] {
                state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                return (state & 0xffff) / 32767.5f - 1.f;
            };
            const QRgb *s = reinterpret_cast<const QRgb *>(src + y * bpl);
            QRgb *d = reinterpret_cast<QRgb *>(dst + y * bplO);
            for (int x = 0; x < w; ++x) {
                const float n = rnd() * amount;
                const float nr = n, ng = color ? rnd() * amount : n, nb = color ? rnd() * amount : n;
                d[x] = qRgba(std::clamp(int(qRed(s[x]) + nr), 0, 255),
                             std::clamp(int(qGreen(s[x]) + ng), 0, 255),
                             std::clamp(int(qBlue(s[x]) + nb), 0, 255),
                             qAlpha(s[x]));
            }
        }
    });
    return out;
}

} // namespace

FilterRegistry &FilterRegistry::instance()
{
    static FilterRegistry registry;
    return registry;
}

FilterRegistry::FilterRegistry()
{
    auto add = [this](QList<const Filter *> &list, QString id, QString name,
                      QList<FilterParameter> params, BuiltinFilter::Fn fn) {
        m_owned.push_back(std::make_unique<BuiltinFilter>(std::move(id), std::move(name),
                                                          std::move(params), std::move(fn)));
        list.append(m_owned.back().get());
    };
    const QString px = QStringLiteral(" px");
    const QString pct = QStringLiteral(" %");

    add(m_adjustments, QStringLiteral("farben"), Filters::tr("Farben anpassen"),
        {number(QStringLiteral("helligkeit"), Filters::tr("Helligkeit"), -100, 100, 0),
         number(QStringLiteral("kontrast"), Filters::tr("Kontrast"), -100, 100, 0),
         number(QStringLiteral("gamma"), Filters::tr("Gamma"), 0.1, 5.0, 1.0, 2),
         number(QStringLiteral("saettigung"), Filters::tr("Sättigung"), -100, 100, 0),
         number(QStringLiteral("farbton"), Filters::tr("Farbton"), -180, 180, 0, 0, QStringLiteral(" °")),
         number(QStringLiteral("rot"), Filters::tr("Rot"), -100, 100, 0),
         number(QStringLiteral("gruen"), Filters::tr("Grün"), -100, 100, 0),
         number(QStringLiteral("blau"), Filters::tr("Blau"), -100, 100, 0)},
        adjustColors);

    add(m_adjustments, QStringLiteral("schaerfen"), Filters::tr("Schärfen"),
        {number(QStringLiteral("staerke"), Filters::tr("Stärke"), 0, 500, 80, 0, pct),
         number(QStringLiteral("radius"), Filters::tr("Radius"), 0.3, 20, 1.2, 1, px, true),
         number(QStringLiteral("schwelle"), Filters::tr("Schwelle"), 0, 50, 2)},
        [](const QImage &img, const QVariantMap &p, double scale) {
            return ImageOps::unsharpMask(img, val(p, "staerke") / 100.0,
                                         std::max(0.3, val(p, "radius") * scale), int(val(p, "schwelle")));
        });

    add(m_adjustments, QStringLiteral("weichzeichnen"), Filters::tr("Weichzeichnen"),
        {number(QStringLiteral("radius"), Filters::tr("Radius"), 0.3, 50, 2, 1, px, true)},
        [](const QImage &img, const QVariantMap &p, double scale) {
            return ImageOps::gaussianBlur(img, val(p, "radius") * scale);
        });

    add(m_adjustments, QStringLiteral("schwarzweiss"), Filters::tr("Schwarzweiß"),
        {number(QStringLiteral("schwelle"), Filters::tr("Schwelle"), 0, 255, 128),
         toggle(QStringLiteral("rasterung"), Filters::tr("Rasterung (Fehlerverteilung)"), false)},
        [](const QImage &img, const QVariantMap &p, double) {
            return ImageOps::blackWhite(img, int(val(p, "schwelle")), val(p, "rasterung") > 0.5);
        });

    add(m_effects, QStringLiteral("negativ"), Filters::tr("Negativ"), {},
        [](const QImage &img, const QVariantMap &, double) { return ImageOps::invert(img); });

    add(m_effects, QStringLiteral("sepia"), Filters::tr("Sepia"),
        {number(QStringLiteral("staerke"), Filters::tr("Stärke"), 0, 100, 100, 0, pct)}, sepia);

    add(m_effects, QStringLiteral("relief"), Filters::tr("Relief"),
        {number(QStringLiteral("staerke"), Filters::tr("Stärke"), 0.2, 5, 1, 1)},
        [](const QImage &img, const QVariantMap &p, double) {
            const float s = float(val(p, "staerke"));
            const float k[9] = {-s, -s, 0, -s, 0, s, 0, s, s};
            return ImageOps::convolve3x3(img, k, 128.f, true);
        });

    add(m_effects, QStringLiteral("kanten"), Filters::tr("Kanten finden"),
        {number(QStringLiteral("staerke"), Filters::tr("Stärke"), 0.2, 5, 1, 1),
         toggle(QStringLiteral("zeichnung"), Filters::tr("Als Zeichnung (hell)"), true)},
        [](const QImage &img, const QVariantMap &p, double) {
            const float s = float(val(p, "staerke"));
            const float k[9] = {-s, -s, -s, -s, 8 * s, -s, -s, -s, -s};
            QImage out = ImageOps::convolve3x3(img, k, 0.f, true);
            return val(p, "zeichnung") > 0.5 ? ImageOps::invert(out) : out;
        });

    add(m_effects, QStringLiteral("posterisieren"), Filters::tr("Posterisieren"),
        {number(QStringLiteral("stufen"), Filters::tr("Farbstufen"), 2, 32, 6)}, posterize);

    add(m_effects, QStringLiteral("verpixeln"), Filters::tr("Verpixeln"),
        {number(QStringLiteral("groesse"), Filters::tr("Blockgröße"), 2, 200, 12, 0, px, true)}, pixelate);

    add(m_effects, QStringLiteral("vignette"), Filters::tr("Vignette"),
        {number(QStringLiteral("staerke"), Filters::tr("Stärke"), 0, 100, 60, 0, pct),
         number(QStringLiteral("groesse"), Filters::tr("Heller Bereich"), 0, 100, 50, 0, pct)},
        vignette);

    add(m_effects, QStringLiteral("rauschen"), Filters::tr("Rauschen hinzufügen"),
        {number(QStringLiteral("menge"), Filters::tr("Menge"), 0, 100, 20),
         toggle(QStringLiteral("farbig"), Filters::tr("Farbiges Rauschen"), false)},
        noise);
}

QStringList FilterRegistry::pluginDirectories()
{
    QStringList dirs;
    dirs << QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    dirs << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/kiekan/plugins");
#ifdef KIEKAN_PLUGIN_RELDIR
    dirs << QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/" KIEKAN_PLUGIN_RELDIR));
#endif
    dirs.removeDuplicates();
    return dirs;
}

void FilterRegistry::loadPlugins()
{
    if (m_pluginsLoaded)
        return;
    m_pluginsLoaded = true;

    for (const QString &dirPath : pluginDirectories()) {
        const QDir dir(dirPath);
        if (!dir.exists())
            continue;
        for (const QString &name : dir.entryList(QDir::Files)) {
            const QString path = dir.absoluteFilePath(name);
            if (!QLibrary::isLibrary(path))
                continue;
            QPluginLoader loader(path);
            QObject *root = loader.instance();
            auto *plugin = qobject_cast<Kiekan::FilterPlugin *>(root);
            if (!plugin) {
                m_pluginErrors << QStringLiteral("%1: %2").arg(name,
                    root ? Filters::tr("kein Kiek-an-Filter-Plugin") : loader.errorString());
                if (!root)
                    loader.unload();
                continue;
            }
            // Plugin bleibt geladen; seine Filter leben bis Programmende.
            m_pluginFiles << path;
            for (Filter *f : plugin->filters()) {
                if (f && !find(f->id()))
                    m_effects.append(f);
            }
        }
    }
}

const Filter *FilterRegistry::find(const QString &id) const
{
    for (const Filter *f : m_adjustments)
        if (f->id() == id)
            return f;
    for (const Filter *f : m_effects)
        if (f->id() == id)
            return f;
    return nullptr;
}

QVariantMap FilterRegistry::defaults(const Filter *filter)
{
    QVariantMap map;
    for (const FilterParameter &p : filter->parameters())
        map.insert(p.id, p.defaultValue);
    return map;
}

QImage FilterRegistry::run(const Filter *filter, const QImage &image, const QVariantMap &params, double scale)
{
    if (!filter || image.isNull())
        return image;
    QVariantMap full = defaults(filter);
    for (auto it = params.begin(); it != params.end(); ++it)
        full.insert(it.key(), it.value());
    const QImage result = filter->apply(ImageOps::forProcessing(image), full, scale);
    return result.isNull() ? image : ImageOps::forDisplay(result);
}

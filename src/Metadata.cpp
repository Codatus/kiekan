// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Metadata.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#ifdef KIEKAN_HAVE_EXIV2
#include <exiv2/exiv2.hpp>
#endif
#ifdef KIEKAN_HAVE_HEIF
#include <libheif/heif.h>
#endif
#ifdef KIEKAN_HAVE_LIBRAW
#include <libraw/libraw.h>
#endif

#include <cmath>
#include <stdexcept>

namespace Metadata {

#ifdef KIEKAN_HAVE_EXIV2

namespace {

// Dateien liest und schreibt Qt; exiv2 arbeitet nur im Speicher. Das umgeht
// Unterschiede bei Dateinamen (Windows: Umlaute, UNC-Pfade) und zwischen
// exiv2 0.27 und 0.28, die Pfade unterschiedlich behandeln.
struct OpenImage
{
    QByteArray data;   // muss so lange leben wie das Bildobjekt
    decltype(Exiv2::ImageFactory::open(static_cast<const Exiv2::byte *>(nullptr), 0)) image;
};

// Füllt `o` (exiv2 0.27 verwendet std::auto_ptr, das sich nicht verschieben lässt).
void openImage(OpenImage &o, const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(("cannot open " + path).toStdString());
    o.data = f.readAll();
    o.image = Exiv2::ImageFactory::open(reinterpret_cast<const Exiv2::byte *>(o.data.constData()), o.data.size());
    o.image->readMetadata();
}

// Nach writeMetadata(): Ergebnis aus dem Speicher sicher (atomar) in die Datei.
void saveImage(OpenImage &o, const QString &path)
{
    o.image->writeMetadata();
    Exiv2::BasicIo &io = o.image->io();
    const auto size = io.size();
    const Exiv2::byte *bytes = io.mmap();
    const QByteArray out(reinterpret_cast<const char *>(bytes), qsizetype(size));
    io.munmap();
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(out) != out.size() || !f.commit())
        throw std::runtime_error(("cannot write " + path).toStdString());
}

// exiv2 0.28 hat toLong() durch toInt64() ersetzt.
template<typename Datum>
long long toInteger(const Datum &d)
{
#if EXIV2_TEST_VERSION(0, 28, 0)
    return d.toInt64();
#else
    return d.toLong();
#endif
}

QString str(const std::string &s)
{
    return QString::fromStdString(s).trimmed();
}

QString printed(const Exiv2::ExifData &exif, const char *key)
{
    const auto it = exif.findKey(Exiv2::ExifKey(key));
    if (it == exif.end())
        return {};
    return str(it->print(&exif));
}

// GPS-Koordinate aus Grad/Minuten/Sekunden (drei Brüche) und Richtung.
bool gpsValue(const Exiv2::ExifData &exif, const char *key, const char *refKey, double *out)
{
    const auto it = exif.findKey(Exiv2::ExifKey(key));
    if (it == exif.end() || it->count() < 3)
        return false;
    double v = 0;
    const double div[3] = {1, 60, 3600};
    for (int i = 0; i < 3; ++i) {
        const Exiv2::Rational r = it->toRational(i);
        if (r.second == 0)
            return false;
        v += double(r.first) / double(r.second) / div[i];
    }
    const QString ref = printed(exif, refKey).toUpper();
    if (ref.startsWith(QLatin1Char('S')) || ref.startsWith(QLatin1Char('W')))
        v = -v;
    *out = v;
    return true;
}

QString langAlt(const Exiv2::XmpData &xmp, const char *key)
{
    const auto it = xmp.findKey(Exiv2::XmpKey(key));
    if (it == xmp.end())
        return {};
    if (it->typeId() == Exiv2::langAlt)
        return str(static_cast<const Exiv2::LangAltValue &>(it->value()).toString("x-default"));
    return str(it->toString());
}

void eraseKey(Exiv2::XmpData &xmp, const char *key)
{
    for (auto it = xmp.begin(); it != xmp.end();) {
        if (it->key() == key)
            it = xmp.erase(it);
        else
            ++it;
    }
}

void eraseKey(Exiv2::IptcData &iptc, const char *key)
{
    for (auto it = iptc.begin(); it != iptc.end();) {
        if (it->key() == key)
            it = iptc.erase(it);
        else
            ++it;
    }
}

#ifdef KIEKAN_HAVE_HEIF
// HEIC/AVIF: EXIF-Block aus dem HEIF-Container holen (exiv2 0.27 kennt das
// Format ohne BMFF-Unterstützung nicht).
bool heifExif(const QString &path, Exiv2::ExifData &exif)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = f.readAll();
    heif_context *ctx = heif_context_alloc();
    bool ok = false;
    if (heif_context_read_from_memory_without_copy(ctx, data.constData(), size_t(data.size()), nullptr).code == heif_error_Ok) {
        heif_image_handle *handle = nullptr;
        if (heif_context_get_primary_image_handle(ctx, &handle).code == heif_error_Ok) {
            heif_item_id id;
            if (heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &id, 1) == 1) {
                QByteArray block(int(heif_image_handle_get_metadata_size(handle, id)), Qt::Uninitialized);
                if (heif_image_handle_get_metadata(handle, id, block.data()).code == heif_error_Ok && block.size() > 4) {
                    // Die ersten 4 Byte geben den Abstand zum TIFF-Kopf an.
                    const quint32 offset = (quint32(uchar(block[0])) << 24) | (quint32(uchar(block[1])) << 16)
                                         | (quint32(uchar(block[2])) << 8) | quint32(uchar(block[3]));
                    const qsizetype start = 4 + qsizetype(offset);
                    if (start < block.size()) {
                        Exiv2::ExifParser::decode(exif, reinterpret_cast<const Exiv2::byte *>(block.constData() + start),
                                                  uint32_t(block.size() - start));
                        ok = !exif.empty();
                    }
                }
            }
            heif_image_handle_release(handle);
        }
    }
    heif_context_free(ctx);
    return ok;
}
#endif

#ifdef KIEKAN_HAVE_LIBRAW
// RAW-Formate, die exiv2 nicht kennt (z. B. Canon CR3 bei exiv2 0.27):
// Aufnahmedaten aus LibRaw als EXIF nachbilden.
bool rawExif(const QString &path, Exiv2::ExifData &exif)
{
    auto raw = std::make_unique<LibRaw>();
#if defined(Q_OS_WIN) && defined(LIBRAW_WIN32_UNICODEPATHS)
    if (raw->open_file(reinterpret_cast<const wchar_t *>(path.utf16())) != LIBRAW_SUCCESS)
#else
    if (raw->open_file(QFile::encodeName(path).constData()) != LIBRAW_SUCCESS)
#endif
        return false;
    const auto &d = raw->imgdata;
    // EXIF speichert diese Werte als vorzeichenlose Brüche (sonst gibt exiv2
    // sie nicht lesbar aus).
    auto rational = [](double v, uint32_t den) { return Exiv2::URational(uint32_t(std::lround(v * den)), den); };
    if (d.idata.make[0])
        exif["Exif.Image.Make"] = std::string(d.idata.make);
    if (d.idata.model[0])
        exif["Exif.Image.Model"] = std::string(d.idata.model);
    if (d.lens.Lens[0])
        exif["Exif.Photo.LensModel"] = std::string(d.lens.Lens);
    if (d.other.iso_speed > 0)
        exif["Exif.Photo.ISOSpeedRatings"] = uint16_t(std::lround(d.other.iso_speed));
    if (d.other.shutter > 0) {
        const double t = d.other.shutter;
        const double whole = std::round(t);
        exif["Exif.Photo.ExposureTime"] = t < 1 ? Exiv2::URational(1, uint32_t(std::lround(1 / t)))
                                        : std::abs(t - whole) < 0.05 ? Exiv2::URational(uint32_t(whole), 1)
                                                                      : rational(t, 10);
    }
    if (d.other.aperture > 0)
        exif["Exif.Photo.FNumber"] = rational(d.other.aperture, 10);
    if (d.other.focal_len > 0)
        exif["Exif.Photo.FocalLength"] = rational(d.other.focal_len, 10);
    if (d.other.timestamp > 0)
        exif["Exif.Photo.DateTimeOriginal"] = QDateTime::fromSecsSinceEpoch(d.other.timestamp)
                                                  .toString(QStringLiteral("yyyy:MM:dd HH:mm:ss")).toStdString();
    const auto &gps = d.other.parsed_gps;
    const bool gpsSet = gps.gpsparsed && (gps.latitude[0] != 0 || gps.latitude[1] != 0 || gps.longitude[0] != 0 || gps.longitude[1] != 0);
    if (gpsSet) {
        auto dms = [](const float v[3]) {
            Exiv2::URationalValue val;
            for (int i = 0; i < 3; ++i)
                val.value_.push_back({uint32_t(std::lround(v[i] * 100)), 100});
            return val;
        };
        exif["Exif.GPSInfo.GPSLatitude"] = dms(d.other.parsed_gps.latitude);
        exif["Exif.GPSInfo.GPSLatitudeRef"] = std::string(1, d.other.parsed_gps.latref ? d.other.parsed_gps.latref : 'N');
        exif["Exif.GPSInfo.GPSLongitude"] = dms(d.other.parsed_gps.longitude);
        exif["Exif.GPSInfo.GPSLongitudeRef"] = std::string(1, d.other.parsed_gps.longref ? d.other.parsed_gps.longref : 'E');
    }
    return !exif.empty();
}
#endif

// EXIF auf anderem Weg, wenn exiv2 die Datei nicht (vollständig) lesen kann.
bool fallbackExif(const QString &path, Exiv2::ExifData &exif)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
#ifdef KIEKAN_HAVE_HEIF
    if (suffix == QLatin1String("heic") || suffix == QLatin1String("heif") || suffix == QLatin1String("hif")
        || suffix == QLatin1String("avif"))
        return heifExif(path, exif);
#endif
#ifdef KIEKAN_HAVE_LIBRAW
    static const QStringList rawSuffixes = {
        QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("crw"), QStringLiteral("nef"), QStringLiteral("nrw"),
        QStringLiteral("arw"), QStringLiteral("srf"), QStringLiteral("sr2"), QStringLiteral("dng"), QStringLiteral("orf"),
        QStringLiteral("rw2"), QStringLiteral("raf"), QStringLiteral("pef"), QStringLiteral("srw"), QStringLiteral("x3f"),
        QStringLiteral("3fr"), QStringLiteral("mef"), QStringLiteral("mos"), QStringLiteral("mrw"), QStringLiteral("erf"),
        QStringLiteral("kdc"), QStringLiteral("dcr"), QStringLiteral("rwl"), QStringLiteral("iiq")};
    if (rawSuffixes.contains(suffix))
        return rawExif(path, exif);
#endif
    Q_UNUSED(exif);
    Q_UNUSED(suffix);
    return false;
}

// exiv2 öffnen, wenn möglich; EXIF notfalls aus der Ersatzquelle.
struct Loaded
{
    std::unique_ptr<OpenImage> opened;
    Exiv2::ExifData fallback;
    const Exiv2::ExifData *exif = nullptr;
};

Loaded load(const QString &path)
{
    Loaded l;
    try {
        l.opened = std::make_unique<OpenImage>();
        openImage(*l.opened, path);
        l.exif = &l.opened->image->exifData();
    } catch (const std::exception &) {
        l.opened.reset();
    }
    if ((!l.exif || l.exif->empty()) && fallbackExif(path, l.fallback))
        l.exif = &l.fallback;
    return l;
}

} // namespace

bool available() { return true; }

void initialize()
{
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);
    Exiv2::XmpParser::initialize();
}

QList<Entry> read(const QString &path, QString *error)
{
    QList<Entry> list;
    try {
        Loaded l = load(path);
        if (!l.exif && !l.opened)
            throw std::runtime_error("unsupported file format");
        if (l.exif) {
            const Exiv2::ExifData &exif = *l.exif;
            for (const auto &d : exif) {
                if (d.size() > 512)
                    continue;   // Herstellerblöcke und Vorschaubilder überspringen
                list.append({QStringLiteral("EXIF"), str(d.key()), str(d.tagLabel()), str(d.print(&exif))});
            }
        }
        if (l.opened) {
            for (const auto &d : l.opened->image->iptcData())
                list.append({QStringLiteral("IPTC"), str(d.key()), str(d.tagLabel()), str(d.print())});
            for (const auto &d : l.opened->image->xmpData())
                list.append({QStringLiteral("XMP"), str(d.key()), str(d.tagLabel()), str(d.print())});
        }
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromLocal8Bit(e.what());
    }
    return list;
}

Summary summary(const QString &path)
{
    Summary s;
    try {
        Loaded l = load(path);
        static const Exiv2::ExifData none;
        const Exiv2::ExifData &exif = l.exif ? *l.exif : none;

        const QString make = printed(exif, "Exif.Image.Make");
        const QString model = printed(exif, "Exif.Image.Model");
        s.camera = model.startsWith(make, Qt::CaseInsensitive) ? model : (make + QLatin1Char(' ') + model).trimmed();
        s.lens = printed(exif, "Exif.Photo.LensModel");
        s.exposure = printed(exif, "Exif.Photo.ExposureTime");
        s.aperture = printed(exif, "Exif.Photo.FNumber");
        s.iso = printed(exif, "Exif.Photo.ISOSpeedRatings");
        s.focalLength = printed(exif, "Exif.Photo.FocalLength");
        const QString focal35 = printed(exif, "Exif.Photo.FocalLengthIn35mmFilm");
        if (!focal35.isEmpty() && focal35 != s.focalLength)
            s.focalLength += QStringLiteral(" (KB: %1)").arg(focal35);
        s.dateTaken = printed(exif, "Exif.Photo.DateTimeOriginal");
        if (s.dateTaken.isEmpty())
            s.dateTaken = printed(exif, "Exif.Image.DateTime");

        // Beschreibende Angaben: XMP bevorzugt, sonst IPTC/EXIF
        static const Exiv2::XmpData noXmp;
        static const Exiv2::IptcData noIptc;
        const Exiv2::XmpData &xmp = l.opened ? l.opened->image->xmpData() : noXmp;
        const Exiv2::IptcData &iptc = l.opened ? l.opened->image->iptcData() : noIptc;
        s.title = langAlt(xmp, "Xmp.dc.title");
        s.description = langAlt(xmp, "Xmp.dc.description");
        if (s.description.isEmpty()) {
            const auto c = iptc.findKey(Exiv2::IptcKey("Iptc.Application2.Caption"));
            if (c != iptc.end())
                s.description = str(c->toString());
        }
        if (s.description.isEmpty())
            s.description = printed(exif, "Exif.Image.ImageDescription");
        const auto subj = xmp.findKey(Exiv2::XmpKey("Xmp.dc.subject"));
        if (subj != xmp.end()) {
            for (long i = 0; i < long(subj->count()); ++i)
                s.keywords << str(subj->toString(i));
        } else {
            for (const auto &d : iptc)
                if (d.key() == "Iptc.Application2.Keywords")
                    s.keywords << str(d.toString());
        }
        const auto rating = xmp.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"));
        if (rating != xmp.end())
            s.rating = int(toInteger(*rating));
        else if (exif.findKey(Exiv2::ExifKey("Exif.Image.Rating")) != exif.end())
            s.rating = int(toInteger(*exif.findKey(Exiv2::ExifKey("Exif.Image.Rating"))));

        s.hasGps = gpsValue(exif, "Exif.GPSInfo.GPSLatitude", "Exif.GPSInfo.GPSLatitudeRef", &s.latitude)
                && gpsValue(exif, "Exif.GPSInfo.GPSLongitude", "Exif.GPSInfo.GPSLongitudeRef", &s.longitude);
        const auto alt = exif.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSAltitude"));
        if (alt != exif.end()) {
            const Exiv2::Rational r = alt->toRational(0);
            if (r.second != 0) {
                s.altitude = double(r.first) / r.second;
                s.hasAltitude = true;
            }
        }
    } catch (const std::exception &) {
        // Keine oder unlesbare Metadaten: leere Zusammenfassung.
    }
    return s;
}

bool canWrite(const QString &path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    return s == QLatin1String("jpg") || s == QLatin1String("jpeg") || s == QLatin1String("png")
        || s == QLatin1String("tif") || s == QLatin1String("tiff") || s == QLatin1String("webp");
}

bool writeDescriptive(const QString &path, const DescriptiveFields &f, QString *error)
{
    if (!canWrite(path)) {
        if (error)
            *error = QStringLiteral("metadata can only be written to JPEG, PNG, TIFF and WebP files");
        return false;
    }
    try {
        OpenImage opened;
        openImage(opened, path);
        auto &image = opened.image;
        Exiv2::ExifData &exif = image->exifData();
        Exiv2::IptcData &iptc = image->iptcData();
        Exiv2::XmpData &xmp = image->xmpData();
        const bool useIptc = image->supportsMetadata(Exiv2::mdIptc);

        auto setLangAlt = [&xmp](const char *key, const QString &text) {
            eraseKey(xmp, key);
            if (!text.isEmpty())
                xmp[key] = std::string("lang=x-default ") + text.toStdString();
        };
        if (f.title)
            setLangAlt("Xmp.dc.title", *f.title);
        if (f.description) {
            setLangAlt("Xmp.dc.description", *f.description);
            if (f.description->isEmpty()) {
                const auto it = exif.findKey(Exiv2::ExifKey("Exif.Image.ImageDescription"));
                if (it != exif.end())
                    exif.erase(it);
            } else {
                exif["Exif.Image.ImageDescription"] = f.description->toStdString();
            }
            if (useIptc) {
                eraseKey(iptc, "Iptc.Application2.Caption");
                if (!f.description->isEmpty())
                    iptc["Iptc.Application2.Caption"] = f.description->toStdString();
            }
        }
        if (f.keywords) {
            eraseKey(xmp, "Xmp.dc.subject");
            if (!f.keywords->isEmpty()) {
                auto bag = Exiv2::Value::create(Exiv2::xmpBag);
                for (const QString &k : *f.keywords)
                    bag->read(k.toStdString());
                xmp.add(Exiv2::XmpKey("Xmp.dc.subject"), bag.get());
            }
            if (useIptc) {
                eraseKey(iptc, "Iptc.Application2.Keywords");
                for (const QString &k : *f.keywords) {
                    auto v = Exiv2::Value::create(Exiv2::string);
                    v->read(k.toStdString());
                    iptc.add(Exiv2::IptcKey("Iptc.Application2.Keywords"), v.get());
                }
            }
        }
        if (f.rating) {
            const int r = std::clamp(*f.rating, 0, 5);
            xmp["Xmp.xmp.Rating"] = std::to_string(r);
            exif["Exif.Image.Rating"] = uint16_t(r);
        }
        if (useIptc && !iptc.empty())
            iptc["Iptc.Envelope.CharacterSet"] = std::string("\x1b%G");   // UTF-8
        saveImage(opened, path);
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromLocal8Bit(e.what());
        return false;
    }
}

bool setOrientation(QByteArray &data, int orientation, bool swapDimensions, QString *error)
{
    try {
        OpenImage o;
        o.data = data;
        o.image = Exiv2::ImageFactory::open(reinterpret_cast<const Exiv2::byte *>(o.data.constData()), o.data.size());
        o.image->readMetadata();
        Exiv2::ExifData exif = o.image->exifData();
        Exiv2::XmpData xmp = o.image->xmpData();
        if (swapDimensions)
            Exiv2::ExifThumb(exif).erase();   // zeigt sonst die alte Lage
        exif["Exif.Image.Orientation"] = uint16_t(orientation);
        if (xmp.findKey(Exiv2::XmpKey("Xmp.tiff.Orientation")) != xmp.end())
            xmp["Xmp.tiff.Orientation"] = std::to_string(orientation);
        if (swapDimensions) {
            const auto wIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelXDimension"));
            const auto hIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelYDimension"));
            if (wIt != exif.end() && hIt != exif.end()) {
                const auto w = uint32_t(toInteger(*wIt));
                const auto h = uint32_t(toInteger(*hIt));
                exif["Exif.Photo.PixelXDimension"] = h;
                exif["Exif.Photo.PixelYDimension"] = w;
            }
        }
        o.image->setExifData(exif);
        o.image->setXmpData(xmp);
        o.image->writeMetadata();
        Exiv2::BasicIo &io = o.image->io();
        const auto size = io.size();
        const Exiv2::byte *bytes = io.mmap();
        data = QByteArray(reinterpret_cast<const char *>(bytes), qsizetype(size));
        io.munmap();
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromLocal8Bit(e.what());
        return false;
    }
}

struct Snapshot::Data
{
    Exiv2::ExifData exif;
    Exiv2::IptcData iptc;
    Exiv2::XmpData xmp;
};

Snapshot Snapshot::capture(const QString &path)
{
    Snapshot snap;
    try {
        Loaded l = load(path);
        if (!l.exif && !l.opened)
            return snap;
        snap.d = std::make_unique<Data>();
        if (l.exif)
            snap.d->exif = *l.exif;
        if (l.opened) {
            snap.d->iptc = l.opened->image->iptcData();
            snap.d->xmp = l.opened->image->xmpData();
        }
    } catch (const std::exception &) {
        snap.d.reset();
    }
    return snap;
}

bool Snapshot::isEmpty() const
{
    return !d || (d->exif.empty() && d->iptc.empty() && d->xmp.empty());
}

bool Snapshot::applyTo(const QString &path, const QSize &size, bool resetOrientation, QString *error) const
{
    if (isEmpty())
        return true;
    try {
        Exiv2::ExifData exif = d->exif;
        Exiv2::XmpData xmp = d->xmp;

        // Eingebettetes Vorschaubild zeigt den alten Inhalt – entfernen.
        Exiv2::ExifThumb(exif).erase();

        if (resetOrientation) {
            if (exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation")) != exif.end())
                exif["Exif.Image.Orientation"] = uint16_t(1);
            if (xmp.findKey(Exiv2::XmpKey("Xmp.tiff.Orientation")) != xmp.end())
                xmp["Xmp.tiff.Orientation"] = std::string("1");
        }
        if (size.isValid()) {
            if (exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelXDimension")) != exif.end()) {
                exif["Exif.Photo.PixelXDimension"] = uint32_t(size.width());
                exif["Exif.Photo.PixelYDimension"] = uint32_t(size.height());
            }
        }

        OpenImage opened;
        openImage(opened, path);
        auto &image = opened.image;   // vorhandenes (z. B. ICC-Profil) beibehalten
        image->setExifData(exif);
        image->setIptcData(d->iptc);
        image->setXmpData(xmp);
        saveImage(opened, path);
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromLocal8Bit(e.what());
        return false;
    }
}

#else // ohne exiv2

struct Snapshot::Data {};

bool available() { return false; }
void initialize() {}
bool canWrite(const QString &) { return false; }
bool setOrientation(QByteArray &, int, bool, QString *error)
{
    if (error)
        *error = QStringLiteral("this build has no metadata support");
    return false;
}
bool writeDescriptive(const QString &, const DescriptiveFields &, QString *error)
{
    if (error)
        *error = QStringLiteral("this build has no metadata support");
    return false;
}
QList<Entry> read(const QString &, QString *) { return {}; }
Summary summary(const QString &) { return {}; }
Snapshot Snapshot::capture(const QString &) { return {}; }
bool Snapshot::isEmpty() const { return true; }
bool Snapshot::applyTo(const QString &, const QSize &, bool, QString *) const { return true; }

#endif

Snapshot::Snapshot() = default;
Snapshot::~Snapshot() = default;
Snapshot::Snapshot(Snapshot &&) noexcept = default;
Snapshot &Snapshot::operator=(Snapshot &&) noexcept = default;

} // namespace Metadata

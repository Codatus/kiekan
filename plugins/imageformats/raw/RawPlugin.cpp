// Kiek an – Kamera-RAW (über LibRaw)
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Für schnelles Blättern wird das in der RAW-Datei eingebettete Vorschau-JPEG
// gezeigt, sobald es brauchbar groß ist (mindestens 1000 Pixel) – das dauert
// Millisekunden statt Sekunden (Fujifilm X-Trans: über 40 s). Das Bild trägt
// dann den Text „kiekan-raw“ = „preview“ und „kiekan-raw-size“ = volle Größe,
// damit Kiek an bei Bedarf voll entwickeln kann. Mit Qualität 100 (oder
// KIEKAN_RAW_DEVELOP=1) wird immer entwickelt: Kamera-Weißabgleich, 8 Bit, sRGB.
//
// Nur über die Dateiendung gewählt: CR2, NEF, DNG … sind intern TIFF-Dateien,
// eine Erkennung am Inhalt würde mit dem TIFF-Leser kollidieren.

#include <QFile>
#include <QImage>
#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QIODevice>
#include <QTransform>

#include <libraw/libraw.h>

#include <memory>

namespace {

int rotationForFlip(int flip)
{
    switch (flip) {
    case 3: return 180;
    case 5: return 270;   // 90° gegen den Uhrzeigersinn
    case 6: return 90;
    default: return 0;
    }
}

QImage fromProcessed(const libraw_processed_image_t *p)
{
    if (!p)
        return {};
    if (p->type == LIBRAW_IMAGE_JPEG)
        return QImage::fromData(reinterpret_cast<const uchar *>(p->data), int(p->data_size), "jpeg");
    if (p->type == LIBRAW_IMAGE_BITMAP && p->bits == 8 && (p->colors == 3 || p->colors == 1)) {
        const QImage::Format f = p->colors == 3 ? QImage::Format_RGB888 : QImage::Format_Grayscale8;
        return QImage(p->data, p->width, p->height, p->width * p->colors, f).copy();
    }
    return {};
}

// Echte Dateien direkt öffnen: LibRaw liest dann nur, was es braucht
// (wichtig für die Größenabfrage beim Auflisten großer Ordner).
bool openRaw(LibRaw &raw, QIODevice *device, QByteArray &buffer)
{
    if (auto *file = qobject_cast<QFile *>(device); file && !file->fileName().isEmpty())
#if defined(Q_OS_WIN) && defined(LIBRAW_WIN32_UNICODEPATHS)
        return raw.open_file(reinterpret_cast<const wchar_t *>(file->fileName().utf16())) == LIBRAW_SUCCESS;
#else
        return raw.open_file(QFile::encodeName(file->fileName()).constData()) == LIBRAW_SUCCESS;
#endif
    buffer = device->readAll();
    return raw.open_buffer(const_cast<char *>(buffer.constData()), size_t(buffer.size())) == LIBRAW_SUCCESS;
}

class RawHandler : public QImageIOHandler
{
public:
    bool canRead() const override { return device() != nullptr; }

    bool read(QImage *image) override
    {
        QByteArray buffer;
        auto raw = std::make_unique<LibRaw>();
        if (!openRaw(*raw, device(), buffer))
            return false;

        const int flip = raw->imgdata.sizes.flip;
        QSize fullSize(raw->imgdata.sizes.width, raw->imgdata.sizes.height);
        if (rotationForFlip(flip) % 180 != 0)
            fullSize.transpose();
        const QString fullText = QStringLiteral("%1x%2").arg(fullSize.width()).arg(fullSize.height());
        const int rawLong = std::max<int>(raw->imgdata.sizes.width, raw->imgdata.sizes.height);
        const bool forceDevelop = m_quality >= 100 || qEnvironmentVariableIntValue("KIEKAN_RAW_DEVELOP") == 1;

        // 1. Eingebettete Vorschau
        if (!forceDevelop && raw->unpack_thumb() == LIBRAW_SUCCESS) {
            int err = 0;
            libraw_processed_image_t *thumb = raw->dcraw_make_mem_thumb(&err);
            QImage img = fromProcessed(thumb);
            LibRaw::dcraw_clear_mem(thumb);
            const int longSide = std::max(img.width(), img.height());
            if (!img.isNull() && (longSide >= 1000 || longSide >= rawLong * 8 / 10)) {
                if (const int rot = rotationForFlip(flip))
                    img = img.transformed(QTransform().rotate(rot));
                const bool complete = longSide >= rawLong * 9 / 10;
                img.setText(QStringLiteral("kiekan-raw"), complete ? QStringLiteral("developed") : QStringLiteral("preview"));
                img.setText(QStringLiteral("kiekan-raw-size"), fullText);
                *image = img;
                return true;
            }
        }

        // 2. Volle Entwicklung (dreht selbst nach „flip“)
        raw->imgdata.params.use_camera_wb = 1;
        raw->imgdata.params.output_bps = 8;
        raw->imgdata.params.output_color = 1;   // sRGB
        if (raw->unpack() != LIBRAW_SUCCESS || raw->dcraw_process() != LIBRAW_SUCCESS)
            return false;
        int err = 0;
        libraw_processed_image_t *processed = raw->dcraw_make_mem_image(&err);
        QImage img = fromProcessed(processed);
        LibRaw::dcraw_clear_mem(processed);
        if (img.isNull())
            return false;
        img.setText(QStringLiteral("kiekan-raw"), QStringLiteral("developed"));
        img.setText(QStringLiteral("kiekan-raw-size"), fullText);
        *image = img;
        return true;
    }

    QVariant option(ImageOption option) const override
    {
        if (option == Quality)
            return m_quality;
        if (option != Size || !device())
            return {};
        QByteArray buffer;
        auto raw = std::make_unique<LibRaw>();
        const qint64 pos = device()->pos();
        const bool ok = openRaw(*raw, device(), buffer);
        device()->seek(pos);
        if (!ok)
            return {};
        QSize s(raw->imgdata.sizes.width, raw->imgdata.sizes.height);
        if (rotationForFlip(raw->imgdata.sizes.flip) % 180 != 0)
            s.transpose();
        return s;
    }

    void setOption(ImageOption option, const QVariant &value) override
    {
        if (option == Quality)
            m_quality = value.toInt();
    }

    bool supportsOption(ImageOption option) const override { return option == Size || option == Quality; }

private:
    int m_quality = -1;
};

const QList<QByteArray> &rawKeys()
{
    static const QList<QByteArray> keys = {"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "dng", "orf",
                                           "rw2", "raf", "pef", "srw", "x3f", "3fr", "mef", "mos", "mrw", "erf",
                                           "kdc", "dcr", "rwl", "iiq"};
    return keys;
}

} // namespace

class raw : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QImageIOHandlerFactoryInterface_iid FILE "raw.json")

public:
    Capabilities capabilities(QIODevice *, const QByteArray &format) const override
    {
        return rawKeys().contains(format.toLower()) ? CanRead : Capabilities();
    }

    QImageIOHandler *create(QIODevice *device, const QByteArray &format) const override
    {
        auto *h = new RawHandler;
        h->setDevice(device);
        h->setFormat(format);
        return h;
    }
};

#include "RawPlugin.moc"

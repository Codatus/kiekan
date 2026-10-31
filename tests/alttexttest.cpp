// KiekAn! – Test: Bildbeschreibung (Alternativtext) und Textebene
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Metadata.h"
#include "TextLayer.h"

#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>

#include <exiv2/exiv2.hpp>

#include <cstdio>

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "OK    " : "FEHLER", what);
    if (!ok)
        ++failures;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    Metadata::initialize();
    QTemporaryDir tmp;
    const QString jpg = tmp.filePath(QStringLiteral("bild.jpg"));
    QImage img(64, 48, QImage::Format_RGB32);
    img.fill(Qt::darkGreen);
    check(img.save(jpg, "jpg"), "Testbild angelegt");

    const QString alt = QStringLiteral("Grüner Hügel unter blauem Himmel – „Test“");
    Metadata::DescriptiveFields f;
    f.altText = alt;
    QString error;
    check(Metadata::writeDescriptive(jpg, f, &error), "Alternativtext geschrieben");
    check(Metadata::summary(jpg).altText == alt, "Alternativtext gelesen (mit Umlauten)");
    check(Metadata::summary(jpg).description.isEmpty(), "Beschreibung bleibt unberührt");

    // Typ in der Datei: Sprachalternative (lang="x-default"), wie IPTC verlangt
    auto image = Exiv2::ImageFactory::open(jpg.toStdString());
    image->readMetadata();
    const auto it = image->xmpData().findKey(Exiv2::XmpKey("Xmp.iptc.AltTextAccessibility"));
    check(it != image->xmpData().end() && it->typeId() == Exiv2::langAlt, "XMP-Typ ist LangAlt");

    f.altText = QString();
    check(Metadata::writeDescriptive(jpg, f, &error) && Metadata::summary(jpg).altText.isEmpty(), "Alternativtext gelöscht");

    // Wortanfänge passen zu text(): Leerzeichen, Zeilenumbruch, Leerzeile
    TextLayer layer;
    layer.words = {{QRect(), QStringLiteral("Stadtwerke"), 0, 0}, {QRect(), QStringLiteral("Musterstadt"), 0, 0},
                   {QRect(), QStringLiteral("Rechnung"), 1, 1}, {QRect(), QStringLiteral("4711"), 2, 1}};
    const QString text = layer.text();
    const QList<int> offsets = layer.wordOffsets();
    bool ok = offsets.size() == layer.words.size();
    for (int i = 0; ok && i < offsets.size(); ++i)
        ok = text.mid(offsets.at(i), layer.words.at(i).text.size()) == layer.words.at(i).text;
    check(ok, "Wortanfänge stimmen mit dem Text überein");
    check(text == QStringLiteral("Stadtwerke Musterstadt\n\nRechnung\n4711"), "Zeilen und Absätze im Text");

    std::printf("%s\n", failures ? "FEHLGESCHLAGEN" : "alles in Ordnung");
    return failures ? 1 : 0;
}

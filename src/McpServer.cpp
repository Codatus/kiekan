// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "McpServer.h"

#include "EditOps.h"
#include "FilterRegistry.h"
#include "ImageLoader.h"
#include "ImageOps.h"
#include "Media.h"
#include "Metadata.h"
#include "Ocr.h"
#include "OcrWorker.h"
#include "PdfSplit.h"
#include "ViewerBridge.h"

#include <QBuffer>
#include <QCollator>
#include <QColorSpace>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFontMetrics>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QTransform>

#include <cmath>
#include <cstdio>
#include <thread>

namespace {

constexpr int kDefaultPreview = 1024;
constexpr int kMaxPreview = 2048;
constexpr int kMaxListing = 2000;

const char *kInstructions =
    "Kiek an is an image viewer and editor. Use these tools to look at images, PDFs and their "
    "metadata and to edit or convert them.\n"
    "- All paths must be absolute.\n"
    "- view_image returns a downscaled preview. Pixel coordinates in all tools always refer to the "
    "ORIGINAL image (after EXIF rotation). To inspect details, call view_image with `region` "
    "(original pixels) - the region is shown at up to max_size, i.e. often at full resolution. "
    "Use grid=true to overlay labeled coordinates before choosing crop rectangles.\n"
    "- edit_image applies a list of operations in order and writes a NEW file. It never "
    "overwrites unless overwrite=true. EXIF metadata is preserved. Check the returned preview.\n"
    "- list_filters shows the available filters (including plugins) with their parameters.\n"
    "- contact_sheet shows many images at once with numbers - use it first to survey a folder.\n"
    "- viewer_apply shows an edit to the user as an unsaved proposal (Ctrl+S saves, Ctrl+Z reverts).\n"
    "- extract_text reads text from PDFs (text layer) and scans/images (OCR with automatic "
    "language detection); check the reported language and confidence.\n"
    "- split_pdf splits large PDFs (e.g. above upload limits) into documented parts; pdf_parts maps an "
    "original page number to its part and verifies parts. When citing pages of split documents, use the "
    "ORIGINAL page numbers (the parts display them too).\n"
    "- write_metadata stores title, description, keywords and rating in the file.\n"
    "- viewer_* tools talk to the Kiek an window the user has open: viewer_state tells you which "
    "file the user is looking at and what area they selected; viewer_get_image returns that image "
    "(including unsaved edits); viewer_show opens a file for the user; viewer_select highlights a "
    "region so the user can see what you mean.";

// Werkzeugbeschreibungen (JSON-Schema), einmal beim Start eingelesen.
const char *kToolsJson = R"JSON([
{"name":"list_media","description":"List images, PDFs and videos in a folder with type, size in bytes, pixel dimensions (from the file header, before EXIF rotation) and modification time.",
 "inputSchema":{"type":"object","properties":{
   "directory":{"type":"string","description":"Absolute folder path"},
   "recursive":{"type":"boolean","default":false},
   "limit":{"type":"integer","default":500,"description":"Maximum number of entries (up to 2000)"}},
  "required":["directory"]}},
{"name":"image_info","description":"Basic facts about one image or PDF: format, dimensions, pages, animation, transparency, file size, and an EXIF summary (camera, lens, exposure, date, GPS).",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}},
{"name":"view_image","description":"Look at an image or PDF page. Returns a JPEG/PNG preview scaled to max_size (longest side). With `region` only that rectangle (original pixels) is returned, so details can be inspected at full resolution. grid=true overlays labeled coordinate lines in original pixels.",
 "inputSchema":{"type":"object","properties":{
   "path":{"type":"string"},
   "page":{"type":"integer","default":0,"description":"0-based page for PDF and multi-page TIFF"},
   "max_size":{"type":"integer","default":1024,"description":"Longest side of the returned image, 64..2048"},
   "region":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"}},"required":["x","y","width","height"]},
   "grid":{"type":"boolean","default":false}},
  "required":["path"]}},
{"name":"read_metadata","description":"All EXIF, IPTC and XMP entries of a file as key = value lines. Optional case-insensitive filter on key, label or value.",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"filter":{"type":"string"}},"required":["path"]}},
{"name":"histogram","description":"Tonal statistics per channel (min, max, mean, median, standard deviation, share of clipped shadows/highlights) plus a 32-bin luminance histogram. Useful to judge exposure and contrast before editing.",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"page":{"type":"integer","default":0},
   "region":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"}},"required":["x","y","width","height"]}},
  "required":["path"]}},
{"name":"list_filters","description":"Filters available for the `filter` operation of edit_image (built-in and plugins), with their parameters, ranges and defaults.",
 "inputSchema":{"type":"object","properties":{}}},
{"name":"edit_image","description":"Apply operations in order and save the result to a new file (format from the output extension: jpg, png, webp, tif, bmp). Operations: {op:'rotate',degrees:90|180|270} {op:'flip',direction:'horizontal'|'vertical'} {op:'crop',x,y,width,height} {op:'resize',width?,height?,percent?,max_size?} {op:'grayscale'} {op:'invert'} {op:'auto_levels'} {op:'equalize'} {op:'curves',points:[[in,out],...],channel:'all'|'red'|'green'|'blue'} {op:'filter',id,params:{...}}. Coordinates refer to the image as it is at that step. EXIF is kept.",
 "inputSchema":{"type":"object","properties":{
   "input":{"type":"string"},"page":{"type":"integer","default":0},
   "output":{"type":"string","description":"Absolute path of the new file"},
   "operations":{"type":"array","items":{"type":"object"}},
   "quality":{"type":"integer","default":90,"description":"JPEG/WebP quality 1..100"},
   "overwrite":{"type":"boolean","default":false},
   "preview":{"type":"boolean","default":true,"description":"Return a preview of the result"}},
  "required":["input","output","operations"]}},
{"name":"convert_batch","description":"Convert many images (or all images of a folder) to another format, optionally limiting the size and applying the same operations as edit_image. Files keep their base name; pages of PDF, DjVu and multi-page TIFF become name_page-N. Existing files are skipped unless overwrite=true.",
 "inputSchema":{"type":"object","properties":{
   "inputs":{"type":"array","items":{"type":"string"},"description":"Absolute file paths"},
   "directory":{"type":"string","description":"Alternatively: convert all images in this folder"},
   "recursive":{"type":"boolean","default":false},
   "output_dir":{"type":"string"},
   "format":{"type":"string","enum":["jpg","png","webp","tif","bmp"]},
   "quality":{"type":"integer","default":90},
   "max_size":{"type":"integer","description":"Longest side in pixels; larger images are scaled down"},
   "operations":{"type":"array","items":{"type":"object"}},
   "overwrite":{"type":"boolean","default":false}},
  "required":["output_dir","format"]}},
{"name":"viewer_show","description":"Open a file in the user's Kiek an window (starts Kiek an if needed) so the user sees what you are talking about.",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"page":{"type":"integer","default":0}},"required":["path"]}},
{"name":"viewer_state","description":"What the user currently sees in Kiek an: file, page, image size, zoom, unsaved changes and the rectangle the user selected (original pixels).",
 "inputSchema":{"type":"object","properties":{}}},
{"name":"viewer_get_image","description":"The image currently shown in Kiek an, including unsaved edits; with selection_only=true only the user's selected area. For videos: the current frame.",
 "inputSchema":{"type":"object","properties":{"selection_only":{"type":"boolean","default":false},"max_size":{"type":"integer","default":1024}}}},
{"name":"viewer_select","description":"Highlight a rectangle (original pixels) in the user's Kiek an window, e.g. to point at something. clear=true removes the selection.",
 "inputSchema":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"clear":{"type":"boolean","default":false}}}},
{"name":"viewer_apply","description":"Apply edit operations (same format as edit_image) to the image open in the user's Kiek an window as an UNSAVED proposal. The user sees the result and decides: Ctrl+S saves, Ctrl+Z reverts. Nothing is written to disk. Use this when the user should judge an edit.",
 "inputSchema":{"type":"object","properties":{"operations":{"type":"array","items":{"type":"object"}},"note":{"type":"string","description":"Short explanation shown to the user"}},"required":["operations"]}},
{"name":"contact_sheet","description":"Many images at once: numbered thumbnails on one or more sheets plus an index (number -> file). Far cheaper than viewing images one by one - use it to survey, sort or find images in a folder, then view_image the interesting ones.",
 "inputSchema":{"type":"object","properties":{
   "inputs":{"type":"array","items":{"type":"string"}},
   "directory":{"type":"string"},"recursive":{"type":"boolean","default":false},
   "start":{"type":"integer","default":0,"description":"Skip this many files (for paging through large folders)"},
   "max_images":{"type":"integer","default":48,"description":"Images in this call, up to 200"},
   "columns":{"type":"integer","default":6},"rows":{"type":"integer","default":4,"description":"Rows per sheet"},
   "cell_size":{"type":"integer","default":256,"description":"Thumbnail size in pixels, 96..512"}}}},
{"name":"extract_text","description":"Read the text of an image, scan or PDF. PDFs with a text layer are read directly; scans and images go through OCR (Tesseract). With languages='auto' (default) Kiek an detects the script and page orientation, recognises the language from common words and picks the best installed language pack; the answer says which one was used and how confident it is, and suggests packs to install if the right one is missing. Pass e.g. languages='deu+eng' to force packs. words=true adds every word with its box (original pixels) - useful for locating, cropping or redacting text.",
 "inputSchema":{"type":"object","properties":{
   "path":{"type":"string"},
   "pages":{"type":"array","items":{"type":"integer"},"description":"0-based pages for PDF/TIFF; default [0]"},
   "all_pages":{"type":"boolean","default":false,"description":"All pages (up to 100)"},
   "languages":{"type":"string","default":"auto"},
   "region":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"}},"required":["x","y","width","height"]},
   "force_ocr":{"type":"boolean","default":false,"description":"OCR even if the PDF has a text layer"},
   "words":{"type":"boolean","default":false}},
  "required":["path"]}},
{"name":"ocr_languages","description":"Installed OCR language packs, whether orientation/script detection is available, and how to install more.",
 "inputSchema":{"type":"object","properties":{}}},
{"name":"split_pdf","description":"Split a large PDF into parts, e.g. to stay below upload limits (Claude: 30 MB). By default each part is at most max_megabytes (measured, not estimated) and cuts are placed at bookmarks (sections) where possible; alternatively pages_per_part or explicit ranges. Each part is named <name>_Teil-NN-von-MM_Sxxxx-yyyy.pdf, shows the ORIGINAL page numbers in PDF viewers and names source and page range in its document properties. A manifest (<name>_Teilung.json + .md) records source checksum, parts, page ranges, sizes, checksums and bookmarks. Original pages are never modified.",
 "inputSchema":{"type":"object","properties":{
   "path":{"type":"string"},
   "output_dir":{"type":"string","description":"Default: <folder>/<name>_Teile"},
   "max_megabytes":{"type":"number","default":25},
   "pages_per_part":{"type":"integer"},
   "ranges":{"type":"array","items":{"type":"array","items":{"type":"integer"}},"description":"1-based inclusive [[1,120],[121,300]]"},
   "prefer_bookmarks":{"type":"boolean","default":true},
   "overwrite":{"type":"boolean","default":false}},
  "required":["path"]}},
{"name":"pdf_parts","description":"Information about a split PDF: give the manifest, any part, the source PDF or the parts folder. Lists the parts with original page ranges. With `page` (1-based original page number) it tells which part file and which page inside it. With verify=true it checks that all parts exist and are unchanged and whether the source changed. Also works for a single part without manifest (reads its document properties).",
 "inputSchema":{"type":"object","properties":{"path":{"type":"string"},"page":{"type":"integer"},"verify":{"type":"boolean","default":false}},"required":["path"]}},
{"name":"write_metadata","description":"Write descriptive metadata into an image file (XMP, plus IPTC/EXIF for compatibility with other programs): title, description, keywords, rating 0-5. Only given fields change; pixels stay untouched. keywords replaces the list; add_keywords/remove_keywords edit it. JPEG, PNG, TIFF, WebP.",
 "inputSchema":{"type":"object","properties":{
   "path":{"type":"string"},"title":{"type":"string"},"description":{"type":"string"},
   "keywords":{"type":"array","items":{"type":"string"}},
   "add_keywords":{"type":"array","items":{"type":"string"}},
   "remove_keywords":{"type":"array","items":{"type":"string"}},
   "rating":{"type":"integer","minimum":0,"maximum":5}},
  "required":["path"]}}
])JSON";

using ToolError = EditOps::Error;

[[noreturn]] void fail(const QString &message)
{
    throw ToolError{message};
}

QJsonObject textContent(const QString &text)
{
    return {{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}};
}

QJsonObject imageContent(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    const bool alpha = image.hasAlphaChannel();
    QImageWriter writer(&buffer, alpha ? "png" : "jpeg");
    if (!alpha)
        writer.setQuality(85);
    writer.write(alpha ? image : image.convertToFormat(QImage::Format_RGB32));
    return {{QStringLiteral("type"), QStringLiteral("image")},
            {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())},
            {QStringLiteral("mimeType"), alpha ? QStringLiteral("image/png") : QStringLiteral("image/jpeg")}};
}

QString str(const QJsonObject &args, const char *key, bool required = false)
{
    const QJsonValue v = args.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull()) {
        if (required)
            fail(QStringLiteral("missing argument '%1'").arg(QLatin1String(key)));
        return {};
    }
    if (!v.isString())
        fail(QStringLiteral("argument '%1' must be a string").arg(QLatin1String(key)));
    return v.toString();
}

int integer(const QJsonObject &args, const char *key, int def)
{
    const QJsonValue v = args.value(QLatin1String(key));
    return v.isDouble() ? int(std::lround(v.toDouble())) : def;
}

bool boolean(const QJsonObject &args, const char *key, bool def)
{
    const QJsonValue v = args.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : def;
}

QString humanBytes(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
}

// --- Zugriffsregeln ------------------------------------------------------------

QStringList allowedRoots()
{
    QStringList roots = QSettings().value(QStringLiteral("ai/allowedDirs")).toStringList();
    const QString env = qEnvironmentVariable("KIEKAN_MCP_ROOTS");
    if (!env.isEmpty())
        roots += env.split(QDir::listSeparator(), Qt::SkipEmptyParts);
    QStringList clean;
    for (const QString &r : roots) {
        const QFileInfo fi(r);
        clean << QDir::cleanPath(fi.exists() ? fi.canonicalFilePath() : fi.absoluteFilePath());
    }
    return clean;
}

// Prüft einen Pfad aus der Anfrage: absolut, ggf. existent, innerhalb der
// erlaubten Ordner. Gibt den bereinigten Pfad zurück.
QString checkedPath(const QString &path, bool mustExist)
{
    if (path.isEmpty())
        fail(QStringLiteral("empty path"));
    if (QDir::isRelativePath(path))
        fail(QStringLiteral("path must be absolute: %1").arg(path));
    QFileInfo fi(path);
    if (mustExist && !fi.exists())
        fail(QStringLiteral("not found: %1").arg(path));

    // Für neue Dateien den existierenden Elternordner auflösen.
    QString resolved;
    if (fi.exists()) {
        resolved = fi.canonicalFilePath();
    } else {
        const QFileInfo parent(fi.absolutePath());
        resolved = QDir::cleanPath((parent.exists() ? parent.canonicalFilePath() : parent.absoluteFilePath())
                                   + QLatin1Char('/') + fi.fileName());
    }

    const QStringList roots = allowedRoots();
    if (!roots.isEmpty()) {
        bool inside = false;
        for (const QString &root : roots) {
            if (resolved == root || resolved.startsWith(root + QLatin1Char('/'))) {
                inside = true;
                break;
            }
        }
        if (!inside)
            fail(QStringLiteral("access outside the allowed folders (%1) is not permitted").arg(roots.join(QStringLiteral(", "))));
    }
    return resolved;
}

// --- Bilder laden, verkleinern, schreiben ---------------------------------------

// `full`: Kamera-RAW voll entwickeln (für Bearbeiten/Konvertieren); zum
// Ansehen genügt die eingebettete Vorschau.
QImage loadImage(const QString &path, int page, LoadResult *info = nullptr, bool full = false)
{
    if (Media::isVideo(path))
        fail(QStringLiteral("videos are not supported by this tool; use viewer_show and viewer_get_image for a frame"));
    const LoadResult r = ImageLoader::decode(path, page, 200, full);
    if (r.image.isNull())
        fail(QStringLiteral("cannot read %1: %2").arg(path, r.error));
    if (info)
        *info = r;
    return r.image;
}

QRect regionArg(const QJsonObject &args, const QSize &bounds, bool *given)
{
    const QJsonValue v = args.value(QLatin1String("region"));
    *given = v.isObject();
    if (!v.isObject())
        return QRect(QPoint(0, 0), bounds);
    const QJsonObject o = v.toObject();
    const QRect r(integer(o, "x", 0), integer(o, "y", 0), integer(o, "width", 0), integer(o, "height", 0));
    const QRect clipped = r.intersected(QRect(QPoint(0, 0), bounds));
    if (clipped.isEmpty())
        fail(QStringLiteral("region %1,%2 %3x%4 lies outside the image (%5x%6)")
                 .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(bounds.width()).arg(bounds.height()));
    return clipped;
}

// Beschriftetes Koordinatengitter (in Originalpixeln) über die Vorschau legen.
void drawGrid(QImage &preview, const QRect &source)
{
    const double scale = double(preview.width()) / source.width();
    const int longest = std::max(source.width(), source.height());
    int step = 10;
    for (int s : {10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000, 10000}) {
        step = s;
        if (longest / s <= 10)
            break;
    }
    QPainter p(&preview);
    p.setRenderHint(QPainter::Antialiasing);
    QFont font = p.font();
    font.setPixelSize(std::max(10, preview.width() / 60));
    p.setFont(font);
    const QPen line(QColor(255, 230, 0, 170), 1);

    auto label = [&](const QPointF &at, const QString &text) {
        const QRectF box = QRectF(p.fontMetrics().boundingRect(text)).translated(at).adjusted(-2, -1, 2, 1);
        p.fillRect(box, QColor(0, 0, 0, 160));
        p.setPen(Qt::white);
        p.drawText(at, text);
    };
    for (int x = (source.left() / step + 1) * step; x < source.right(); x += step) {
        const double px = (x - source.left()) * scale;
        p.setPen(line);
        p.drawLine(QPointF(px, 0), QPointF(px, preview.height()));
        label(QPointF(px + 2, font.pixelSize() + 2), QString::number(x));
    }
    for (int y = (source.top() / step + 1) * step; y < source.bottom(); y += step) {
        const double py = (y - source.top()) * scale;
        p.setPen(line);
        p.drawLine(QPointF(0, py), QPointF(preview.width(), py));
        label(QPointF(2, py - 3), QString::number(y));
    }
}

QByteArray writerFormat(const QString &path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    if (s == QLatin1String("jpg") || s == QLatin1String("jpeg"))
        return "jpeg";
    if (s == QLatin1String("tif") || s == QLatin1String("tiff"))
        return "tiff";
    if (s == QLatin1String("png") || s == QLatin1String("webp") || s == QLatin1String("bmp"))
        return s.toLatin1();
    return {};
}

void writeImage(const QImage &input, const QString &output, int quality, const QString &metadataSource)
{
    const QByteArray format = writerFormat(output);
    if (format.isEmpty() || !QImageWriter::supportedImageFormats().contains(format))
        fail(QStringLiteral("unsupported output format: %1 (use jpg, png, webp, tif or bmp)").arg(QFileInfo(output).suffix()));

    QImage img = input;
    if (img.hasAlphaChannel() && (format == "jpeg" || format == "bmp")) {
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, img);
        p.end();
        img = flat;
    }

    Metadata::Snapshot meta;
    if (!metadataSource.isEmpty() && !Media::isPdf(metadataSource) && format != "bmp")
        meta = Metadata::Snapshot::capture(metadataSource);

    QSaveFile file(output);
    if (!file.open(QIODevice::WriteOnly))
        fail(QStringLiteral("cannot write %1: %2").arg(output, file.errorString()));
    QImageWriter writer(&file, format);
    if (format == "jpeg" || format == "webp")
        writer.setQuality(std::clamp(quality, 1, 100));
    if (!writer.write(img) || !file.commit())
        fail(QStringLiteral("cannot write %1: %2").arg(output, writer.errorString()));
    meta.applyTo(output, img.size(), true);
}

QJsonArray viewerCall(const QJsonObject &req, QJsonObject *answer)
{
    QString error;
    *answer = ViewerBridge::request(req, 10000, &error);
    if (!error.isEmpty())
        fail(error + QStringLiteral(" (open Kiek an first, or use viewer_show)"));
    if (answer->contains(QLatin1String("error")))
        fail(answer->value(QLatin1String("error")).toString());
    return {};
}

} // namespace

McpServer::McpServer(QObject *parent)
    : QObject(parent)
{
}

void McpServer::start()
{
    // stdin blockierend in einem eigenen Thread lesen; jede Zeile ist eine
    // JSON-RPC-Nachricht (MCP stdio-Transport).
    std::thread([this] {
        std::string line;
        char buf[65536];
        while (std::fgets(buf, sizeof buf, stdin)) {
            line += buf;
            if (!line.empty() && line.back() == '\n') {
                const QByteArray bytes = QByteArray::fromStdString(line);
                line.clear();
                QMetaObject::invokeMethod(this, [this, bytes] { handleLine(bytes); }, Qt::QueuedConnection);
            }
        }
        QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
    }).detach();
}

void McpServer::handleLine(const QByteArray &line)
{
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &error);
    if (!doc.isObject()) {
        send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), QJsonValue::Null},
              {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32700}, {QStringLiteral("message"), QStringLiteral("parse error")}}}});
        return;
    }
    const QJsonObject answer = handleMessage(doc.object());
    if (!answer.isEmpty())
        send(answer);
}

void McpServer::send(const QJsonObject &message)
{
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

QJsonObject McpServer::handleMessage(const QJsonObject &message)
{
    const QString method = message.value(QLatin1String("method")).toString();
    const QJsonValue id = message.value(QLatin1String("id"));
    if (id.isUndefined())
        return {};   // Benachrichtigung (z. B. notifications/initialized)

    auto reply = [&](const QJsonObject &result) {
        return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
                           {QStringLiteral("result"), result}};
    };
    auto error = [&](int code, const QString &text) {
        return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
                           {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), text}}}};
    };
    const QJsonObject params = message.value(QLatin1String("params")).toObject();

    if (method == QLatin1String("initialize")) {
        static const QStringList known = {QStringLiteral("2024-11-05"), QStringLiteral("2025-03-26"), QStringLiteral("2025-06-18")};
        const QString requested = params.value(QLatin1String("protocolVersion")).toString();
        return reply({
            {QStringLiteral("protocolVersion"), known.contains(requested) ? requested : known.last()},
            {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{}}}},
            {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("kiekan")},
                                                       {QStringLiteral("version"), QCoreApplication::applicationVersion()}}},
            {QStringLiteral("instructions"), QString::fromUtf8(kInstructions)},
        });
    }
    if (method == QLatin1String("ping"))
        return reply({});
    if (method == QLatin1String("tools/list")) {
        static const QJsonArray tools = QJsonDocument::fromJson(kToolsJson).array();
        return reply({{QStringLiteral("tools"), tools}});
    }
    if (method == QLatin1String("tools/call")) {
        return reply(callTool(params.value(QLatin1String("name")).toString(),
                              params.value(QLatin1String("arguments")).toObject()));
    }
    return error(-32601, QStringLiteral("method not found: %1").arg(method));
}

QJsonObject McpServer::callTool(const QString &name, const QJsonObject &args)
{
    using Fn = QJsonArray (McpServer::*)(const QJsonObject &);
    static const QHash<QString, Fn> tools = {
        {QStringLiteral("list_media"), &McpServer::listMedia},
        {QStringLiteral("image_info"), &McpServer::imageInfo},
        {QStringLiteral("view_image"), &McpServer::viewImage},
        {QStringLiteral("read_metadata"), &McpServer::readMetadata},
        {QStringLiteral("histogram"), &McpServer::histogram},
        {QStringLiteral("list_filters"), &McpServer::listFilters},
        {QStringLiteral("edit_image"), &McpServer::editImage},
        {QStringLiteral("convert_batch"), &McpServer::convertBatch},
        {QStringLiteral("viewer_show"), &McpServer::viewerShow},
        {QStringLiteral("viewer_state"), &McpServer::viewerState},
        {QStringLiteral("viewer_get_image"), &McpServer::viewerGetImage},
        {QStringLiteral("viewer_select"), &McpServer::viewerSelect},
        {QStringLiteral("viewer_apply"), &McpServer::viewerApply},
        {QStringLiteral("contact_sheet"), &McpServer::contactSheet},
        {QStringLiteral("write_metadata"), &McpServer::writeMetadata},
        {QStringLiteral("extract_text"), &McpServer::extractText},
        {QStringLiteral("ocr_languages"), &McpServer::ocrLanguages},
        {QStringLiteral("split_pdf"), &McpServer::splitPdf},
        {QStringLiteral("pdf_parts"), &McpServer::pdfParts},
    };
    const auto it = tools.find(name);
    if (it == tools.end())
        return {{QStringLiteral("content"), QJsonArray{textContent(QStringLiteral("unknown tool: ") + name)}},
                {QStringLiteral("isError"), true}};
    try {
        return {{QStringLiteral("content"), (this->*it.value())(args)}};
    } catch (const ToolError &e) {
        return {{QStringLiteral("content"), QJsonArray{textContent(e.message)}}, {QStringLiteral("isError"), true}};
    }
}

// --- Dateiwerkzeuge ------------------------------------------------------------

QJsonArray McpServer::listMedia(const QJsonObject &args)
{
    const QString dir = checkedPath(str(args, "directory", true), true);
    if (!QFileInfo(dir).isDir())
        fail(QStringLiteral("not a folder: %1").arg(dir));
    const int limit = std::clamp(integer(args, "limit", 500), 1, kMaxListing);

    QDirIterator it(dir, Media::allNameFilters(), QDir::Files | QDir::Readable,
                    boolean(args, "recursive", false) ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    QStringList paths;
    bool truncated = false;
    while (it.hasNext()) {
        if (paths.size() >= kMaxListing) {
            truncated = true;
            break;
        }
        paths << it.next();
    }
    // Natürliche Sortierung wie im Fenster (Bild2 vor Bild10).
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(paths.begin(), paths.end(), [&](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
    if (paths.size() > limit) {
        paths = paths.mid(0, limit);
        truncated = true;
    }

    QStringList lines;
    int count = 0;
    for (const QString &path : paths) {
        ++count;
        const QFileInfo fi(path);
        QString type = QStringLiteral("image");
        QString dims;
        if (Media::isVideo(path)) {
            type = QStringLiteral("video");
        } else if (Media::isPdf(path)) {
            type = QStringLiteral("pdf");
        } else {
            const QSize s = QImageReader(path).size();
            if (s.isValid())
                dims = QStringLiteral("%1x%2").arg(s.width()).arg(s.height());
        }
        lines << QStringLiteral("%1\t%2\t%3\t%4\t%5").arg(QDir(dir).relativeFilePath(path), type, dims)
                     .arg(fi.size()).arg(fi.lastModified().toString(Qt::ISODate));
    }
    QString text = QStringLiteral("%1 (%2 entries%3)\nname\ttype\tdimensions\tbytes\tmodified\n")
                       .arg(dir).arg(count).arg(truncated ? QStringLiteral(", truncated") : QString());
    text += lines.join(QLatin1Char('\n'));
    return {textContent(text)};
}

QJsonArray McpServer::imageInfo(const QJsonObject &args)
{
    const QString path = checkedPath(str(args, "path", true), true);
    const QFileInfo fi(path);
    QJsonObject info{{QStringLiteral("path"), path}, {QStringLiteral("bytes"), fi.size()},
                     {QStringLiteral("modified"), fi.lastModified().toString(Qt::ISODate)}};

    if (Media::isVideo(path)) {
        info.insert(QStringLiteral("type"), QStringLiteral("video"));
    } else if (Media::isPdf(path)) {
        info.insert(QStringLiteral("type"), QStringLiteral("pdf"));
        info.insert(QStringLiteral("pages"), Media::pdfPageCount(path));
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QSize size = reader.size();
        if (reader.transformation() & QImageIOHandler::TransformationRotate90)
            size.transpose();
        info.insert(QStringLiteral("type"), QStringLiteral("image"));
        info.insert(QStringLiteral("format"), QString::fromLatin1(reader.format()));
        info.insert(QStringLiteral("width"), size.width());
        info.insert(QStringLiteral("height"), size.height());
        const int frames = reader.imageCount();
        info.insert(QStringLiteral("animated"), reader.supportsAnimation() && frames != 1);
        if (!reader.supportsAnimation() && frames > 1)
            info.insert(QStringLiteral("pages"), frames);
        const QImage probe = reader.read();
        if (!probe.isNull()) {
            info.insert(QStringLiteral("alpha"), probe.hasAlphaChannel());
            if (probe.colorSpace().isValid())
                info.insert(QStringLiteral("color_space"), probe.colorSpace().description());
        }
    }

    const Metadata::Summary s = Metadata::summary(path);
    if (!s.camera.isEmpty() || !s.dateTaken.isEmpty() || !s.exposure.isEmpty() || s.hasGps) {
        QJsonObject exif;
        auto put = [&exif](const char *k, const QString &v) { if (!v.isEmpty()) exif.insert(QLatin1String(k), v); };
        put("camera", s.camera);
        put("lens", s.lens);
        put("date_taken", s.dateTaken);
        put("exposure", s.exposure);
        put("aperture", s.aperture);
        put("iso", s.iso);
        put("focal_length", s.focalLength);
        if (s.hasGps) {
            exif.insert(QStringLiteral("gps_latitude"), s.latitude);
            exif.insert(QStringLiteral("gps_longitude"), s.longitude);
            if (s.hasAltitude)
                exif.insert(QStringLiteral("gps_altitude_m"), s.altitude);
        }
        info.insert(QStringLiteral("exif"), exif);
    }
    if (s.hasDescriptive()) {
        QJsonObject d;
        if (!s.title.isEmpty()) d.insert(QStringLiteral("title"), s.title);
        if (!s.description.isEmpty()) d.insert(QStringLiteral("description"), s.description);
        if (!s.keywords.isEmpty()) d.insert(QStringLiteral("keywords"), QJsonArray::fromStringList(s.keywords));
        if (s.rating >= 0) d.insert(QStringLiteral("rating"), s.rating);
        info.insert(QStringLiteral("descriptive"), d);
    }
    return {textContent(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Indented)))};
}

QJsonArray McpServer::viewImage(const QJsonObject &args)
{
    const QString path = checkedPath(str(args, "path", true), true);
    const int page = std::max(0, integer(args, "page", 0));
    LoadResult info;
    const QImage image = loadImage(path, page, &info);
    bool hasRegion = false;
    const QRect region = regionArg(args, image.size(), &hasRegion);
    const int maxSize = std::clamp(integer(args, "max_size", kDefaultPreview), 64, kMaxPreview);

    QImage preview = EditOps::scaledToMax(hasRegion ? image.copy(region) : image, maxSize);
    if (preview.format() != QImage::Format_RGB32 && preview.format() != QImage::Format_ARGB32_Premultiplied)
        preview = ImageOps::forDisplay(preview);
    if (boolean(args, "grid", false)) {
        preview = preview.convertToFormat(preview.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
        drawGrid(preview, region);
    }

    QString text = QStringLiteral("%1: original %2x%3").arg(QFileInfo(path).fileName()).arg(image.width()).arg(image.height());
    if (info.pageCount > 1)
        text += QStringLiteral(", page %1 of %2").arg(page + 1).arg(info.pageCount);
    if (info.rawPreview)
        text += QStringLiteral(" (embedded RAW preview; full RAW is %1x%2 – edit_image/convert_batch develop it fully)")
                    .arg(info.rawFullSize.width()).arg(info.rawFullSize.height());
    if (hasRegion)
        text += QStringLiteral(", region x=%1 y=%2 %3x%4").arg(region.x()).arg(region.y()).arg(region.width()).arg(region.height());
    text += QStringLiteral(", shown at %1x%2 (scale %3)").arg(preview.width()).arg(preview.height())
                .arg(double(preview.width()) / region.width(), 0, 'f', 3);
    return {textContent(text), imageContent(preview)};
}

QJsonArray McpServer::readMetadata(const QJsonObject &args)
{
    const QString path = checkedPath(str(args, "path", true), true);
    if (!Metadata::available())
        fail(QStringLiteral("this build has no metadata support"));
    const QString filter = str(args, "filter");
    QString error;
    const QList<Metadata::Entry> entries = Metadata::read(path, &error);
    QStringList lines;
    for (const Metadata::Entry &e : entries) {
        if (!filter.isEmpty() && !e.key.contains(filter, Qt::CaseInsensitive)
            && !e.label.contains(filter, Qt::CaseInsensitive) && !e.value.contains(filter, Qt::CaseInsensitive))
            continue;
        lines << QStringLiteral("%1 = %2").arg(e.key, e.value);
    }
    if (lines.isEmpty())
        return {textContent(error.isEmpty() ? QStringLiteral("no matching metadata") : QStringLiteral("no metadata: ") + error)};
    return {textContent(lines.join(QLatin1Char('\n')))};
}

QJsonArray McpServer::histogram(const QJsonObject &args)
{
    const QString path = checkedPath(str(args, "path", true), true);
    QImage image = loadImage(path, std::max(0, integer(args, "page", 0)));
    bool hasRegion = false;
    const QRect region = regionArg(args, image.size(), &hasRegion);
    if (hasRegion)
        image = image.copy(region);
    const ImageOps::Histogram h = ImageOps::histogram(image);
    if (h.pixels == 0)
        fail(QStringLiteral("image has no opaque pixels"));

    auto stats = [&h](const std::array<uint32_t, 256> &d) {
        int lo = -1, hi = 0, median = 0;
        double sum = 0;
        uint64_t acc = 0;
        for (int i = 0; i < 256; ++i) {
            if (d[i]) {
                if (lo < 0) lo = i;
                hi = i;
            }
            sum += double(i) * d[i];
        }
        for (int i = 0; i < 256; ++i) {
            acc += d[i];
            if (acc * 2 >= h.pixels) { median = i; break; }
        }
        const double mean = sum / double(h.pixels);
        double var = 0;
        for (int i = 0; i < 256; ++i)
            var += (i - mean) * (i - mean) * d[i];
        uint64_t dark = 0, bright = 0;
        for (int i = 0; i <= 2; ++i) dark += d[i];
        for (int i = 253; i <= 255; ++i) bright += d[i];
        return QJsonObject{
            {QStringLiteral("min"), lo}, {QStringLiteral("max"), hi},
            {QStringLiteral("mean"), std::round(mean * 10) / 10}, {QStringLiteral("median"), median},
            {QStringLiteral("stddev"), std::round(std::sqrt(var / double(h.pixels)) * 10) / 10},
            {QStringLiteral("clipped_shadows_pct"), std::round(1000.0 * dark / double(h.pixels)) / 10},
            {QStringLiteral("clipped_highlights_pct"), std::round(1000.0 * bright / double(h.pixels)) / 10},
        };
    };
    QJsonArray bins;
    for (int b = 0; b < 32; ++b) {
        uint64_t n = 0;
        for (int i = b * 8; i < b * 8 + 8; ++i)
            n += h.luma[i];
        bins.append(std::round(1000.0 * n / double(h.pixels)) / 10);
    }
    const QJsonObject result{
        {QStringLiteral("pixels"), double(h.pixels)},
        {QStringLiteral("luminance"), stats(h.luma)},
        {QStringLiteral("red"), stats(h.red)},
        {QStringLiteral("green"), stats(h.green)},
        {QStringLiteral("blue"), stats(h.blue)},
        {QStringLiteral("luminance_histogram_32_bins_pct"), bins},
    };
    return {textContent(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented)))};
}

QJsonArray McpServer::listFilters(const QJsonObject &)
{
    QJsonArray list;
    const FilterRegistry &reg = FilterRegistry::instance();
    for (const auto *group : {&reg.adjustments(), &reg.effects()}) {
        for (const Kiekan::Filter *f : *group) {
            QJsonArray params;
            for (const Kiekan::FilterParameter &p : f->parameters()) {
                QJsonObject o{{QStringLiteral("id"), p.id}, {QStringLiteral("label"), p.label},
                              {QStringLiteral("default"), p.defaultValue}};
                if (p.type == Kiekan::FilterParameter::Toggle) {
                    o.insert(QStringLiteral("type"), QStringLiteral("toggle (0 or 1)"));
                } else {
                    o.insert(QStringLiteral("min"), p.minimum);
                    o.insert(QStringLiteral("max"), p.maximum);
                    if (!p.suffix.trimmed().isEmpty())
                        o.insert(QStringLiteral("unit"), p.suffix.trimmed());
                }
                params.append(o);
            }
            list.append(QJsonObject{{QStringLiteral("id"), f->id()}, {QStringLiteral("name"), f->name()},
                                    {QStringLiteral("params"), params}});
        }
    }
    return {textContent(QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Indented)))};
}

QJsonArray McpServer::editImage(const QJsonObject &args)
{
    const QString input = checkedPath(str(args, "input", true), true);
    const QString output = checkedPath(str(args, "output", true), false);
    const bool overwrite = boolean(args, "overwrite", false);
    if (QFileInfo::exists(output) && !overwrite)
        fail(QStringLiteral("output exists: %1 (pass overwrite=true to replace it)").arg(output));
    if (!QFileInfo(QFileInfo(output).absolutePath()).isDir())
        fail(QStringLiteral("output folder does not exist: %1").arg(QFileInfo(output).absolutePath()));

    const QImage source = loadImage(input, std::max(0, integer(args, "page", 0)), nullptr, true);
    QStringList log;
    const QImage result = EditOps::apply(source, args.value(QLatin1String("operations")).toArray(), &log);
    writeImage(result, output, integer(args, "quality", 90), input);

    QString text = QStringLiteral("saved %1 (%2x%3, %4)\n").arg(output).arg(result.width()).arg(result.height())
                       .arg(humanBytes(QFileInfo(output).size()));
    if (!log.isEmpty())
        text += QStringLiteral("steps: ") + log.join(QStringLiteral("; "));
    QJsonArray content{textContent(text)};
    if (boolean(args, "preview", true))
        content.append(imageContent(EditOps::scaledToMax(result, 768)));
    return content;
}

QJsonArray McpServer::convertBatch(const QJsonObject &args)
{
    QString format = str(args, "format", true).toLower();
    if (format == QLatin1String("jpeg"))
        format = QStringLiteral("jpg");
    if (format == QLatin1String("tiff"))
        format = QStringLiteral("tif");
    if (writerFormat(QStringLiteral("x.") + format).isEmpty())
        fail(QStringLiteral("unsupported format '%1'").arg(format));

    const QString outDir = checkedPath(str(args, "output_dir", true), false);
    if (!QDir().mkpath(outDir))
        fail(QStringLiteral("cannot create %1").arg(outDir));

    QStringList inputs;
    for (const QJsonValue &v : args.value(QLatin1String("inputs")).toArray())
        inputs << checkedPath(v.toString(), true);
    const QString dir = str(args, "directory");
    if (!dir.isEmpty()) {
        const QString d = checkedPath(dir, true);
        QDirIterator it(d, Media::imageNameFilters(), QDir::Files | QDir::Readable,
                        boolean(args, "recursive", false) ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (it.hasNext())
            inputs << it.next();
    }
    if (inputs.isEmpty())
        fail(QStringLiteral("no input files (give inputs or directory)"));
    if (inputs.size() > kMaxListing)
        fail(QStringLiteral("too many files (%1, maximum %2)").arg(inputs.size()).arg(kMaxListing));

    const int quality = integer(args, "quality", 90);
    const int maxSize = integer(args, "max_size", 0);
    const bool overwrite = boolean(args, "overwrite", false);
    const QJsonArray ops = args.value(QLatin1String("operations")).toArray();

    int done = 0, skipped = 0, failed = 0;
    QStringList report;
    for (const QString &in : inputs) {
        if (Media::isVideo(in))
            continue;
        // Mehrseitige Dokumente (PDF, DjVu, TIFF) seitenweise umwandeln
        int pages = 1;
        if (Media::isPdf(in)) {
            pages = std::max(1, Media::pdfPageCount(in));
        } else {
            QImageReader probe(in);
            if (!probe.supportsAnimation() && probe.imageCount() > 1)
                pages = probe.imageCount();
        }
        for (int page = 0; page < pages; ++page) {
            QString base = QFileInfo(in).completeBaseName();
            if (pages > 1)
                base += QStringLiteral("_page-%1").arg(page + 1);
            const QString out = QDir(outDir).filePath(base + QLatin1Char('.') + format);
            if (QFileInfo::exists(out) && !overwrite) {
                ++skipped;
                report << QStringLiteral("skipped (exists): %1").arg(out);
                continue;
            }
            if (QFileInfo(out).canonicalFilePath() == QFileInfo(in).canonicalFilePath()) {
                ++skipped;
                report << QStringLiteral("skipped (would replace its own source): %1").arg(out);
                continue;
            }
            try {
                QImage img = EditOps::apply(loadImage(in, page, nullptr, true), ops, nullptr);
                if (maxSize > 0)
                    img = EditOps::scaledToMax(img, maxSize);
                writeImage(img, out, quality, in);
                ++done;
                report << QStringLiteral("ok: %1 (%2x%3)").arg(out).arg(img.width()).arg(img.height());
            } catch (const ToolError &e) {
                ++failed;
                report << QStringLiteral("failed: %1: %2").arg(in, e.message);
            }
        }
    }
    QString text = QStringLiteral("converted %1, skipped %2, failed %3\n").arg(done).arg(skipped).arg(failed);
    text += report.mid(0, 200).join(QLatin1Char('\n'));
    if (report.size() > 200)
        text += QStringLiteral("\n… %1 more lines").arg(report.size() - 200);
    return {textContent(text)};
}

// --- Fensterwerkzeuge ----------------------------------------------------------

QJsonArray McpServer::viewerShow(const QJsonObject &args)
{
    const QString path = checkedPath(str(args, "path", true), true);
    const int page = std::max(0, integer(args, "page", 0));
    QString error;
    const QJsonObject answer = ViewerBridge::request({{QStringLiteral("cmd"), QStringLiteral("show")},
                                                      {QStringLiteral("path"), path},
                                                      {QStringLiteral("page"), page}}, 10000, &error);
    if (!error.isEmpty()) {
        // Kein Fenster offen: Kiek an mit der Datei starten.
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), {path}))
            fail(QStringLiteral("could not start Kiek an"));
        return {textContent(QStringLiteral("started Kiek an with %1").arg(path))};
    }
    if (answer.contains(QLatin1String("error")))
        fail(answer.value(QLatin1String("error")).toString());
    return {textContent(QStringLiteral("showing %1").arg(path))};
}

QJsonArray McpServer::viewerState(const QJsonObject &)
{
    QJsonObject answer;
    viewerCall({{QStringLiteral("cmd"), QStringLiteral("state")}}, &answer);
    return {textContent(QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Indented)))};
}

QJsonArray McpServer::viewerGetImage(const QJsonObject &args)
{
    QJsonObject answer;
    viewerCall({{QStringLiteral("cmd"), QStringLiteral("image")},
                {QStringLiteral("selection_only"), boolean(args, "selection_only", false)},
                {QStringLiteral("max_size"), std::clamp(integer(args, "max_size", kDefaultPreview), 64, kMaxPreview)}},
               &answer);
    const QImage image = QImage::fromData(QByteArray::fromBase64(answer.value(QLatin1String("data")).toString().toLatin1()));
    if (image.isNull())
        fail(QStringLiteral("Kiek an sent no image"));
    QString text = QStringLiteral("current image in Kiek an: original %1x%2")
                       .arg(answer.value(QLatin1String("original_width")).toInt())
                       .arg(answer.value(QLatin1String("original_height")).toInt());
    if (answer.contains(QLatin1String("offset_x")))
        text += QStringLiteral(", selection at x=%1 y=%2").arg(answer.value(QLatin1String("offset_x")).toInt())
                    .arg(answer.value(QLatin1String("offset_y")).toInt());
    text += QStringLiteral(", shown at %1x%2").arg(image.width()).arg(image.height());
    if (answer.value(QLatin1String("modified")).toBool())
        text += QStringLiteral(" (contains unsaved edits)");
    return {textContent(text), imageContent(image)};
}

QJsonArray McpServer::viewerSelect(const QJsonObject &args)
{
    QJsonObject req{{QStringLiteral("cmd"), QStringLiteral("select")}};
    if (boolean(args, "clear", false)) {
        req.insert(QStringLiteral("clear"), true);
    } else {
        req.insert(QStringLiteral("x"), integer(args, "x", 0));
        req.insert(QStringLiteral("y"), integer(args, "y", 0));
        req.insert(QStringLiteral("width"), integer(args, "width", 0));
        req.insert(QStringLiteral("height"), integer(args, "height", 0));
    }
    QJsonObject answer;
    viewerCall(req, &answer);
    return {textContent(answer.value(QLatin1String("message")).toString(QStringLiteral("ok")))};
}

QJsonArray McpServer::viewerApply(const QJsonObject &args)
{
    const QJsonArray ops = args.value(QLatin1String("operations")).toArray();
    if (ops.isEmpty())
        fail(QStringLiteral("operations must not be empty"));
    QJsonObject answer;
    viewerCall({{QStringLiteral("cmd"), QStringLiteral("apply")},
                {QStringLiteral("operations"), ops},
                {QStringLiteral("note"), str(args, "note")}}, &answer);
    return {textContent(QStringLiteral("proposal shown in Kiek an (%1x%2, unsaved). The user can save it with "
                                       "Ctrl+S or revert it with Ctrl+Z.")
                            .arg(answer.value(QLatin1String("width")).toInt())
                            .arg(answer.value(QLatin1String("height")).toInt()))};
}

QJsonArray McpServer::contactSheet(const QJsonObject &args)
{
    QStringList files;
    for (const QJsonValue &v : args.value(QLatin1String("inputs")).toArray())
        files << checkedPath(v.toString(), true);
    const QString dir = str(args, "directory");
    if (!dir.isEmpty()) {
        const QString d = checkedPath(dir, true);
        QStringList found;
        QDirIterator it(d, Media::imageNameFilters(), QDir::Files | QDir::Readable,
                        boolean(args, "recursive", false) ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (it.hasNext() && found.size() < 20000)
            found << it.next();
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(found.begin(), found.end(), [&](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
        files += found;
    }
    files.erase(std::remove_if(files.begin(), files.end(), [](const QString &f) { return Media::isVideo(f); }), files.end());
    if (files.isEmpty())
        fail(QStringLiteral("no images (give inputs or directory)"));

    const int total = int(files.size());
    const int start = std::clamp(integer(args, "start", 0), 0, total);
    const int count = std::min(std::clamp(integer(args, "max_images", 48), 1, 200), total - start);
    if (count <= 0)
        fail(QStringLiteral("start %1 is beyond the last file (%2 files)").arg(start).arg(total));
    const int columns = std::clamp(integer(args, "columns", 6), 1, 12);
    const int rows = std::clamp(integer(args, "rows", 4), 1, 12);
    const int cell = std::clamp(integer(args, "cell_size", 256), 96, 512);

    // Vorschaubilder parallel erzeugen; JPEGs werden beim Dekodieren
    // verkleinert (deutlich schneller als volles Laden).
    std::vector<QImage> thumbs(static_cast<size_t>(count));
    std::vector<QSize> sizes(static_cast<size_t>(count));
    ImageOps::parallelRows(count, [&](int i0, int i1) {
        for (int i = i0; i < i1; ++i) {
            const QString &path = files.at(start + i);
            QImage img;
            if (Media::isPdf(path)) {
                img = ImageLoader::decode(path, 0).image;
                sizes[size_t(i)] = img.size();
            } else {
                QImageReader reader(path);
                reader.setAutoTransform(true);
                const QSize full = reader.size();
                sizes[size_t(i)] = (reader.transformation() & QImageIOHandler::TransformationRotate90) ? full.transposed() : full;
                if (full.isValid() && (full.width() > cell || full.height() > cell))
                    reader.setScaledSize(full.scaled(cell, cell, Qt::KeepAspectRatio));
                img = reader.read();
            }
            if (!img.isNull())
                thumbs[size_t(i)] = img.scaled(cell, cell, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    });

    const int labelH = 20;
    const int perSheet = columns * rows;
    QJsonArray content;
    QStringList index;
    for (int sheetStart = 0; sheetStart < count; sheetStart += perSheet) {
        const int n = std::min(perSheet, count - sheetStart);
        const int usedRows = (n + columns - 1) / columns;
        const int usedCols = std::min(columns, n);
        QImage sheet(usedCols * (cell + 6) + 6, usedRows * (cell + labelH + 6) + 6, QImage::Format_RGB32);
        sheet.fill(QColor(0x22, 0x22, 0x22));
        QPainter p(&sheet);
        p.setRenderHint(QPainter::Antialiasing);
        QFont font = p.font();
        font.setPixelSize(13);
        p.setFont(font);
        QFont numberFont = font;
        numberFont.setPixelSize(18);
        numberFont.setBold(true);
        for (int k = 0; k < n; ++k) {
            const int i = sheetStart + k;
            const int number = start + i + 1;
            const QRect cellRect(6 + (k % columns) * (cell + 6), 6 + (k / columns) * (cell + labelH + 6), cell, cell);
            const QImage &t = thumbs[size_t(i)];
            if (t.isNull()) {
                p.setPen(Qt::red);
                p.drawText(cellRect, Qt::AlignCenter, QStringLiteral("?"));
            } else {
                p.drawImage(cellRect.x() + (cell - t.width()) / 2, cellRect.y() + (cell - t.height()) / 2, t);
            }
            const QString num = QString::number(number);
            const QRect numBox(cellRect.x(), cellRect.y(), QFontMetrics(numberFont).horizontalAdvance(num) + 10, 24);
            p.fillRect(numBox, QColor(0, 0, 0, 190));
            p.setFont(numberFont);
            p.setPen(QColor(255, 220, 0));
            p.drawText(numBox, Qt::AlignCenter, num);
            p.setFont(font);
            p.setPen(QColor(220, 220, 220));
            const QString name = QFileInfo(files.at(start + i)).fileName();
            p.drawText(QRect(cellRect.x(), cellRect.bottom() + 2, cell, labelH), Qt::AlignHCenter | Qt::AlignTop,
                       p.fontMetrics().elidedText(name, Qt::ElideMiddle, cell));

            const QSize s = sizes[size_t(i)];
            index << QStringLiteral("%1: %2%3").arg(number).arg(files.at(start + i))
                         .arg(s.isValid() ? QStringLiteral(" (%1x%2)").arg(s.width()).arg(s.height()) : QString());
        }
        p.end();
        content.append(imageContent(sheet));
    }

    QString text = QStringLiteral("images %1-%2 of %3").arg(start + 1).arg(start + count).arg(total);
    if (start + count < total)
        text += QStringLiteral(" (more: call again with start=%1)").arg(start + count);
    text += QLatin1Char('\n') + index.join(QLatin1Char('\n'));
    content.prepend(textContent(text));
    return content;
}

QJsonArray McpServer::writeMetadata(const QJsonObject &args)
{
    const QString path = checkedPath(str(args, "path", true), true);
    if (!Metadata::canWrite(path))
        fail(QStringLiteral("metadata can only be written to JPEG, PNG, TIFF and WebP files"));

    Metadata::DescriptiveFields f;
    if (args.contains(QLatin1String("title")))
        f.title = str(args, "title");
    if (args.contains(QLatin1String("description")))
        f.description = str(args, "description");
    if (args.contains(QLatin1String("rating"))) {
        const int r = integer(args, "rating", -1);
        if (r < 0 || r > 5)
            fail(QStringLiteral("rating must be 0..5"));
        f.rating = r;
    }
    auto list = [&args](const char *key) {
        QStringList out;
        for (const QJsonValue &v : args.value(QLatin1String(key)).toArray())
            if (!v.toString().trimmed().isEmpty())
                out << v.toString().trimmed();
        return out;
    };
    if (args.contains(QLatin1String("keywords")) || args.contains(QLatin1String("add_keywords"))
        || args.contains(QLatin1String("remove_keywords"))) {
        QStringList kw = args.contains(QLatin1String("keywords")) ? list("keywords") : Metadata::summary(path).keywords;
        for (const QString &k : list("add_keywords"))
            if (!kw.contains(k, Qt::CaseInsensitive))
                kw << k;
        for (const QString &k : list("remove_keywords"))
            kw.removeIf([&k](const QString &x) { return x.compare(k, Qt::CaseInsensitive) == 0; });
        f.keywords = kw;
    }
    if (!f.title && !f.description && !f.keywords && !f.rating)
        fail(QStringLiteral("nothing to write (give title, description, keywords, add_keywords, remove_keywords or rating)"));

    QString error;
    if (!Metadata::writeDescriptive(path, f, &error))
        fail(QStringLiteral("could not write metadata: ") + error);

    const Metadata::Summary s = Metadata::summary(path);
    QJsonObject now{{QStringLiteral("title"), s.title}, {QStringLiteral("description"), s.description},
                    {QStringLiteral("keywords"), QJsonArray::fromStringList(s.keywords)},
                    {QStringLiteral("rating"), s.rating}};
    return {textContent(QStringLiteral("metadata written to %1:\n%2").arg(path,
                            QString::fromUtf8(QJsonDocument(now).toJson(QJsonDocument::Indented))))};
}

QJsonArray McpServer::extractText(const QJsonObject &args)
{
    const QString path = checkedPath(str(args, "path", true), true);
    if (Media::isVideo(path))
        fail(QStringLiteral("videos are not supported"));

    // Welche Seiten?
    int pageCount = 1;
    if (Media::isPdf(path))
        pageCount = std::max(1, Media::pdfPageCount(path));
    else
        pageCount = std::max(1, QImageReader(path).imageCount());
    QList<int> pages;
    if (boolean(args, "all_pages", false)) {
        for (int p = 0; p < std::min(pageCount, 500); ++p)
            pages << p;
    } else {
        for (const QJsonValue &v : args.value(QLatin1String("pages")).toArray())
            pages << v.toInt();
        if (pages.isEmpty())
            pages << 0;
    }
    for (int p : pages)
        if (p < 0 || p >= pageCount)
            fail(QStringLiteral("page %1 does not exist (file has %2 pages, 0-based)").arg(p).arg(pageCount));

    Ocr::Options opt;
    opt.languages = str(args, "languages").isEmpty() ? QStringLiteral("auto") : str(args, "languages");
    opt.words = boolean(args, "words", false);
    const bool forceOcr = boolean(args, "force_ocr", false);
    QRect region;
    if (args.value(QLatin1String("region")).isObject()) {
        const QJsonObject o = args.value(QLatin1String("region")).toObject();
        region = QRect(integer(o, "x", 0), integer(o, "y", 0), integer(o, "width", 0), integer(o, "height", 0));
        if (region.isEmpty())
            fail(QStringLiteral("region is empty"));
    }

    // 1. PDF-Textebene, wo vorhanden (exakt und schnell)
    QMap<int, QString> blocks;
    QMap<int, QJsonObject> metas;
    QList<int> ocrPages;
    for (int page : pages) {
        if (Media::isPdf(path) && !forceOcr && region.isEmpty()) {
            const QString text = Media::pdfPageText(path, page).trimmed();
            if (text.count(QRegularExpression(QStringLiteral("\\S"))) >= 20) {
                metas[page] = {{QStringLiteral("page"), page}, {QStringLiteral("method"), QStringLiteral("pdf text layer")}};
                blocks[page] = QStringLiteral("=== page %1 (pdf text layer) ===\n%2").arg(page).arg(text);
                continue;
            }
        }
        ocrPages << page;
    }

    // 2. Texterkennung in Hilfsprozessen: erste Seite bestimmt die Sprache,
    //    die übrigen laufen dann parallel mit dieser Sprache.
    if (!ocrPages.isEmpty()) {
        if (!Ocr::available())
            fail(QStringLiteral("OCR is not available in this build or no Tesseract language packs are installed"));
        auto makeJob = [&](int page, const QString &languages) {
            OcrWorker::Job job;
            job.path = path;
            job.page = page;
            job.pdfDpi = 300;
            job.region = region;
            job.options = opt;
            job.options.languages = languages;
            if (Media::isPdf(path))
                job.options.dpi = 300;
            return job;
        };
        QList<Ocr::Result> results;
        const Ocr::Result first = OcrWorker::run(makeJob(ocrPages.first(), opt.languages));
        if (!first.ok)
            fail(first.error + (first.hints.isEmpty() ? QString() : QStringLiteral(" – ") + first.hints.join(QStringLiteral("; "))));
        results << first;
        if (ocrPages.size() > 1) {
            QList<OcrWorker::Job> rest;
            const QString lang = opt.languages == QLatin1String("auto") ? first.languages : opt.languages;
            for (int i = 1; i < ocrPages.size(); ++i)
                rest << makeJob(ocrPages.at(i), lang);
            results += OcrWorker::runMany(rest, OcrWorker::defaultParallel());

            // Gemischte Dokumente (z. B. Fraktur-Anlagen): Seiten mit anderer
            // Schrift oder deutlich schlechterer Sicherheit selbst wählen lassen.
            if (opt.languages == QLatin1String("auto")) {
                QList<int> redo;
                QList<OcrWorker::Job> redoJobs;
                for (int i = 1; i < results.size(); ++i) {
                    const Ocr::Result &r = results.at(i);
                    if (r.ok && !r.text.isEmpty()
                        && (r.script != first.script || r.confidence < first.confidence - 8)) {
                        redo << i;
                        redoJobs << makeJob(ocrPages.at(i), QStringLiteral("auto"));
                    }
                }
                const QList<Ocr::Result> again = OcrWorker::runMany(redoJobs, OcrWorker::defaultParallel());
                for (int k = 0; k < redo.size(); ++k)
                    if (again.at(k).ok && again.at(k).confidence > results.at(redo.at(k)).confidence)
                        results[redo.at(k)] = again.at(k);
            }
        }

        for (int i = 0; i < ocrPages.size(); ++i) {
            const int page = ocrPages.at(i);
            const Ocr::Result &r = results.at(i);
            QJsonObject meta{{QStringLiteral("page"), page}, {QStringLiteral("method"), QStringLiteral("ocr")}};
            if (!r.ok) {
                meta.insert(QStringLiteral("error"), r.error);
                metas[page] = meta;
                blocks[page] = QStringLiteral("=== page %1 (ocr failed: %2) ===").arg(page).arg(r.error);
                continue;
            }
            meta.insert(QStringLiteral("languages"), r.languages);
            meta.insert(QStringLiteral("confidence"), std::round(r.confidence));
            meta.insert(QStringLiteral("script"), r.script);
            if (r.rotation)
                meta.insert(QStringLiteral("page_was_rotated_deg"), r.rotation);
            if (!r.textLanguage.isEmpty())
                meta.insert(QStringLiteral("text_language"), r.textLanguage + QStringLiteral(" (") + Ocr::languageName(r.textLanguage) + QLatin1Char(')'));
            if (!r.ranking.isEmpty()) {
                QJsonArray ranking;
                for (const Ocr::Candidate &c : r.ranking)
                    ranking.append(QJsonObject{{QStringLiteral("language"), c.language},
                                               {QStringLiteral("confidence"), std::round(c.confidence)},
                                               {QStringLiteral("common_words_pct"), std::round(c.stopwords * 1000) / 10}});
                meta.insert(QStringLiteral("language_candidates"), ranking);
            }
            if (!r.hints.isEmpty())
                meta.insert(QStringLiteral("hints"), QJsonArray::fromStringList(r.hints));
            metas[page] = meta;

            QString block = QStringLiteral("=== page %1 (ocr, %2, confidence %3) ===\n%4")
                                .arg(page).arg(r.languages).arg(qRound(r.confidence)).arg(r.text);
            if (opt.words) {
                QStringList w;
                for (const Ocr::Word &word : r.words)
                    w << QStringLiteral("%1\t%2\t%3,%4,%5,%6").arg(word.text).arg(word.confidence)
                             .arg(word.box.x()).arg(word.box.y()).arg(word.box.width()).arg(word.box.height());
                block += QStringLiteral("\n--- words (text, confidence, x,y,width,height) ---\n") + w.join(QLatin1Char('\n'));
            }
            blocks[page] = block;
        }
    }

    QJsonArray summary;
    QStringList out;
    for (int page : pages) {
        summary.append(metas.value(page));
        out << blocks.value(page);
    }
    return {textContent(QString::fromUtf8(QJsonDocument(summary).toJson(QJsonDocument::Compact)) + QLatin1Char('\n')
                        + out.join(QStringLiteral("\n\n")))};
}

QJsonArray McpServer::ocrLanguages(const QJsonObject &)
{
    const Ocr::Installed inst = Ocr::installed();
    QJsonArray langs;
    for (const QString &l : inst.languages)
        langs.append(QStringLiteral("%1 (%2)").arg(l, Ocr::languageName(l)));
    const QJsonObject info{
        {QStringLiteral("ocr_available"), Ocr::available()},
        {QStringLiteral("orientation_and_script_detection"), inst.osd},
        {QStringLiteral("installed"), langs},
        {QStringLiteral("how_to_install"), QStringLiteral("Debian/Ubuntu: sudo apt install tesseract-ocr-<code> "
                                                          "(e.g. tesseract-ocr-fra); all packs: tesseract-ocr-all")},
    };
    return {textContent(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Indented)))};
}

QJsonArray McpServer::splitPdf(const QJsonObject &args)
{
    const QString path = checkedPath(str(args, "path", true), true);
    if (!Media::isPdf(path))
        fail(QStringLiteral("not a PDF: %1").arg(path));
    if (!PdfSplit::available())
        fail(QStringLiteral("this build has no PDF splitting support (qpdf)"));

    PdfSplit::Options opt;
    if (args.contains(QLatin1String("max_megabytes")))
        opt.maxMegabytes = args.value(QLatin1String("max_megabytes")).toDouble(25);
    if (opt.maxMegabytes < 0.5)
        fail(QStringLiteral("max_megabytes must be at least 0.5"));
    opt.pagesPerPart = std::max(0, integer(args, "pages_per_part", 0));
    for (const QJsonValue &v : args.value(QLatin1String("ranges")).toArray()) {
        const QJsonArray pair = v.toArray();
        if (pair.size() != 2)
            fail(QStringLiteral("ranges must look like [[1,120],[121,300]]"));
        opt.ranges.append({pair.at(0).toInt(), pair.at(1).toInt()});
    }
    opt.preferBookmarks = boolean(args, "prefer_bookmarks", true);
    opt.overwrite = boolean(args, "overwrite", false);
    const QString outDir = str(args, "output_dir");
    if (!outDir.isEmpty())
        opt.outputDir = checkedPath(outDir, false);
    else
        checkedPath(QFileInfo(path).dir().filePath(QFileInfo(path).completeBaseName() + QStringLiteral("_Teile")), false);

    const PdfSplit::Result r = PdfSplit::split(path, opt);
    if (!r.ok)
        fail(r.error);

    QString text = QStringLiteral("split %1 (%2 pages, %3 MB) into %4 parts in %5\nmanifest: %6\nreadable summary: %7\n\n"
                                  "part\tpages (original)\tMB\tfile\n")
                       .arg(r.source).arg(r.sourcePages).arg(r.sourceBytes / 1e6, 0, 'f', 1).arg(r.parts.size())
                       .arg(QFileInfo(r.manifestJson).absolutePath(), r.manifestJson, r.manifestMarkdown);
    for (const PdfSplit::Part &p : r.parts)
        text += QStringLiteral("%1\t%2-%3\t%4\t%5%6\n").arg(p.number).arg(p.firstPage).arg(p.lastPage)
                    .arg(p.bytes / 1e6, 0, 'f', 1).arg(p.file).arg(p.oversize ? QStringLiteral("\t(single page above limit)") : QString());
    if (!r.warnings.isEmpty())
        text += QStringLiteral("\nwarnings:\n- ") + r.warnings.join(QStringLiteral("\n- "));
    return {textContent(text)};
}

QJsonArray McpServer::pdfParts(const QJsonObject &args)
{
    const QString path = checkedPath(str(args, "path", true), true);
    const QString manifestPath = PdfSplit::findManifest(path);

    if (manifestPath.isEmpty()) {
        // Ohne Protokoll: Angaben aus dem Teil selbst
        const PdfSplit::PartInfo info = PdfSplit::partInfo(path);
        if (info.part == 0)
            fail(QStringLiteral("no split manifest found for %1 and it is not a Kiek an part").arg(path));
        QString text = QStringLiteral("%1 is part %2 of %3 of '%4', original pages %5-%6 (manifest not found)")
                           .arg(path).arg(info.part).arg(info.of).arg(info.source).arg(info.firstPage).arg(info.lastPage);
        const int page = integer(args, "page", 0);
        if (page > 0) {
            if (page >= info.firstPage && page <= info.lastPage)
                text += QStringLiteral("\noriginal page %1 = page %2 of this part").arg(page).arg(page - info.firstPage + 1);
            else
                text += QStringLiteral("\noriginal page %1 is not in this part").arg(page);
        }
        return {textContent(text)};
    }

    QString error;
    const QJsonObject m = PdfSplit::loadManifest(manifestPath, &error);
    if (m.isEmpty())
        fail(error);
    const QDir dir = QFileInfo(manifestPath).dir();
    const QJsonObject src = m.value(QLatin1String("source")).toObject();
    const QJsonArray parts = m.value(QLatin1String("parts")).toArray();

    QString text = QStringLiteral("manifest: %1\nsource: %2 (%3 pages, split %4)\n")
                       .arg(manifestPath, src.value(QLatin1String("path")).toString())
                       .arg(src.value(QLatin1String("pages")).toInt()).arg(m.value(QLatin1String("created")).toString());

    const int page = integer(args, "page", 0);
    if (page > 0) {
        bool found = false;
        for (const QJsonValue &v : parts) {
            const QJsonObject p = v.toObject();
            const int a = p.value(QLatin1String("first_page")).toInt(), b = p.value(QLatin1String("last_page")).toInt();
            if (page >= a && page <= b) {
                text += QStringLiteral("original page %1 -> part %2: %3, page %4 of that file (0-based index %5)\n")
                            .arg(page).arg(p.value(QLatin1String("part")).toInt())
                            .arg(dir.filePath(p.value(QLatin1String("file")).toString())).arg(page - a + 1).arg(page - a);
                found = true;
            }
        }
        if (!found)
            text += QStringLiteral("original page %1 is not in any part\n").arg(page);
    }

    text += QStringLiteral("\npart\tpages (original)\tMB\tfile\tbookmarks\n");
    for (const QJsonValue &v : parts) {
        const QJsonObject p = v.toObject();
        QStringList bms;
        for (const QJsonValue &b : p.value(QLatin1String("bookmarks")).toArray())
            bms << QStringLiteral("%1 (p.%2)").arg(b.toObject().value(QLatin1String("title")).toString())
                       .arg(b.toObject().value(QLatin1String("page")).toInt());
        text += QStringLiteral("%1\t%2-%3\t%4\t%5\t%6\n").arg(p.value(QLatin1String("part")).toInt())
                    .arg(p.value(QLatin1String("first_page")).toInt()).arg(p.value(QLatin1String("last_page")).toInt())
                    .arg(p.value(QLatin1String("bytes")).toDouble() / 1e6, 0, 'f', 1)
                    .arg(dir.filePath(p.value(QLatin1String("file")).toString())).arg(bms.join(QStringLiteral("; ")));
    }
    if (boolean(args, "verify", false)) {
        const QStringList problems = PdfSplit::verify(m, manifestPath);
        text += problems.isEmpty() ? QStringLiteral("\nverify: all parts present and unchanged, source unchanged")
                                   : QStringLiteral("\nverify found problems:\n- ") + problems.join(QStringLiteral("\n- "));
    }
    return {textContent(text)};
}

// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PdfSplit.h"

#include "Media.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#ifdef KIEKAN_HAVE_QPDF
#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFOutlineDocumentHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#endif

#include <cmath>
#include <map>
#include <memory>

namespace PdfSplit {

namespace {

constexpr const char *kManifestSuffix = "_Teilung.json";

QString humanMB(qint64 bytes)
{
    return QStringLiteral("%1 MB").arg(bytes / 1e6, 0, 'f', 1);
}

#ifdef KIEKAN_HAVE_QPDF

std::string utf8(const QString &s)
{
    return s.toStdString();
}

// qpdf erwartet Dateinamen unter Windows als UTF-8, sonst in der Kodierung
// des Dateisystems.
QByteArray fileName(const QString &path)
{
#ifdef Q_OS_WIN
    return path.toUtf8();
#else
    return QFile::encodeName(path);
#endif
}

// Eine Teil-PDF im Speicher aufbauen (Seiten first..last, 0-basiert).
std::unique_ptr<QPDF> buildPart(QPDF &source, const std::vector<QPDFPageObjectHelper> &pages, int first, int last,
                                const QJsonObject &info)
{
    auto out = std::make_unique<QPDF>();
    out->emptyPDF();
    QPDFPageDocumentHelper helper(*out);
    for (int i = first; i <= last; ++i)
        helper.addPage(pages[size_t(i)], false);

    // Seitenbeschriftung: Seiten zeigen ihre Nummer im Original.
    out->getRoot().replaceKey("/PageLabels",
        QPDFObjectHandle::parse(QStringLiteral("<< /Nums [ 0 << /S /D /St %1 >> ] >>").arg(first + 1).toStdString()));

    // Dokumentinfo: wichtige Angaben der Quelle übernehmen, Teilangaben ergänzen.
    QPDFObjectHandle dict = QPDFObjectHandle::newDictionary();
    QPDFObjectHandle srcInfo = source.getTrailer().getKey("/Info");
    if (srcInfo.isDictionary()) {
        for (const char *key : {"/Author", "/CreationDate", "/Creator"}) {
            QPDFObjectHandle v = srcInfo.getKey(key);
            if (v.isString())
                dict.replaceKey(key, QPDFObjectHandle::newUnicodeString(v.getUTF8Value()));
        }
    }
    for (auto it = info.begin(); it != info.end(); ++it)
        dict.replaceKey("/" + it.key().toStdString(), QPDFObjectHandle::newUnicodeString(utf8(it.value().toString())));
    out->getTrailer().replaceKey("/Info", out->makeIndirectObject(dict));
    return out;
}

void configure(QPDFWriter &w)
{
    w.setObjectStreamMode(qpdf_o_generate);   // kompakter
    w.setCompressStreams(true);
    w.setStaticID(false);
}

qint64 sizeInMemory(QPDF &part)
{
    QPDFWriter w(part);
    configure(w);
    w.setOutputMemory();
    w.write();
    std::unique_ptr<Buffer> buffer(w.getBuffer());
    return qint64(buffer->getSize());
}

void collectOutlines(const std::vector<QPDFOutlineObjectHelper> &items, const std::map<QPDFObjGen, int> &pageIndex,
                     int level, QList<Bookmark> &out)
{
    for (const QPDFOutlineObjectHelper &o : items) {
        QPDFOutlineObjectHelper item = o;
        QPDFObjectHandle dest = item.getDestPage();
        if (dest.isDictionary()) {
            const auto it = pageIndex.find(dest.getObjGen());
            if (it != pageIndex.end())
                out.append({QString::fromStdString(item.getTitle()), it->second + 1, level});
        }
        if (level < 2)
            collectOutlines(item.getKids(), pageIndex, level + 1, out);
    }
}

#endif

QString firstText(const QString &pdf, int page)
{
    QString t = Media::pdfPageText(pdf, page).simplified();
    if (t.size() > 160)
        t = t.left(157) + QStringLiteral("…");
    return t;
}

QString markdownEscape(QString s)
{
    return s.replace(QLatin1Char('|'), QStringLiteral("\\|"));
}

} // namespace

QString sha256(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

#ifdef KIEKAN_HAVE_QPDF

bool available() { return true; }

int pageCount(const QString &path, QString *error)
{
    try {
        QPDF pdf;
        pdf.processFile(fileName(path).constData());
        return int(QPDFPageDocumentHelper(pdf).getAllPages().size());
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromLocal8Bit(e.what());
        return 0;
    }
}

Result split(const QString &path, const Options &opt, const std::function<void(const QString &)> &progress)
{
    Result r;
    const QFileInfo src(path);
    r.source = src.absoluteFilePath();
    r.sourceBytes = src.size();
    auto report = [&](const QString &t) { if (progress) progress(t); };

    try {
        QPDF pdf;
        pdf.processFile(fileName(r.source).constData());
        const std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
        const int n = int(pages.size());
        r.sourcePages = n;
        if (n == 0) {
            r.error = QStringLiteral("the PDF has no pages");
            return r;
        }

        // Lesezeichen mit Seitennummern
        QList<Bookmark> bookmarks;
        {
            std::map<QPDFObjGen, int> pageIndex;
            for (int i = 0; i < n; ++i)
                pageIndex[pages[size_t(i)].getObjectHandle().getObjGen()] = i;
            QPDFOutlineDocumentHelper outlines(pdf);
            if (outlines.hasOutlines())
                collectOutlines(outlines.getTopLevelOutlines(), pageIndex, 0, bookmarks);
        }

        // Teilgrenzen bestimmen (0-basiert, einschließlich)
        QList<QPair<int, int>> bounds;
        QList<bool> oversize;
        const qint64 limit = qint64(opt.maxMegabytes * 1e6);
        if (!opt.ranges.isEmpty()) {
            for (const auto &rg : opt.ranges) {
                const int a = std::clamp(rg.first, 1, n) - 1, b = std::clamp(rg.second, 1, n) - 1;
                if (a > b) {
                    r.error = QStringLiteral("invalid range %1-%2").arg(rg.first).arg(rg.second);
                    return r;
                }
                bounds.append({a, b});
                oversize.append(false);
            }
        } else if (opt.pagesPerPart > 0) {
            for (int a = 0; a < n; a += opt.pagesPerPart) {
                bounds.append({a, std::min(n - 1, a + opt.pagesPerPart - 1)});
                oversize.append(false);
            }
        } else {
            if (limit < 100000) {
                r.error = QStringLiteral("size limit too small");
                return r;
            }
            const QJsonObject noInfo;
            // Teilgröße wird tatsächlich gemessen (Schriften/Bilder, die sich
            // Seiten teilen, machen Schätzungen unzuverlässig).
            auto measure = [&](int a, int b) {
                std::unique_ptr<QPDF> part = buildPart(pdf, pages, a, b, noInfo);
                return sizeInMemory(*part);
            };
            int start = 0;
            while (start < n) {
                report(QStringLiteral("plane Teil %1 ab Seite %2 …").arg(bounds.size() + 1).arg(start + 1));
                const qint64 single = measure(start, start);
                if (single > limit) {
                    bounds.append({start, start});
                    oversize.append(true);
                    ++start;
                    continue;
                }
                // Exponentiell vergrößern, dann binär eingrenzen.
                int good = start;
                int bad = -1;
                int step = std::max(1, int(double(limit) / std::max<qint64>(single, 1) * 0.8));
                int cand = std::min(n - 1, start + step - 1);
                while (true) {
                    if (cand <= good) {
                        if (good == n - 1) break;
                        cand = good + 1;
                    }
                    if (measure(start, cand) <= limit) {
                        good = cand;
                        if (good == n - 1)
                            break;
                        cand = std::min(n - 1, start + (cand - start + 1) * 2 - 1);
                    } else {
                        bad = cand;
                        break;
                    }
                }
                if (bad > 0) {
                    while (bad - good > 1) {
                        const int mid = (good + bad) / 2;
                        if (measure(start, mid) <= limit)
                            good = mid;
                        else
                            bad = mid;
                    }
                }
                int end = good;
                // Möglichst vor einem Lesezeichen schneiden (Abschnittsanfang),
                // wenn dabei höchstens 40 % der möglichen Seiten verloren gehen.
                if (opt.preferBookmarks && end < n - 1) {
                    const int minEnd = start + int((end - start) * 0.6);
                    int bestCut = -1;
                    for (const Bookmark &bm : bookmarks) {
                        const int b = bm.page - 1;   // Abschnitt beginnt hier
                        if (bm.level <= 1 && b - 1 >= minEnd && b - 1 < end && b > start)
                            bestCut = std::max(bestCut, b - 1);
                    }
                    if (bestCut >= start)
                        end = bestCut;
                }
                bounds.append({start, end});
                oversize.append(false);
                start = end + 1;
            }
        }

        // Ausgabeordner
        const QString base = src.completeBaseName();
        const QString outDir = opt.outputDir.isEmpty() ? src.dir().filePath(base + QStringLiteral("_Teile")) : opt.outputDir;
        if (!QDir().mkpath(outDir)) {
            r.error = QStringLiteral("cannot create %1").arg(outDir);
            return r;
        }
        const int count = int(bounds.size());
        const int pageDigits = std::max(3, int(QString::number(n).size()));
        const int partDigits = std::max(2, int(QString::number(count).size()));
        r.manifestJson = QDir(outDir).filePath(base + QString::fromLatin1(kManifestSuffix));
        r.manifestMarkdown = QDir(outDir).filePath(base + QStringLiteral("_Teilung.md"));
        if (!opt.overwrite && QFileInfo::exists(r.manifestJson)) {
            r.error = QStringLiteral("this PDF was already split into %1 (pass overwrite to replace)").arg(outDir);
            return r;
        }

        report(QStringLiteral("berechne Prüfsumme der Quelle …"));
        const QString sourceHash = sha256(r.source);
        QString sourceTitle;
        QPDFObjectHandle srcInfo = pdf.getTrailer().getKey("/Info");
        if (srcInfo.isDictionary() && srcInfo.getKey("/Title").isString())
            sourceTitle = QString::fromStdString(srcInfo.getKey("/Title").getUTF8Value()).trimmed();
        const QString titleBase = sourceTitle.isEmpty() ? base : sourceTitle;

        for (int i = 0; i < count; ++i) {
            const int a = bounds.at(i).first, b = bounds.at(i).second;
            Part part;
            part.number = i + 1;
            part.firstPage = a + 1;
            part.lastPage = b + 1;
            part.oversize = oversize.at(i);
            part.file = QStringLiteral("%1_Teil-%2-von-%3_S%4-%5.pdf").arg(base)
                            .arg(i + 1, partDigits, 10, QLatin1Char('0')).arg(count, partDigits, 10, QLatin1Char('0'))
                            .arg(a + 1, pageDigits, 10, QLatin1Char('0')).arg(b + 1, pageDigits, 10, QLatin1Char('0'));
            for (const Bookmark &bm : bookmarks)
                if (bm.page >= part.firstPage && bm.page <= part.lastPage)
                    part.bookmarks.append(bm);

            const QString target = QDir(outDir).filePath(part.file);
            if (QFileInfo::exists(target) && !opt.overwrite) {
                r.error = QStringLiteral("file exists: %1").arg(target);
                return r;
            }
            report(QStringLiteral("schreibe Teil %1 von %2 (Seiten %3–%4) …").arg(i + 1).arg(count).arg(a + 1).arg(b + 1));
            const QJsonObject info{
                {QStringLiteral("Title"), QStringLiteral("%1 – Teil %2 von %3 (S. %4–%5)").arg(titleBase).arg(i + 1).arg(count).arg(a + 1).arg(b + 1)},
                {QStringLiteral("Subject"), QStringLiteral("Teil %1 von %2 aus „%3“, Seiten %4–%5 von %6")
                                                .arg(i + 1).arg(count).arg(src.fileName()).arg(a + 1).arg(b + 1).arg(n)},
                {QStringLiteral("Keywords"), QStringLiteral("kiekan-teil=%1/%2; seiten=%3-%4; quelle=%5; quelle-seiten=%6; quelle-sha256=%7")
                                                 .arg(i + 1).arg(count).arg(a + 1).arg(b + 1).arg(src.fileName()).arg(n).arg(sourceHash)},
                {QStringLiteral("Producer"), QStringLiteral("KiekAn! %1 (qpdf)").arg(QCoreApplication::applicationVersion())},
            };
            std::unique_ptr<QPDF> out = buildPart(pdf, pages, a, b, info);
            const QString tmp = target + QStringLiteral(".part");
            {
                QPDFWriter w(*out, fileName(tmp).constData());
                configure(w);
                w.write();
            }
            QFile::remove(target);
            if (!QFile::rename(tmp, target)) {
                r.error = QStringLiteral("cannot write %1").arg(target);
                return r;
            }
            part.bytes = QFileInfo(target).size();
            part.sha256 = sha256(target);
            if (part.oversize)
                r.warnings << QStringLiteral("page %1 alone is %2, larger than the limit of %3")
                                  .arg(part.firstPage).arg(humanMB(part.bytes)).arg(humanMB(limit));
            else if (opt.ranges.isEmpty() && opt.pagesPerPart <= 0 && part.bytes > limit)
                r.warnings << QStringLiteral("part %1 is %2, above the limit").arg(part.number).arg(humanMB(part.bytes));
            r.parts.append(part);
        }

        // Protokoll (JSON)
        QJsonArray partsJson;
        for (const Part &p : r.parts) {
            QJsonArray bms;
            for (const Bookmark &bm : p.bookmarks)
                bms.append(QJsonObject{{QStringLiteral("title"), bm.title}, {QStringLiteral("page"), bm.page},
                                       {QStringLiteral("page_in_part"), bm.page - p.firstPage + 1}, {QStringLiteral("level"), bm.level}});
            QJsonObject o{{QStringLiteral("part"), p.number}, {QStringLiteral("file"), p.file},
                          {QStringLiteral("first_page"), p.firstPage}, {QStringLiteral("last_page"), p.lastPage},
                          {QStringLiteral("pages"), p.lastPage - p.firstPage + 1}, {QStringLiteral("bytes"), p.bytes},
                          {QStringLiteral("sha256"), p.sha256}, {QStringLiteral("bookmarks"), bms}};
            const QString text = firstText(r.source, p.firstPage - 1);
            if (!text.isEmpty())
                o.insert(QStringLiteral("first_page_text"), text);
            if (p.oversize)
                o.insert(QStringLiteral("oversize"), true);
            partsJson.append(o);
        }
        QJsonObject settings{{QStringLiteral("preferred_cut_at_bookmarks"), opt.preferBookmarks}};
        if (!opt.ranges.isEmpty()) {
            settings.insert(QStringLiteral("mode"), QStringLiteral("ranges"));
        } else if (opt.pagesPerPart > 0) {
            settings.insert(QStringLiteral("mode"), QStringLiteral("pages"));
            settings.insert(QStringLiteral("pages_per_part"), opt.pagesPerPart);
        } else {
            settings.insert(QStringLiteral("mode"), QStringLiteral("size"));
            settings.insert(QStringLiteral("max_megabytes"), opt.maxMegabytes);
        }

        const QJsonObject manifest{
            {QStringLiteral("format"), QStringLiteral("kiekan-pdf-split/1")},
            {QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate)},
            {QStringLiteral("tool"), QStringLiteral("KiekAn! %1").arg(QCoreApplication::applicationVersion())},
            {QStringLiteral("source"), QJsonObject{{QStringLiteral("path"), r.source}, {QStringLiteral("file"), src.fileName()},
                                                   {QStringLiteral("title"), sourceTitle}, {QStringLiteral("pages"), n},
                                                   {QStringLiteral("bytes"), r.sourceBytes}, {QStringLiteral("sha256"), sourceHash},
                                                   {QStringLiteral("modified"), src.lastModified().toString(Qt::ISODate)}}},
            {QStringLiteral("settings"), settings},
            {QStringLiteral("parts"), partsJson},
            {QStringLiteral("note"), QStringLiteral("Page numbers are 1-based and refer to the original. Inside each part, "
                                                    "PDF page labels show the original numbers. Original page P is in the part "
                                                    "with first_page <= P <= last_page, at page P - first_page + 1 of that file.")},
        };
        QFile jf(r.manifestJson);
        if (!jf.open(QIODevice::WriteOnly)) {
            r.error = QStringLiteral("cannot write %1").arg(r.manifestJson);
            return r;
        }
        jf.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
        jf.close();

        // Protokoll (Markdown, für Menschen)
        QString md;
        md += QStringLiteral("# Teilung von „%1“\n\n").arg(src.fileName());
        md += QStringLiteral("- Quelle: `%1`\n").arg(r.source);
        if (!sourceTitle.isEmpty())
            md += QStringLiteral("- Titel: %1\n").arg(sourceTitle);
        md += QStringLiteral("- Seiten: %1 · Größe: %2\n").arg(n).arg(humanMB(r.sourceBytes));
        md += QStringLiteral("- SHA-256: `%1`\n").arg(sourceHash);
        md += QStringLiteral("- Geteilt am %1 mit KiekAn! %2\n").arg(QDateTime::currentDateTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")),
                                                                       QCoreApplication::applicationVersion());
        if (opt.ranges.isEmpty() && opt.pagesPerPart <= 0)
            md += QStringLiteral("- Regel: höchstens %1 MB je Teil%2\n").arg(opt.maxMegabytes)
                      .arg(opt.preferBookmarks ? QStringLiteral(", bevorzugt an Lesezeichen") : QString());
        else if (opt.pagesPerPart > 0)
            md += QStringLiteral("- Regel: %1 Seiten je Teil\n").arg(opt.pagesPerPart);
        else
            md += QStringLiteral("- Regel: vorgegebene Seitenbereiche\n");
        md += QStringLiteral("\n| Teil | Datei | Originalseiten | Seiten | Größe |\n|---|---|---|---|---|\n");
        for (const Part &p : r.parts)
            md += QStringLiteral("| %1 | `%2` | %3–%4 | %5 | %6%7 |\n").arg(p.number).arg(p.file).arg(p.firstPage).arg(p.lastPage)
                      .arg(p.lastPage - p.firstPage + 1).arg(humanMB(p.bytes)).arg(p.oversize ? QStringLiteral(" ⚠") : QString());
        bool anyBookmarks = false;
        for (const Part &p : r.parts)
            anyBookmarks = anyBookmarks || !p.bookmarks.isEmpty();
        if (anyBookmarks) {
            md += QStringLiteral("\n## Lesezeichen\n\n| Teil | Originalseite | Seite im Teil | Lesezeichen |\n|---|---|---|---|\n");
            for (const Part &p : r.parts)
                for (const Bookmark &bm : p.bookmarks)
                    md += QStringLiteral("| %1 | %2 | %3 | %4%5 |\n").arg(p.number).arg(bm.page).arg(bm.page - p.firstPage + 1)
                              .arg(QString(bm.level * 2, QLatin1Char(' ')).replace(QLatin1String("  "), QStringLiteral("– ")))
                              .arg(markdownEscape(bm.title));
        }
        md += QStringLiteral("\n## Seiten finden\n\nOriginalseite *P* liegt in dem Teil, dessen Bereich *P* enthält, "
                             "dort auf Seite *P − erste Seite + 1*. In PDF-Betrachtern zeigen die Teile die "
                             "Originalseitenzahlen an. Jeder Teil nennt Quelle und Bereich auch in seinen "
                             "Dokumenteigenschaften (Titel, Thema, Stichwörter).\n");
        if (!r.warnings.isEmpty()) {
            md += QStringLiteral("\n## Hinweise\n\n");
            for (const QString &w : r.warnings)
                md += QStringLiteral("- %1\n").arg(w);
        }
        QFile mf(r.manifestMarkdown);
        if (mf.open(QIODevice::WriteOnly | QIODevice::Text))
            mf.write(md.toUtf8());

        r.ok = true;
        return r;
    } catch (const std::exception &e) {
        r.error = QString::fromLocal8Bit(e.what());
        return r;
    }
}

PartInfo partInfo(const QString &pdfPath)
{
    PartInfo info;
    try {
        QPDF pdf;
        pdf.processFile(fileName(pdfPath).constData());
        QPDFObjectHandle dict = pdf.getTrailer().getKey("/Info");
        if (!dict.isDictionary() || !dict.getKey("/Keywords").isString())
            return info;
        const QString kw = QString::fromStdString(dict.getKey("/Keywords").getUTF8Value());
        static const QRegularExpression partRe(QStringLiteral("kiekan-teil=(\\d+)/(\\d+)"));
        static const QRegularExpression pagesRe(QStringLiteral("seiten=(\\d+)-(\\d+)"));
        static const QRegularExpression srcRe(QStringLiteral("quelle=([^;]+)"));
        const auto m = partRe.match(kw);
        if (!m.hasMatch())
            return info;
        info.part = m.captured(1).toInt();
        info.of = m.captured(2).toInt();
        const auto p = pagesRe.match(kw);
        if (p.hasMatch()) {
            info.firstPage = p.captured(1).toInt();
            info.lastPage = p.captured(2).toInt();
        }
        const auto s = srcRe.match(kw);
        if (s.hasMatch())
            info.source = s.captured(1).trimmed();
    } catch (const std::exception &) {
    }
    return info;
}

#else

bool available() { return false; }
int pageCount(const QString &, QString *error)
{
    if (error)
        *error = QStringLiteral("this build has no PDF splitting support (qpdf)");
    return 0;
}
Result split(const QString &, const Options &, const std::function<void(const QString &)> &)
{
    Result r;
    r.error = QStringLiteral("this build has no PDF splitting support (qpdf)");
    return r;
}
PartInfo partInfo(const QString &) { return {}; }

#endif

QString findManifest(const QString &path)
{
    const QFileInfo fi(path);
    if (fi.isFile() && fi.fileName().endsWith(QString::fromLatin1(kManifestSuffix)))
        return fi.absoluteFilePath();

    auto newestIn = [](const QDir &dir, const QString &mustContainFile) -> QString {
        QString best;
        QDateTime bestTime;
        for (const QFileInfo &m : dir.entryInfoList({QStringLiteral("*") + QString::fromLatin1(kManifestSuffix)}, QDir::Files)) {
            if (!mustContainFile.isEmpty()) {
                QFile f(m.absoluteFilePath());
                if (!f.open(QIODevice::ReadOnly) || !f.readAll().contains(mustContainFile.toUtf8()))
                    continue;
            }
            if (best.isEmpty() || m.lastModified() > bestTime) {
                best = m.absoluteFilePath();
                bestTime = m.lastModified();
            }
        }
        return best;
    };

    if (fi.isDir())
        return newestIn(QDir(fi.absoluteFilePath()), QString());
    if (!fi.isFile())
        return {};
    // Ein Teil: Protokoll liegt im selben Ordner und nennt die Datei.
    QString m = newestIn(fi.dir(), QStringLiteral("\"") + fi.fileName() + QStringLiteral("\""));
    if (!m.isEmpty())
        return m;
    // Die Quelle: Standardordner <Name>_Teile neben der Datei.
    const QDir partsDir(fi.dir().filePath(fi.completeBaseName() + QStringLiteral("_Teile")));
    m = newestIn(partsDir, QStringLiteral("\"") + fi.fileName() + QStringLiteral("\""));
    return m;
}

QJsonObject loadManifest(const QString &manifestPath, QString *error)
{
    QFile f(manifestPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read %1").arg(manifestPath);
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject() || doc.object().value(QLatin1String("format")).toString() != QLatin1String("kiekan-pdf-split/1")) {
        if (error)
            *error = QStringLiteral("not a KiekAn! split manifest: %1").arg(manifestPath);
        return {};
    }
    return doc.object();
}

QStringList verify(const QJsonObject &manifest, const QString &manifestPath)
{
    QStringList problems;
    const QDir dir = QFileInfo(manifestPath).dir();
    for (const QJsonValue &v : manifest.value(QLatin1String("parts")).toArray()) {
        const QJsonObject p = v.toObject();
        const QString file = dir.filePath(p.value(QLatin1String("file")).toString());
        if (!QFileInfo::exists(file))
            problems << QStringLiteral("part %1 is missing: %2").arg(p.value(QLatin1String("part")).toInt()).arg(file);
        else if (sha256(file) != p.value(QLatin1String("sha256")).toString())
            problems << QStringLiteral("part %1 was changed since the split: %2").arg(p.value(QLatin1String("part")).toInt()).arg(file);
    }
    const QJsonObject src = manifest.value(QLatin1String("source")).toObject();
    const QString srcPath = src.value(QLatin1String("path")).toString();
    if (!QFileInfo::exists(srcPath))
        problems << QStringLiteral("source not found at %1 (moved or deleted)").arg(srcPath);
    else if (sha256(srcPath) != src.value(QLatin1String("sha256")).toString())
        problems << QStringLiteral("source was changed since the split: %1").arg(srcPath);
    return problems;
}

} // namespace PdfSplit

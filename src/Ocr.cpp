// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ocr.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#ifdef KIEKAN_HAVE_TESSERACT
#include <tesseract/baseapi.h>
#endif

namespace Ocr {

namespace {

struct Profile
{
    const char *code;   // Tesseract-Code
    const char *name;
    const char *words;  // typische, häufige Kurzwörter
};

// Füllwörter je Sprache – genug, um die Sprache eines Textabschnitts sicher
// zu erkennen, auch wenn er mit einem fremden Sprachmodell gelesen wurde.
const Profile kProfiles[] = {
    {"deu", "Deutsch", "der die das und ist nicht mit sich des auf für ein eine einen im dem den zu von wird werden auch als bei oder wir sie es ich nach aus wie über"},
    {"eng", "English", "the and of to in is that for it with as was on be by this are not or from at which have an has were their been they he she we you"},
    {"fra", "Français", "le la les des est et en un une du que qui dans pour pas sur au avec ce sont par plus nous vous il elle aux cette être"},
    {"ita", "Italiano", "il la di che e un una per non sono del della le gli con si da al nel alla anche come più questo"},
    {"spa", "Español", "el la de que y en los las del se por un una con para es al lo como más pero sus le ya"},
    {"por", "Português", "o a de que e do da em um uma para com não os as dos das no na por mais se foi ao"},
    {"nld", "Nederlands", "de het een en van is dat niet op te zijn voor met die in aan er ook als bij zich naar"},
    {"pol", "Polski", "i w na z się nie do że to jest o jak po od za ale czy jego przez być tak"},
    {"ces", "Čeština", "a se na je že v to s z do o by jako ale jsou pro jeho jsem také nebo"},
    {"swe", "Svenska", "och att det som en på är av för med den till inte har de om ett var men"},
    {"dan", "Dansk", "og at det er en på til af med som for ikke den har de om et var men jeg"},
    {"nor", "Norsk", "og i det er en på til av som for med ikke den har de om et var men jeg"},
    {"fin", "Suomi", "ja on ei se että oli hän mutta kun niin myös tai ovat hänen joka mikä kuin"},
    {"tur", "Türkçe", "ve bir bu da de için ile çok ne olarak gibi daha en ama kadar sonra"},
    {"hun", "Magyar", "a az és hogy nem is egy meg van ez de csak volt már mint még"},
    {"ron", "Română", "și de la în cu nu pe o un este care se din a mai fost sau"},
    {"rus", "Русский", "и в не на что с по как он это к но из у за от то же все она так"},
};

// Welche Sprachpakete zu welcher Schrift (laut OSD) passen.
[[maybe_unused]] QStringList candidatesForScript(const QString &script)
{
    static const QHash<QString, QStringList> map = {
        {QStringLiteral("Latin"), {QStringLiteral("deu"), QStringLiteral("eng"), QStringLiteral("fra"), QStringLiteral("ita"),
                                   QStringLiteral("spa"), QStringLiteral("por"), QStringLiteral("nld"), QStringLiteral("pol"),
                                   QStringLiteral("ces"), QStringLiteral("slk"), QStringLiteral("swe"), QStringLiteral("dan"),
                                   QStringLiteral("nor"), QStringLiteral("fin"), QStringLiteral("tur"), QStringLiteral("hun"),
                                   QStringLiteral("ron"), QStringLiteral("hrv"), QStringLiteral("slv"), QStringLiteral("cat"),
                                   QStringLiteral("lat")}},
        {QStringLiteral("Fraktur"), {QStringLiteral("deu_latf"), QStringLiteral("frk"), QStringLiteral("deu")}},
        {QStringLiteral("Cyrillic"), {QStringLiteral("rus"), QStringLiteral("ukr"), QStringLiteral("bul"), QStringLiteral("srp"), QStringLiteral("bel"), QStringLiteral("mkd")}},
        {QStringLiteral("Greek"), {QStringLiteral("ell"), QStringLiteral("grc")}},
        {QStringLiteral("Arabic"), {QStringLiteral("ara"), QStringLiteral("fas"), QStringLiteral("urd")}},
        {QStringLiteral("Hebrew"), {QStringLiteral("heb")}},
        {QStringLiteral("Han"), {QStringLiteral("chi_sim"), QStringLiteral("chi_tra"), QStringLiteral("jpn")}},
        {QStringLiteral("Japanese"), {QStringLiteral("jpn")}},
        {QStringLiteral("Katakana"), {QStringLiteral("jpn")}},
        {QStringLiteral("Hiragana"), {QStringLiteral("jpn")}},
        {QStringLiteral("Hangul"), {QStringLiteral("kor")}},
        {QStringLiteral("Devanagari"), {QStringLiteral("hin"), QStringLiteral("mar"), QStringLiteral("nep"), QStringLiteral("san")}},
        {QStringLiteral("Thai"), {QStringLiteral("tha")}},
    };
    return map.value(script);
}

// Anteil typischer Füllwörter je Sprache im Text.
QList<QPair<QString, double>> detectTextLanguage(const QString &text, int *wordCount)
{
    static QHash<QString, QSet<QString>> sets;
    if (sets.isEmpty()) {
        for (const Profile &p : kProfiles) {
            QSet<QString> s;
            for (const QString &w : QString::fromUtf8(p.words).split(QLatin1Char(' ')))
                s.insert(w);
            sets.insert(QString::fromLatin1(p.code), s);
        }
    }
    static const QRegularExpression splitter(QStringLiteral("[^\\p{L}]+"));
    const QStringList words = text.toLower().split(splitter, Qt::SkipEmptyParts);
    *wordCount = int(words.size());
    QList<QPair<QString, double>> scores;
    if (words.isEmpty())
        return scores;
    for (auto it = sets.cbegin(); it != sets.cend(); ++it) {
        int hits = 0;
        for (const QString &w : words)
            if (it.value().contains(w))
                ++hits;
        scores.append({it.key(), double(hits) / words.size()});
    }
    std::sort(scores.begin(), scores.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    return scores;
}

[[maybe_unused]] double stopwordShare(const QString &text, const QString &language)
{
    int n = 0;
    const QString base = language.section(QLatin1Char('_'), 0, 0);   // deu_latf -> deu
    for (const auto &s : detectTextLanguage(text, &n))
        if (s.first == base)
            return s.second;
    return 0;
}

#ifdef KIEKAN_HAVE_TESSERACT

struct Engine
{
    tesseract::TessBaseAPI api;
    bool ok = false;

    explicit Engine(const QString &languages)
    {
        ok = api.Init(nullptr, languages.toUtf8().constData(), tesseract::OEM_LSTM_ONLY) == 0;
        if (!ok)
            ok = api.Init(nullptr, languages.toUtf8().constData(), tesseract::OEM_DEFAULT) == 0;
    }
    ~Engine() { api.End(); }

    void setImage(const QImage &gray, int dpi)
    {
        api.SetImage(gray.constBits(), gray.width(), gray.height(), 1, int(gray.bytesPerLine()));
        api.SetSourceResolution(dpi);
    }
};

QString takeText(char *raw)
{
    const QString s = QString::fromUtf8(raw ? raw : "");
    delete[] raw;
    return s;
}

// Probe- oder Volltexterkennung; liefert Text und mittlere Sicherheit.
QString run(const QString &languages, const QImage &gray, int dpi, double *confidence)
{
    Engine e(languages);
    if (!e.ok) {
        *confidence = 0;
        return {};
    }
    e.api.SetPageSegMode(tesseract::PSM_AUTO);
    e.setImage(gray, dpi);
    if (e.api.Recognize(nullptr) != 0) {
        *confidence = 0;
        return {};
    }
    *confidence = e.api.MeanTextConf();
    return takeText(e.api.GetUTF8Text());
}

#endif

} // namespace

QString languageName(const QString &code)
{
    if (code == QLatin1String("deu_latf") || code == QLatin1String("frk"))
        return QStringLiteral("Deutsch (Fraktur)");
    for (const Profile &p : kProfiles)
        if (code == QLatin1String(p.code))
            return QString::fromUtf8(p.name);
    return code;
}

namespace {

enum class Distro { Debian, Fedora, Suse, Arch, Gentoo, Bundled };

// Paketfamilie aus /etc/os-release; Flatpak und Windows bringen die
// Sprachdaten selbst mit.
Distro distro()
{
    static const Distro d = [] {
#ifdef Q_OS_WIN
        return Distro::Bundled;
#else
        if (QFile::exists(QStringLiteral("/.flatpak-info")))
            return Distro::Bundled;
        QFile f(QStringLiteral("/etc/os-release"));
        QString ids;
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            for (const QByteArray &line : f.readAll().split('\n'))
                if (line.startsWith("ID=") || line.startsWith("ID_LIKE="))
                    ids += QString::fromUtf8(line) + QLatin1Char(' ');
        if (ids.contains(QLatin1String("suse")))
            return Distro::Suse;
        if (ids.contains(QLatin1String("fedora")) || ids.contains(QLatin1String("rhel")))
            return Distro::Fedora;
        if (ids.contains(QLatin1String("arch")))
            return Distro::Arch;
        if (ids.contains(QLatin1String("gentoo")))
            return Distro::Gentoo;
        return Distro::Debian;
#endif
    }();
    return d;
}

} // namespace

QString packageName(const QString &code)
{
    QString c = code;
    switch (distro()) {
    case Distro::Fedora:
        return c == QLatin1String("osd") ? QStringLiteral("tesseract-osd") : QStringLiteral("tesseract-langpack-") + c;
    case Distro::Suse:
        return QStringLiteral("tesseract-ocr-traineddata-") + c;
    case Distro::Arch:
        return QStringLiteral("tesseract-data-") + c;
    case Distro::Gentoo:
        return QStringLiteral("app-text/tessdata_fast");
    case Distro::Bundled:
        return c + QStringLiteral(".traineddata");
    case Distro::Debian:
        break;
    }
    return QStringLiteral("tesseract-ocr-") + c.replace(QLatin1Char('_'), QLatin1Char('-')).toLower();
}

QString installCommand(const QString &code)
{
    const QString pkg = packageName(code);
    switch (distro()) {
    case Distro::Fedora:
        return QStringLiteral("sudo dnf install ") + pkg;
    case Distro::Suse:
        return QStringLiteral("sudo zypper install ") + pkg;
    case Distro::Arch:
        return QStringLiteral("sudo pacman -S ") + pkg;
    case Distro::Gentoo:
        // Sprachen wählt Gentoo über L10N (z. B. L10N="de en" in make.conf).
        return QStringLiteral("L10N=\"…\" (/etc/portage/make.conf); sudo emerge --changed-use ") + pkg;
    case Distro::Bundled:
        return QStringLiteral("%1 → tessdata (TESSDATA_PREFIX)").arg(pkg);
    case Distro::Debian:
        break;
    }
    return QStringLiteral("sudo apt install ") + pkg;
}

#ifdef KIEKAN_HAVE_TESSERACT

bool available()
{
    return !installed().languages.isEmpty();
}

Installed installed()
{
    // Sprachpakete = *.traineddata im Datenordner von Tesseract. Direkt
    // nachsehen ist einfacher als die je nach Version andere API. Einmal
    // ermittelt (thread-sicher über die statische Initialisierung).
    static const Installed cache = [] {
        Installed info;
        tesseract::TessBaseAPI api;
        if (api.Init(nullptr, "osd", tesseract::OEM_DEFAULT) != 0 && api.Init(nullptr, "eng") != 0) {
            api.End();
            return info;
        }
        const QDir dir(QString::fromLocal8Bit(api.GetDatapath()));
        api.End();
        for (const QString &f : dir.entryList({QStringLiteral("*.traineddata")}, QDir::Files)) {
            const QString code = f.chopped(int(strlen(".traineddata")));
            if (code == QLatin1String("osd"))
                info.osd = true;
            else if (code != QLatin1String("equ"))
                info.languages << code;
        }
        info.languages.sort();
        return info;
    }();
    return cache;
}

Result recognize(const QImage &input, const Options &options)
{
    Result r;
    if (input.isNull()) {
        r.error = QStringLiteral("empty image");
        return r;
    }
    const Installed inst = installed();
    if (inst.languages.isEmpty()) {
        r.error = QStringLiteral("no Tesseract language packs installed");
        r.hints << QStringLiteral("install e.g. %1 and %2").arg(packageName(QStringLiteral("deu")), packageName(QStringLiteral("eng")));
        return r;
    }

    // Graustufen; kleine Bilder (Bildschirmfotos) verdoppeln – Tesseract
    // braucht etwa 20 px Zeichenhöhe.
    QImage gray = input.convertToFormat(QImage::Format_Grayscale8);
    int dpi = options.dpi;
    if (dpi <= 0) {
        dpi = int(std::lround(input.dotsPerMeterX() * 0.0254));
        if (dpi < 70 || dpi > 1200)
            dpi = 300;
    }
    if (std::max(gray.width(), gray.height()) < 1200) {
        gray = gray.scaled(gray.size() * 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        dpi *= 2;
    }
    const double backScale = double(input.width()) / gray.width();

    // 1. Lage und Schrift
    r.script = QStringLiteral("Latin");
    if (inst.osd) {
        tesseract::TessBaseAPI osd;
        if (osd.Init(nullptr, "osd", tesseract::OEM_DEFAULT) == 0) {
            osd.SetPageSegMode(tesseract::PSM_OSD_ONLY);
            osd.SetImage(gray.constBits(), gray.width(), gray.height(), 1, int(gray.bytesPerLine()));
            osd.SetSourceResolution(dpi);
            int orient = 0;
            float orientConf = 0, scriptConf = 0;
            const char *script = nullptr;
            if (osd.DetectOrientationScript(&orient, &orientConf, &script, &scriptConf)) {
                if (script && scriptConf > 1.0f)
                    r.script = QString::fromLatin1(script);
                // orient = Drehung des Bildes im Uhrzeigersinn; zurückdrehen.
                if (orient != 0 && orientConf > 1.5f)
                    r.rotation = (360 - orient) % 360;
            }
            osd.End();
        }
    }
    QTransform toUpright;
    if (r.rotation != 0) {
        toUpright = QImage::trueMatrix(QTransform().rotate(r.rotation), gray.width(), gray.height());
        gray = gray.transformed(QTransform().rotate(r.rotation));
    }

    // 2./3. Sprachwahl
    QString chosen = options.languages.trimmed();
    if (chosen.isEmpty() || chosen == QLatin1String("auto")) {
        // Probe: mittlerer Streifen (spart Zeit, enthält fast immer Fließtext)
        QImage probe = gray;
        if (gray.height() > 1400)
            probe = gray.copy(0, gray.height() / 4, gray.width(), gray.height() / 2);

        QStringList scriptCands;
        for (const QString &c : candidatesForScript(r.script))
            if (inst.languages.contains(c))
                scriptCands << c;
        if (scriptCands.isEmpty()) {
            const QStringList wanted = candidatesForScript(r.script);
            if (!wanted.isEmpty())
                r.hints << QStringLiteral("text uses %1 script but no matching language pack is installed; install e.g. %2")
                               .arg(r.script, packageName(wanted.first()));
            scriptCands = inst.languages;   // bestes Bemühen mit dem, was da ist
        }

        // Erste Probe mit einem allgemeinen Modell, um die Textsprache an
        // Füllwörtern zu erkennen.
        const QString generic = scriptCands.contains(QLatin1String("eng")) ? QStringLiteral("eng") : scriptCands.first();
        double genericConf = 0;
        const QString genericText = run(generic, probe, dpi, &genericConf);
        int words = 0;
        const auto textLangs = detectTextLanguage(genericText, &words);
        if (!textLangs.isEmpty() && words >= 8 && textLangs.first().second >= 0.06)
            r.textLanguage = textLangs.first().first;

        // Kandidaten: erkannte Textsprache(n), Systemsprache, Englisch, Fraktur-Variante
        QStringList toTry;
        auto consider = [&](const QString &c) {
            if (scriptCands.contains(c) && !toTry.contains(c) && toTry.size() < 4)
                toTry << c;
        };
        for (int i = 0; i < std::min<int>(2, int(textLangs.size())); ++i)
            if (textLangs.at(i).second >= 0.04)
                consider(textLangs.at(i).first);
        if (!r.textLanguage.isEmpty() && !scriptCands.contains(r.textLanguage))
            r.hints << QStringLiteral("the text looks %1 but the pack '%2' is not installed (%3); results use another language")
                           .arg(languageName(r.textLanguage), r.textLanguage, packageName(r.textLanguage));
        if (toTry.contains(QLatin1String("deu")) || r.script == QLatin1String("Fraktur")) {
            consider(QStringLiteral("deu_latf"));
            consider(QStringLiteral("frk"));
        }
        consider(QLocale::system().name().left(2) == QLatin1String("de") ? QStringLiteral("deu") : QStringLiteral("eng"));
        consider(generic);
        for (const QString &c : scriptCands)
            consider(c);

        // Kandidaten gleichzeitig prüfen: jeder Lauf ist einfädig (siehe
        // OMP_THREAD_LIMIT im Hilfsprozess), also lohnt die Parallelität.
        std::vector<QString> texts(size_t(toTry.size()));
        std::vector<double> confs(size_t(toTry.size()), 0.0);
        {
            std::vector<std::thread> threads;
            for (int i = 0; i < toTry.size(); ++i) {
                if (toTry.at(i) == generic) {
                    texts[size_t(i)] = genericText;
                    confs[size_t(i)] = genericConf;
                    continue;
                }
                threads.emplace_back([&, i] { texts[size_t(i)] = run(toTry.at(i), probe, dpi, &confs[size_t(i)]); });
            }
            for (auto &t : threads)
                t.join();
        }
        for (int i = 0; i < toTry.size(); ++i) {
            const QString &lang = toTry.at(i);
            const QString &text = texts[size_t(i)];
            Candidate c;
            c.language = lang;
            c.confidence = confs[size_t(i)];
            c.stopwords = stopwordShare(text, lang);
            // Ist die Textsprache erkannt, aber ihr Paket fehlt, sagen die
            // Füllwörter der Ersatzkandidaten nichts aus: nur Sicherheit zählt.
            const bool textLangMissing = !r.textLanguage.isEmpty() && !scriptCands.contains(r.textLanguage);
            c.score = c.confidence / 100.0 + (textLangMissing ? 0.0 : 0.6 * std::min(1.0, c.stopwords / 0.25));
            r.ranking.append(c);
        }
        std::sort(r.ranking.begin(), r.ranking.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });

        chosen = r.ranking.isEmpty() ? generic : r.ranking.first().language;

        // Alle Kandidaten scheitern: eher Handschrift oder ein sehr schlechter
        // Scan als eine fehlende Sprache – dann keine Paketempfehlung, die
        // Schrifterkennung rät in solchen Fällen oft wild (z. B. „Arabic“).
        if (!r.ranking.isEmpty() && r.ranking.first().confidence < 40) {
            r.hints.erase(std::remove_if(r.hints.begin(), r.hints.end(), [](const QString &h) {
                return h.startsWith(QLatin1String("text uses")) || h.startsWith(QLatin1String("the text looks"));
            }), r.hints.end());
            r.hints << QStringLiteral("all language packs read this poorly: the text is probably handwritten or the "
                                      "scan is badly degraded; Tesseract only reads printed text");
            r.script = QStringLiteral("unknown");
        }
        // Mischtext: zweite Sprache mit eigenem Füllwort-Anteil dazunehmen.
        if (textLangs.size() >= 2 && words >= 20) {
            const auto &second = textLangs.at(1);
            const QString firstBase = chosen.section(QLatin1Char('_'), 0, 0);
            if (second.first != firstBase && second.second >= 0.05 && second.second >= textLangs.first().second * 0.45
                && inst.languages.contains(second.first))
                chosen += QLatin1Char('+') + second.first;
        }
    } else {
        for (const QString &l : chosen.split(QLatin1Char('+')))
            if (!inst.languages.contains(l)) {
                r.error = QStringLiteral("language pack '%1' is not installed (%2); installed: %3")
                              .arg(l, packageName(l), inst.languages.join(QStringLiteral(", ")));
                return r;
            }
    }

    // Volltext
    Engine e(chosen);
    if (!e.ok) {
        r.error = QStringLiteral("Tesseract could not load '%1'").arg(chosen);
        return r;
    }
    e.api.SetPageSegMode(tesseract::PSM_AUTO);
    e.setImage(gray, dpi);
    if (e.api.Recognize(nullptr) != 0) {
        r.error = QStringLiteral("recognition failed");
        return r;
    }
    r.text = takeText(e.api.GetUTF8Text()).trimmed();
    r.confidence = e.api.MeanTextConf();
    r.languages = chosen;

    if (options.words) {
        // Wortrahmen zurück in die Koordinaten des Eingabebildes rechnen.
        const QTransform back = toUpright.inverted();
        std::unique_ptr<tesseract::ResultIterator> it(e.api.GetIterator());
        if (it) {
            do {
                const char *w = it->GetUTF8Text(tesseract::RIL_WORD);
                if (!w)
                    continue;
                Word word;
                word.text = QString::fromUtf8(w);
                delete[] w;
                word.confidence = int(it->Confidence(tesseract::RIL_WORD));
                int l, t, rr, b;
                it->BoundingBox(tesseract::RIL_WORD, &l, &t, &rr, &b);
                const QRectF upright(QPointF(l, t), QPointF(rr, b));
                const QRectF orig = back.mapRect(upright);
                word.box = QRectF(orig.x() * backScale, orig.y() * backScale,
                                  orig.width() * backScale, orig.height() * backScale).toAlignedRect();
                r.words.append(word);
            } while (it->Next(tesseract::RIL_WORD));
        }
    }

    if (r.text.isEmpty())
        r.hints << QStringLiteral("no text found");
    else if (r.confidence < 60)
        r.hints << QStringLiteral("low confidence: the scan may be blurry, low-resolution or in another language");
    r.ok = true;
    return r;
}

#else

bool available() { return false; }
Installed installed() { return {}; }
Result recognize(const QImage &, const Options &)
{
    Result r;
    r.error = QStringLiteral("this build has no OCR support (Tesseract)");
    return r;
}

#endif

} // namespace Ocr

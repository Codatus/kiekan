// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OcrWorker.h"

#include "ImageLoader.h"
#include "Media.h"

#include <QCoreApplication>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace OcrWorker {

namespace {

constexpr int kTimeoutMs = 10 * 60 * 1000;   // eine Seite, großzügig

QJsonObject jobToJson(const Job &j)
{
    return {
        {QStringLiteral("path"), j.path},
        {QStringLiteral("page"), j.page},
        {QStringLiteral("pdf_dpi"), j.pdfDpi},
        {QStringLiteral("coord_dpi"), j.coordDpi},
        {QStringLiteral("image_file"), j.imageFile},
        {QStringLiteral("region"), QJsonArray{j.region.x(), j.region.y(), j.region.width(), j.region.height()}},
        {QStringLiteral("languages"), j.options.languages},
        {QStringLiteral("words"), j.options.words},
        {QStringLiteral("dpi"), j.options.dpi},
        {QStringLiteral("thorough"), j.options.thorough},
    };
}

Job jobFromJson(const QJsonObject &o)
{
    Job j;
    j.path = o.value(QLatin1String("path")).toString();
    j.page = o.value(QLatin1String("page")).toInt();
    j.pdfDpi = o.value(QLatin1String("pdf_dpi")).toDouble(300);
    j.coordDpi = o.value(QLatin1String("coord_dpi")).toDouble(0);
    j.imageFile = o.value(QLatin1String("image_file")).toString();
    const QJsonArray r = o.value(QLatin1String("region")).toArray();
    if (r.size() == 4)
        j.region = QRect(r.at(0).toInt(), r.at(1).toInt(), r.at(2).toInt(), r.at(3).toInt());
    j.options.languages = o.value(QLatin1String("languages")).toString(QStringLiteral("auto"));
    j.options.words = o.value(QLatin1String("words")).toBool();
    j.options.dpi = o.value(QLatin1String("dpi")).toInt();
    j.options.thorough = o.value(QLatin1String("thorough")).toBool();
    return j;
}

QJsonObject resultToJson(const Ocr::Result &r)
{
    QJsonArray ranking;
    for (const Ocr::Candidate &c : r.ranking)
        ranking.append(QJsonObject{{QStringLiteral("l"), c.language}, {QStringLiteral("c"), c.confidence},
                                   {QStringLiteral("s"), c.stopwords}, {QStringLiteral("score"), c.score}});
    QJsonArray words;
    for (const Ocr::Word &w : r.words)
        words.append(QJsonArray{w.text, w.confidence, w.box.x(), w.box.y(), w.box.width(), w.box.height(), w.line, w.para});
    return {
        {QStringLiteral("ok"), r.ok}, {QStringLiteral("error"), r.error}, {QStringLiteral("text"), r.text},
        {QStringLiteral("languages"), r.languages}, {QStringLiteral("confidence"), r.confidence},
        {QStringLiteral("rotation"), r.rotation}, {QStringLiteral("script"), r.script},
        {QStringLiteral("text_language"), r.textLanguage}, {QStringLiteral("ranking"), ranking},
        {QStringLiteral("hints"), QJsonArray::fromStringList(r.hints)}, {QStringLiteral("words"), words},
    };
}

Ocr::Result resultFromJson(const QJsonObject &o)
{
    Ocr::Result r;
    r.ok = o.value(QLatin1String("ok")).toBool();
    r.error = o.value(QLatin1String("error")).toString();
    r.text = o.value(QLatin1String("text")).toString();
    r.languages = o.value(QLatin1String("languages")).toString();
    r.confidence = o.value(QLatin1String("confidence")).toDouble();
    r.rotation = o.value(QLatin1String("rotation")).toInt();
    r.script = o.value(QLatin1String("script")).toString();
    r.textLanguage = o.value(QLatin1String("text_language")).toString();
    for (const QJsonValue &v : o.value(QLatin1String("ranking")).toArray()) {
        const QJsonObject c = v.toObject();
        r.ranking.append({c.value(QLatin1String("l")).toString(), c.value(QLatin1String("c")).toDouble(),
                          c.value(QLatin1String("s")).toDouble(), c.value(QLatin1String("score")).toDouble()});
    }
    for (const QJsonValue &v : o.value(QLatin1String("hints")).toArray())
        r.hints << v.toString();
    for (const QJsonValue &v : o.value(QLatin1String("words")).toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() >= 6)
            r.words.append({a.at(0).toString(), a.at(1).toInt(), QRect(a.at(2).toInt(), a.at(3).toInt(), a.at(4).toInt(), a.at(5).toInt()),
                            a.at(6).toInt(), a.at(7).toInt()});
    }
    return r;
}

Ocr::Result failure(const QString &message)
{
    Ocr::Result r;
    r.error = message;
    return r;
}

QProcess *startWorker(const Job &job)
{
    auto *p = new QProcess;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!env.contains(QStringLiteral("OMP_THREAD_LIMIT")))
        env.insert(QStringLiteral("OMP_THREAD_LIMIT"), QStringLiteral("1"));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    p->setProcessEnvironment(env);
    p->setProcessChannelMode(QProcess::SeparateChannels);
    p->start(QCoreApplication::applicationFilePath(), {QStringLiteral("--ocr-worker")});
    if (p->waitForStarted(10000)) {
        p->write(QJsonDocument(jobToJson(job)).toJson(QJsonDocument::Compact));
        p->closeWriteChannel();
    }
    return p;
}

// Fortschrittszeilen „P <Prozent>“ des Hilfsprozesses auswerten; alles
// andere auf stderr bleibt für Fehlermeldungen liegen.
void readProgress(QProcess *p, QByteArray &stderrText, const std::function<void(int)> &progress)
{
    stderrText += p->readAllStandardError();
    qsizetype nl;
    QByteArray rest;
    while ((nl = stderrText.indexOf('\n')) >= 0) {
        const QByteArray line = stderrText.left(nl);
        stderrText.remove(0, nl + 1);
        if (line.startsWith("P ")) {
            if (progress)
                progress(line.mid(2).toInt());
        } else {
            rest += line + '\n';
        }
    }
    stderrText.prepend(rest);
}

Ocr::Result collect(QProcess *p, const std::atomic<bool> *cancel = nullptr,
                    const std::function<void(int)> &progress = {})
{
    QElapsedTimer timer;
    timer.start();
    QByteArray stderrText;
    while (p->state() != QProcess::NotRunning && !p->waitForFinished(100)) {
        if (progress)
            readProgress(p, stderrText, progress);
        const bool cancelled = cancel && *cancel;
        if (cancelled || timer.elapsed() > kTimeoutMs) {
            p->kill();
            p->waitForFinished(2000);
            return failure(cancelled ? QStringLiteral("cancelled") : QStringLiteral("OCR worker timed out"));
        }
    }
    if (p->exitStatus() != QProcess::NormalExit)
        return failure(QStringLiteral("OCR worker crashed"));
    const QJsonDocument doc = QJsonDocument::fromJson(p->readAllStandardOutput());
    if (!doc.isObject())
        return failure(QStringLiteral("OCR worker gave no result: %1")
                           .arg(QString::fromLocal8Bit(stderrText + p->readAllStandardError()).trimmed().right(300)));
    return resultFromJson(doc.object());
}

} // namespace

int workerMain()
{
    QFile in;
    in.open(stdin, QIODevice::ReadOnly);
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll());
    Ocr::Result r;
    if (!doc.isObject()) {
        r = failure(QStringLiteral("invalid job"));
    } else {
        const Job job = jobFromJson(doc.object());
        QImage image = job.imageFile.isEmpty() ? ImageLoader::decode(job.path, job.page, job.pdfDpi).image
                                               : QImage(job.imageFile);
        if (image.isNull()) {
            r = failure(QStringLiteral("cannot read image"));
        } else {
            // PDF in höherer Auflösung erkannt als die Koordinaten der übrigen
            // Werkzeuge: Bereich hin-, Wortrahmen zurückrechnen.
            double fx = 1, fy = 1;
            if (job.imageFile.isEmpty() && job.coordDpi > 0 && job.coordDpi != job.pdfDpi && Media::isPdf(job.path)) {
                const QSize ref = ImageLoader::pdfPageSize(job.path, job.page, job.coordDpi);
                if (ref.isValid()) {
                    fx = double(image.width()) / ref.width();
                    fy = double(image.height()) / ref.height();
                }
            }
            auto toImage = [&](const QRect &q) {
                return QRect(QPoint(int(std::floor(q.left() * fx)), int(std::floor(q.top() * fy))),
                             QPoint(int(std::ceil((q.right() + 1) * fx)) - 1, int(std::ceil((q.bottom() + 1) * fy)) - 1));
            };
            QPoint offset;
            if (job.region.isValid() && !job.region.isEmpty()) {
                const QRect clipped = toImage(job.region).intersected(image.rect());
                image = image.copy(clipped);
                offset = clipped.topLeft();   // tatsächlicher Ausschnitt, auch am Rand
            }
            Ocr::setProgressCallback([](int percent) {
                std::fprintf(stderr, "P %d\n", percent);
                std::fflush(stderr);
            });
            r = Ocr::recognize(image, job.options);
            // Wortrahmen auf das ganze Bild und die Koordinaten des Aufrufers beziehen
            for (Ocr::Word &w : r.words) {
                const QRect b = w.box.translated(offset);
                w.box = QRect(QPoint(int(std::lround(b.left() / fx)), int(std::lround(b.top() / fy))),
                              QSize(std::max(1, int(std::lround(b.width() / fx))), std::max(1, int(std::lround(b.height() / fy)))));
            }
        }
    }
    const QByteArray out = QJsonDocument(resultToJson(r)).toJson(QJsonDocument::Compact);
    std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
    std::fflush(stdout);
    return 0;
}

Ocr::Result run(const Job &job, const std::atomic<bool> *cancel, const std::function<void(int)> &progress)
{
    std::unique_ptr<QProcess> p(startWorker(job));
    return collect(p.get(), cancel, progress);
}

QList<Ocr::Result> runMany(const QList<Job> &jobs, int parallel, const std::function<void(int, int)> &progress)
{
    QList<Ocr::Result> results(jobs.size());
    struct Running
    {
        int index;
        std::unique_ptr<QProcess> process;
        QElapsedTimer started;
    };
    std::vector<Running> running;
    int next = 0, done = 0;
    parallel = std::max(1, parallel);
    while (done < jobs.size()) {
        while (next < jobs.size() && int(running.size()) < parallel) {
            Running r{next, std::unique_ptr<QProcess>(startWorker(jobs.at(next))), {}};
            r.started.start();
            running.push_back(std::move(r));
            ++next;
        }
        // Auf den ersten fertigen Prozess warten (kurze Abfragen, damit auch
        // spätere Aufträge, die früher fertig werden, Platz machen).
        bool finishedOne = false;
        while (!finishedOne) {
            for (auto it = running.begin(); it != running.end(); ++it) {
                QProcess *p = it->process.get();
                const bool overdue = it->started.elapsed() > kTimeoutMs;
                if (p->state() == QProcess::NotRunning || p->waitForFinished(25) || overdue) {
                    if (overdue && p->state() != QProcess::NotRunning) {
                        p->kill();
                        p->waitForFinished(2000);
                        results[it->index] = failure(QStringLiteral("OCR worker timed out"));
                    } else {
                        results[it->index] = collect(p);
                    }
                    running.erase(it);
                    ++done;
                    if (progress)
                        progress(done, int(jobs.size()));
                    finishedOne = true;
                    break;
                }
            }
        }
    }
    return results;
}

int defaultParallel()
{
    const int setting = QSettings().value(QStringLiteral("ocr/jobs"), 0).toInt();
    if (setting > 0)
        return setting;
    return std::clamp(QThread::idealThreadCount(), 1, 8);
}

} // namespace OcrWorker

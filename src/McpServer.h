// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include "AiGuard.h"

// MCP-Server (Model Context Protocol) über stdin/stdout: macht die
// Bildfunktionen von KiekAn! für KI-Assistenten wie Claude nutzbar.
// Start: kiekan --mcp
class McpServer : public QObject
{
    Q_OBJECT

public:
    explicit McpServer(QObject *parent = nullptr);

    // Startet das Lesen von stdin; beendet die Anwendung bei Eingabeende.
    void start();

    // Für Tests: eine JSON-RPC-Nachricht verarbeiten, Antwort zurückgeben
    // (leer bei Benachrichtigungen).
    QJsonObject handleMessage(const QJsonObject &message);

private:
    void handleLine(const QByteArray &line);
    void send(const QJsonObject &message);

    QJsonObject callTool(const QString &name, const QJsonObject &args);
    void logCall(const QString &tool, const QJsonObject &args, const QStringList &read, const QStringList &written,
                 const QList<AiGuard::Finding> &findings, const QStringList &sources,
                 const QList<AiGuard::Finding> &trustedFindings, const QString &error);

    // Werkzeuge
    QJsonArray listMedia(const QJsonObject &args);
    QJsonArray imageInfo(const QJsonObject &args);
    QJsonArray viewImage(const QJsonObject &args);
    QJsonArray readMetadata(const QJsonObject &args);
    QJsonArray histogram(const QJsonObject &args);
    QJsonArray listFilters(const QJsonObject &args);
    QJsonArray editImage(const QJsonObject &args);
    QJsonArray convertBatch(const QJsonObject &args);
    QJsonArray viewerShow(const QJsonObject &args);
    QJsonArray viewerState(const QJsonObject &args);
    QJsonArray viewerGetImage(const QJsonObject &args);
    QJsonArray viewerSelect(const QJsonObject &args);
    QJsonArray viewerApply(const QJsonObject &args);
    QJsonArray contactSheet(const QJsonObject &args);
    QJsonArray writeMetadata(const QJsonObject &args);
    QJsonArray extractText(const QJsonObject &args);
    QJsonArray ocrLanguages(const QJsonObject &args);
    QJsonArray splitPdf(const QJsonObject &args);
    QJsonArray pdfParts(const QJsonObject &args);

    QString m_client;   // Name des verbundenen Programms (aus initialize)
};

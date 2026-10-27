// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ViewerBridge.h"

#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>

namespace ViewerBridge {

QString socketName()
{
    // Eigener Name, z. B. für Testinstanzen, die ein laufendes KiekAn! des
    // Benutzers nicht stören sollen (Fenster und `kiekan --mcp` gleich setzen).
    const QString custom = qEnvironmentVariable("KIEKAN_VIEWER_SOCKET");
    if (!custom.isEmpty())
        return custom;
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty())
        user = qEnvironmentVariable("USERNAME");
    return QStringLiteral("kiekan-viewer-") + user;
}

QJsonObject request(const QJsonObject &req, int timeoutMs, QString *error)
{
    QLocalSocket socket;
    socket.connectToServer(socketName());
    if (!socket.waitForConnected(1000)) {
        if (error)
            *error = QStringLiteral("no KiekAn! window is running");
        return {};
    }
    socket.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    socket.flush();

    QByteArray buffer;
    while (!buffer.contains('\n')) {
        if (!socket.waitForReadyRead(timeoutMs)) {
            if (error)
                *error = QStringLiteral("KiekAn! window did not answer in time");
            return {};
        }
        buffer += socket.readAll();
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(buffer.left(buffer.indexOf('\n')), &parseError);
    if (!doc.isObject()) {
        if (error)
            *error = QStringLiteral("invalid answer from KiekAn! window");
        return {};
    }
    return doc.object();
}

} // namespace ViewerBridge

ViewerServer::ViewerServer(Handler handler, QObject *parent)
    : QObject(parent)
    , m_server(new QLocalServer(this))
    , m_handler(std::move(handler))
{
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &ViewerServer::onNewConnection);
}

ViewerServer::~ViewerServer() = default;

bool ViewerServer::start()
{
    if (m_server->isListening())
        return true;
    const QString name = ViewerBridge::socketName();

    // Lauscht schon ein anderes Fenster? Dann dessen Socket nicht anfassen.
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(200)) {
        probe.abort();
        return false;
    }
    QLocalServer::removeServer(name);   // verwaister Socket nach Absturz
    return m_server->listen(name);
}

void ViewerServer::stop()
{
    m_server->close();
}

bool ViewerServer::isListening() const
{
    return m_server->isListening();
}

void ViewerServer::onNewConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            if (!socket->canReadLine())
                return;
            const QByteArray line = socket->readLine();
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            QJsonObject answer;
            if (doc.isObject())
                answer = m_handler(doc.object());
            else
                answer = {{QStringLiteral("error"), QStringLiteral("invalid request")}};
            socket->write(QJsonDocument(answer).toJson(QJsonDocument::Compact) + '\n');
            socket->flush();
            socket->disconnectFromServer();
        });
    }
}

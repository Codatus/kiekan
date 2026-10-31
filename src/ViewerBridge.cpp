// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ViewerBridge.h"

#include <QDeadlineTimer>
#include <QSettings>
#include <QSaveFile>
#include <QFile>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ViewerBridge {

QString socketName()
{
    // Eigener Name, z. B. für Testinstanzen, die ein laufendes KiekAn! des
    // Benutzers nicht stören sollen (Fenster und `kiekan --mcp` gleich setzen).
    const QString custom = qEnvironmentVariable("KIEKAN_VIEWER_SOCKET");
    if (!custom.isEmpty())
        return custom;
#if defined(Q_OS_UNIX)
    // Nicht im gemeinsamen /tmp, sondern im privaten Laufzeitordner des
    // Benutzers (XDG_RUNTIME_DIR, Rechte 0700) – dort kann kein anderer
    // Benutzer den Namen vorher belegen. Im Flatpak den Ordner, den alle
    // Instanzen derselben App sehen.
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) {
        // Manche Programme (z. B. Claude Desktop) starten `kiekan --mcp` ohne
        // XDG_RUNTIME_DIR. Dann den üblichen Ort /run/user/<uid> nehmen – wie
        // das Fenster –, aber nur, wenn er allein uns gehört.
        const QString standard = QStringLiteral("/run/user/%1").arg(getuid());
        const QFileInfo fi(standard);
        const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        if (fi.isDir() && fi.ownerId() == getuid() && !(fi.permissions() & others))
            runtime = standard;
    }
    const QString flatpak = qEnvironmentVariable("FLATPAK_ID");
    if (!flatpak.isEmpty() && !runtime.isEmpty())
        return QDir(runtime).filePath(QStringLiteral("app/%1/kiekan-viewer").arg(flatpak));
    if (!runtime.isEmpty())
        return QDir(runtime).filePath(QStringLiteral("kiekan-viewer"));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!dir.isEmpty())
        return QDir(dir).filePath(QStringLiteral("kiekan-viewer"));
#endif
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty())
        user = qEnvironmentVariable("USERNAME");
    return QStringLiteral("kiekan-viewer-") + user;
}

// Eigener Socket je Fenster und die Datei, in der das zuletzt benutzte Fenster
// steht – nur bei einem Pfad im privaten Laufzeitordner (nicht unter Windows
// und nicht bei kurzen Namen aus KIEKAN_VIEWER_SOCKET).
QString windowSocketName(qint64 pid)
{
    const QString base = socketName();
    return QDir::isAbsolutePath(base) ? base + QLatin1Char('.') + QString::number(pid) : QString();
}

QString activeFile()
{
    const QString base = socketName();
    return QDir::isAbsolutePath(base) ? base + QStringLiteral(".aktiv") : QString();
}

void markActive()
{
    const QString file = activeFile();
    if (file.isEmpty())
        return;
    QSaveFile f(file);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QByteArray::number(QCoreApplication::applicationPid()));
        f.commit();
    }
}

bool preferLastUsed()
{
    return QSettings().value(QStringLiteral("ai/viewerLastUsed"), true).toBool();
}

namespace {

// Gehört die Gegenseite dem eigenen Benutzer?
bool peerIsSelf(const QLocalSocket &socket)
{
#if defined(Q_OS_LINUX)
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (getsockopt(int(socket.socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return false;
    return cred.uid == getuid();
#elif defined(Q_OS_UNIX)
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(int(socket.socketDescriptor()), &uid, &gid) != 0)
        return false;
    return uid == getuid();
#else
    Q_UNUSED(socket);
    return true;   // Windows: Named Pipe mit Standard-ACL des Benutzers
#endif
}

constexpr qint64 kMaxAnswer = 64 * 1024 * 1024;

} // namespace

QJsonObject request(const QJsonObject &req, int timeoutMs, QString *error)
{
    QLocalSocket socket;
    bool connected = false;
    // Einstellung „zuletzt benutztes Fenster“: dessen eigenen Socket nehmen.
    if (preferLastUsed()) {
        QFile f(activeFile());
        if (!activeFile().isEmpty() && f.open(QIODevice::ReadOnly)) {
            const qint64 pid = f.read(32).trimmed().toLongLong();
            if (pid > 0) {
                socket.connectToServer(windowSocketName(pid));
                connected = socket.waitForConnected(500);
            }
        }
    }
    if (!connected) {
        socket.abort();
        socket.connectToServer(socketName());
        connected = socket.waitForConnected(1000);
    }
    if (!connected) {
        if (error)
            *error = QStringLiteral("no KiekAn! window is running");
        return {};
    }
    if (!peerIsSelf(socket)) {
        if (error)
            *error = QStringLiteral("the viewer socket %1 belongs to another user - not used").arg(socketName());
        return {};
    }
    socket.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    socket.flush();

    QByteArray buffer;
    const QDeadlineTimer deadline(timeoutMs);
    while (!buffer.contains('\n')) {
        if (deadline.hasExpired() || !socket.waitForReadyRead(int(std::max<qint64>(1, deadline.remainingTime())))) {
            if (error)
                *error = QStringLiteral("KiekAn! window did not answer in time");
            return {};
        }
        buffer += socket.readAll();
        if (buffer.size() > kMaxAnswer) {
            if (error)
                *error = QStringLiteral("answer from KiekAn! window is too large");
            return {};
        }
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
    , m_own(new QLocalServer(this))
    , m_handler(std::move(handler))
{
    for (QLocalServer *server : {m_server, m_own}) {
        server->setSocketOptions(QLocalServer::UserAccessOption);
        connect(server, &QLocalServer::newConnection, this, [this, server] { onNewConnection(server); });
    }
}

ViewerServer::~ViewerServer() = default;

bool ViewerServer::start()
{
    // Eigener Socket dieses Fensters (für „zuletzt benutztes Fenster“)
    const QString own = ViewerBridge::windowSocketName(QCoreApplication::applicationPid());
    if (!own.isEmpty() && !m_own->isListening()) {
        QLocalServer::removeServer(own);
        m_own->listen(own);
    }
    if (m_server->isListening())
        return true;
    const QString name = ViewerBridge::socketName();

    // Lauscht schon ein anderes Fenster? Dann dessen Socket nicht anfassen.
    QLocalSocket probe;
    probe.connectToServer(name);
    m_error.clear();
    if (probe.waitForConnected(200)) {
        const bool own = ViewerBridge::peerIsSelf(probe);
        probe.abort();
        if (!own)
            m_error = QStringLiteral("%1 belongs to another user").arg(name);
        return false;
    }
    QLocalServer::removeServer(name);   // verwaister Socket nach Absturz
    if (m_server->listen(name))
        return true;
    m_error = m_server->errorString();
    return false;
}

QString ViewerServer::errorString() const
{
    return m_error;
}

void ViewerServer::stop()
{
    m_server->close();
    m_own->close();
}

bool ViewerServer::isListening() const
{
    return m_server->isListening();
}

void ViewerServer::onNewConnection(QLocalServer *server)

{
    while (QLocalSocket *socket = server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        if (!ViewerBridge::peerIsSelf(*socket)) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            if (!socket->canReadLine()) {
                if (socket->bytesAvailable() > ViewerBridge::kMaxAnswer)
                    socket->abort();
                return;
            }
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

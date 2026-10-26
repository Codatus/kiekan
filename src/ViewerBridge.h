// Kiek an – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QObject>

#include <functional>

class QLocalServer;

// Verbindung zwischen der KI-Schnittstelle (kiekan --mcp) und einem offenen
// Kiek-an-Fenster: eine JSON-Zeile hin, eine JSON-Zeile zurück, über einen
// lokalen Socket, auf den nur der eigene Benutzer zugreifen darf.
namespace ViewerBridge {

QString socketName();

// Sendet eine Anfrage an das laufende Fenster. Bei Fehlern (kein Fenster
// offen, Zeitüberschreitung) leeres Objekt und Text in `error`.
QJsonObject request(const QJsonObject &req, int timeoutMs, QString *error);

} // namespace ViewerBridge

class ViewerServer : public QObject
{
    Q_OBJECT

public:
    using Handler = std::function<QJsonObject(const QJsonObject &)>;

    ViewerServer(Handler handler, QObject *parent = nullptr);
    ~ViewerServer() override;

    // false, wenn bereits ein anderes Fenster lauscht oder der Socket nicht
    // angelegt werden kann.
    bool start();
    void stop();
    bool isListening() const;

private:
    void onNewConnection();

    QLocalServer *m_server;
    Handler m_handler;
};

// KiekAn! – schneller Bildbetrachter
// Copyright (C) 2026 Carl Kliefert
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QObject>

#include <functional>

class QLocalServer;

// Verbindung zwischen der KI-Schnittstelle (kiekan --mcp) und einem offenen
// KiekAn!-Fenster: eine JSON-Zeile hin, eine JSON-Zeile zurück, über einen
// lokalen Socket, auf den nur der eigene Benutzer zugreifen darf.
namespace ViewerBridge {

QString socketName();
QString windowSocketName(qint64 pid);   // eigener Socket je Fenster (leer = nicht möglich)
QString activeFile();                   // hält die PID des zuletzt benutzten Fensters
void markActive();                      // dieses Fenster als zuletzt benutzt eintragen
bool preferLastUsed();                  // Einstellung: KI spricht mit dem zuletzt benutzten Fenster

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
    QString errorString() const;   // leer, wenn nur schon ein Fenster lauscht

private:
    void onNewConnection(QLocalServer *server);

    QLocalServer *m_server;   // gemeinsamer Name: das zuerst geöffnete Fenster
    QLocalServer *m_own;      // eigener Name je Fenster
    Handler m_handler;
    QString m_error;
};

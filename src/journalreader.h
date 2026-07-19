// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QProcess>

class JournalReader final : public QObject
{
    Q_OBJECT

public:
    explicit JournalReader(QObject *parent = nullptr);
    ~JournalReader() override;
    void start();

Q_SIGNALS:
    void messageReceived(const QString &message, const QDateTime &timestamp);
    void errorOccurred(const QString &message);

private Q_SLOTS:
    void readOutput();
    void processLine(const QByteArray &line);

private:
    QProcess m_process;
    QByteArray m_buffer;
};

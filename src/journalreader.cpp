// SPDX-License-Identifier: GPL-3.0-only

#include "journalreader.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

JournalReader::JournalReader(QObject *parent)
    : QObject(parent)
{
    // journalctl is kept as a child process; all output is converted into Qt signals.
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &JournalReader::readOutput);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        Q_EMIT errorOccurred(m_process.errorString());
    });
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        const QString error = QString::fromLocal8Bit(m_process.readAllStandardError()).trimmed();
        if (!error.isEmpty()) {
            Q_EMIT errorOccurred(error);
        }
    });
}

JournalReader::~JournalReader()
{
    // Give journalctl a chance to exit cleanly, then force termination during shutdown.
    if (m_process.state() == QProcess::NotRunning) {
        return;
    }
    m_process.terminate();
    if (!m_process.waitForFinished(1000)) {
        m_process.kill();
        m_process.waitForFinished();
    }
}

void JournalReader::start()
{
    if (m_process.state() != QProcess::NotRunning) {
        return;
    }

    // JSON output preserves both the message and journald's microsecond timestamp.
    m_process.start(QStringLiteral("journalctl"), {
        QStringLiteral("--user-unit=onedrive.service"),
        QStringLiteral("--follow"),
        QStringLiteral("--lines=50"),
        QStringLiteral("--output=json"),
        QStringLiteral("--no-pager"),
    });
}

void JournalReader::readOutput()
{
    // QProcess may deliver partial records, so retain bytes until a newline arrives.
    m_buffer.append(m_process.readAllStandardOutput());
    qsizetype newline = -1;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        processLine(m_buffer.first(newline));
        m_buffer.remove(0, newline + 1);
    }
}

void JournalReader::processLine(const QByteArray &line)
{
    // Ignore malformed or unrelated records: the parser only needs MESSAGE-bearing entries.
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return;
    }

    const auto object = document.object();
    const QString message = object.value(QStringLiteral("MESSAGE")).toString();
    if (message.isEmpty()) {
        return;
    }

    bool timestampValid = false;
    const qint64 microseconds = object.value(QStringLiteral("__REALTIME_TIMESTAMP"))
                                    .toString()
                                    .toLongLong(&timestampValid);
    const QDateTime timestamp = timestampValid
        ? QDateTime::fromMSecsSinceEpoch(microseconds / 1000)
        : QDateTime::currentDateTime();
    Q_EMIT messageReceived(message, timestamp);
}

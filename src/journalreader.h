// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QProcess>

/** Follows the user's onedrive.service journal and emits its messages as they arrive. */
class JournalReader final : public QObject
{
    Q_OBJECT

public:
    /** Creates a reader and connects the journal process to its parsing pipeline. */
    explicit JournalReader(QObject *parent = nullptr);
    /** Stops the journal process before the reader is destroyed. */
    ~JournalReader() override;
    /** Starts journalctl unless a reader process is already running. */
    void start();

Q_SIGNALS:
    /** Emitted for each valid journal message with its event timestamp. */
    void messageReceived(const QString &message, const QDateTime &timestamp);
    /** Emitted when journalctl reports a process or standard-error failure. */
    void errorOccurred(const QString &message);

private Q_SLOTS:
    /** Buffers standard output and dispatches complete newline-delimited records. */
    void readOutput();
    /** Decodes one JSON journal record and emits its MESSAGE field. */
    void processLine(const QByteArray &line);

private:
    QProcess m_process;
    QByteArray m_buffer;
};

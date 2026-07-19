#pragma once

#include <QObject>
#include <QTimer>

class SystemdManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString activeState READ activeState NOTIFY stateChanged)
    Q_PROPERTY(QString subState READ subState NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)

public:
    explicit SystemdManager(QObject *parent = nullptr);

    [[nodiscard]] QString activeState() const;
    [[nodiscard]] QString subState() const;
    [[nodiscard]] QString errorMessage() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void startService();
    Q_INVOKABLE void stopService();
    Q_INVOKABLE void restartService();

Q_SIGNALS:
    void stateChanged();
    void errorMessageChanged();

private:
    void callManager(const QString &method);
    void setErrorMessage(const QString &message);

    QString m_activeState = QStringLiteral("unknown");
    QString m_subState;
    QString m_errorMessage;
    QTimer m_refreshTimer;
};

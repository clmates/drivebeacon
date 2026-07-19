// SPDX-License-Identifier: GPL-3.0-only

#include "activityparser.h"

#include <QTest>

class ActivityParserTest final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesTransfer_data();
    void parsesTransfer();
    void ignoresUnrelatedMessages();
};

void ActivityParserTest::parsesTransfer_data()
{
    QTest::addColumn<QString>("message");
    QTest::addColumn<QString>("operation");
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("completed");

    QTest::newRow("download-completed")
        << QStringLiteral("Downloading file: Documents/report.pdf ... done")
        << QStringLiteral("download") << QStringLiteral("Documents/report.pdf") << true;
    QTest::newRow("upload-started")
        << QStringLiteral("Uploading new file: Photos/image.jpg")
        << QStringLiteral("upload") << QStringLiteral("Photos/image.jpg") << false;
    QTest::newRow("upload-modified")
        << QStringLiteral("Uploading modified file: Notes/todo.txt ... done")
        << QStringLiteral("upload") << QStringLiteral("Notes/todo.txt") << true;
}

void ActivityParserTest::parsesTransfer()
{
    QFETCH(QString, message);
    QFETCH(QString, operation);
    QFETCH(QString, path);
    QFETCH(bool, completed);

    const auto result = ActivityParser::parse(message);
    QVERIFY(result.has_value());
    QCOMPARE(result->operation, operation);
    QCOMPARE(result->path, path);
    QCOMPARE(result->completed, completed);
}

void ActivityParserTest::ignoresUnrelatedMessages()
{
    QVERIFY(!ActivityParser::parse(QStringLiteral("Sync with OneDrive is complete")).has_value());
}

QTEST_MAIN(ActivityParserTest)
#include "activityparser_test.moc"

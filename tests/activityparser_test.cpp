// SPDX-License-Identifier: GPL-3.0-only

#include "activityparser.h"

#include <QTest>

/** Regression tests for the journal-message normalization rules. */
class ActivityParserTest final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesTransfer_data();
    void parsesTransfer();
    void parsesDeletion_data();
    void parsesDeletion();
    void parsesMove();
    void ignoresUnrelatedMessages();
};

void ActivityParserTest::parsesTransfer_data()
{
    // Data-driven cases cover operation direction, file qualifiers, and completion suffixes.
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

void ActivityParserTest::parsesDeletion_data()
{
    // Deletion fixtures distinguish a notification from a completed local deletion.
    QTest::addColumn<QString>("message");
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("completed");

    QTest::newRow("requested-deletion")
        << QStringLiteral("The operating system sent a deletion notification. "
                          "Trying to delete this item as requested: Documentos/PC/Acer_AX3950.pdf")
        << QStringLiteral("Documentos/PC/Acer_AX3950.pdf") << false;
    QTest::newRow("local-file")
        << QStringLiteral("Deleting local file: Documentos/Trabajo/certificados/alta roi.pdf")
        << QStringLiteral("Documentos/Trabajo/certificados/alta roi.pdf") << true;
    QTest::newRow("local-directory")
        << QStringLiteral("Deleting local directory: Documentos/Trabajo/certificados")
        << QStringLiteral("Documentos/Trabajo/certificados") << true;
}

void ActivityParserTest::parsesDeletion()
{
    // All deletion forms share the same normalized operation and source-path contract.
    QFETCH(QString, message);
    QFETCH(QString, path);
    QFETCH(bool, completed);

    const auto result = ActivityParser::parse(message);
    QVERIFY(result.has_value());
    QCOMPARE(result->operation, QStringLiteral("delete"));
    QCOMPARE(result->path, path);
    QVERIFY(result->destinationPath.isEmpty());
    QCOMPARE(result->completed, completed);
}

void ActivityParserTest::parsesMove()
{
    // A move must preserve both endpoints and is always considered complete.
    const auto result = ActivityParser::parse(QStringLiteral(
        "Moving Documentos/Office Lens/21_6_23, 8_39 Microsoft Lens.pdf "
        "to Documentos/Trabajo/Confidencial/21_6_23, 8_39 Microsoft Lens.pdf"));

    QVERIFY(result.has_value());
    QCOMPARE(result->operation, QStringLiteral("move"));
    QCOMPARE(result->path,
             QStringLiteral("Documentos/Office Lens/21_6_23, 8_39 Microsoft Lens.pdf"));
    QCOMPARE(result->destinationPath,
             QStringLiteral("Documentos/Trabajo/Confidencial/21_6_23, 8_39 Microsoft Lens.pdf"));
    QVERIFY(result->completed);
}

void ActivityParserTest::parsesTransfer()
{
    // Verify each transfer fixture against the normalized event fields.
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
    // Messages outside the supported vocabulary must not create visible activity rows.
    QVERIFY(!ActivityParser::parse(QStringLiteral("Sync with OneDrive is complete")).has_value());
    QVERIFY(!ActivityParser::parse(QStringLiteral(
        "Trying to delete local file: Documentos/Trabajo/certificados/alta roi.pdf")).has_value());
}

QTEST_MAIN(ActivityParserTest)
#include "activityparser_test.moc"

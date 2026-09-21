#include "../src/app/databasestartup.h"

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QSemaphore>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <stdexcept>

class DatabaseStartupTests final : public QObject
{
    Q_OBJECT
private slots:
    void migrationKeepsGuiResponsiveAndClosesDialog();
    void ordinaryStartupDoesNotShowDialog();
    void failureClosesDialogAndPreservesException();
};

namespace {
QDialog *visibleRebuildDialog()
{
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("databaseRebuildDialog") && widget->isVisible()) {
            return qobject_cast<QDialog *>(widget);
        }
    }
    return nullptr;
}
}

void DatabaseStartupTests::migrationKeepsGuiResponsiveAndClosesDialog()
{
    QSemaphore guiObserved;
    int visibleTicks = 0;
    bool correctLabel = false;
    bool closeIgnored = false;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, this, [&]() {
        if (auto *dialog = visibleRebuildDialog()) {
            ++visibleTicks;
            correctLabel = dialog->findChild<QLabel *>()->text()
                == QStringLiteral("Please wait, database rebuild in progress…");
            dialog->reject();
            dialog->close();
            closeIgnored = dialog->isVisible();
            if (visibleTicks == 5) guiObserved.release();
        }
    });
    heartbeat.start(30);
    bool workerThread = false;
    bool guiResponded = false;
    OKILTV::App::runDatabaseStartup([&](const auto &started) {
        workerThread = QThread::currentThread() != qApp->thread();
        started();
        guiResponded = guiObserved.tryAcquire(1, 3000);
    });
    QVERIFY(workerThread);
    QVERIFY(guiResponded);
    QVERIFY(visibleTicks >= 5);
    QVERIFY(correctLabel);
    QVERIFY(closeIgnored);
    QVERIFY(!visibleRebuildDialog());
}

void DatabaseStartupTests::ordinaryStartupDoesNotShowDialog()
{
    bool shown = false;
    QSemaphore guiObserved;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, this, [&]() {
        shown = shown || visibleRebuildDialog() != nullptr;
        guiObserved.release();
    });
    heartbeat.start(20);
    bool guiResponded = false;
    OKILTV::App::runDatabaseStartup([&](const auto &) { guiResponded = guiObserved.tryAcquire(3, 3000); });
    QVERIFY(guiResponded);
    QVERIFY(!shown);
}

void DatabaseStartupTests::failureClosesDialogAndPreservesException()
{
    try {
        OKILTV::App::runDatabaseStartup([](const auto &started) {
            started();
            throw std::runtime_error("migration fixture failure");
        });
        QFAIL("Expected original startup exception");
    } catch (const std::runtime_error &error) {
        QCOMPARE(QString::fromUtf8(error.what()), QStringLiteral("migration fixture failure"));
    }
    QVERIFY(!visibleRebuildDialog());
}

QTEST_MAIN(DatabaseStartupTests)
#include "tst_databasestartup.moc"

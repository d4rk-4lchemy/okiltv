#include "databasestartup.h"

#include <QCloseEvent>
#include <QDialog>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QTimer>

#include <chrono>
#include <future>

namespace OKILTV::App {
namespace {

class RebuildSpinner final : public QWidget
{
public:
    explicit RebuildSpinner(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(40, 40);
        connect(&m_timer, &QTimer::timeout, this, [this]() {
            m_angle = (m_angle + 30) % 360;
            update();
        });
        m_timer.start(80);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(width() / 2.0, height() / 2.0);
        painter.rotate(m_angle);
        for (int tick = 0; tick < 12; ++tick) {
            auto color = palette().color(QPalette::Highlight);
            color.setAlpha(35 + tick * 20);
            painter.setPen(QPen(color, 3.0, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(0, -11), QPointF(0, -16));
            painter.rotate(30);
        }
    }

private:
    QTimer m_timer;
    int m_angle { 0 };
};

class RebuildDialog final : public QDialog
{
public:
    RebuildDialog()
    {
        setObjectName(QStringLiteral("databaseRebuildDialog"));
        setWindowTitle(QStringLiteral("OKILTV"));
        setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
        setWindowModality(Qt::ApplicationModal);
        setAttribute(Qt::WA_QuitOnClose, false);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(24, 20, 24, 20);
        layout->setSpacing(16);
        layout->addWidget(new RebuildSpinner(this));
        layout->addWidget(new QLabel(QStringLiteral("Please wait, database rebuild in progress…"), this));
        setFixedSize(sizeHint());
    }

    void reject() override {} // Escape must not hide an in-progress migration.

protected:
    void closeEvent(QCloseEvent *event) override { event->ignore(); }
};

} // namespace

void runDatabaseStartup(const std::function<void(const std::function<void()> &)> &task)
{
    RebuildDialog dialog;
    QEventLoop eventLoop;
    const auto rebuildStarted = [&dialog]() {
        QMetaObject::invokeMethod(&dialog, [&dialog]() {
            if (!qEnvironmentVariableIsSet("OKILTV_HEADLESS_TEST")) dialog.show();
        }, Qt::QueuedConnection);
    };
    auto worker = std::async(std::launch::async, [task, rebuildStarted]() { task(rebuildStarted); });
    QTimer completionTimer;
    QObject::connect(&completionTimer, &QTimer::timeout, &eventLoop, [&]() {
        if (worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) eventLoop.quit();
    });
    completionTimer.start(20);
    eventLoop.exec();
    // Joining before dialog destruction also protects queued callback lifetime.
    worker.get();
}

} // namespace OKILTV::App

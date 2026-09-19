#pragma once

#include "../core/datetimeformat.h"
#include <QObject>

namespace OKILTV::App {
class DateTimeFormatter final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString timePattern READ timePattern NOTIFY formatChanged)
    Q_PROPERTY(QString guideDatePattern READ guideDatePattern NOTIFY formatChanged)
    Q_PROPERTY(QString timelineDatePattern READ timelineDatePattern NOTIFY formatChanged)
    Q_PROPERTY(QString clockPattern READ clockPattern NOTIFY formatChanged)
    Q_PROPERTY(QString dateTimePattern READ dateTimePattern NOTIFY formatChanged)
    Q_PROPERTY(QString systemDateOrderLabel READ systemDateOrderLabel CONSTANT)
    Q_PROPERTY(QString systemTimeFormatLabel READ systemTimeFormatLabel CONSTANT)
public:
    explicit DateTimeFormatter(QObject *parent = nullptr) : QObject(parent) {}
    void apply(const QString &dateOrder, const QString &timeFormat);
    Core::DateTimeFormatOptions options() const { return m_options; }
    QString timePattern() const { return m_options.timePattern(); }
    QString guideDatePattern() const { return m_options.guideDatePattern(); }
    QString timelineDatePattern() const { return m_options.timelineDatePattern(); }
    QString clockPattern() const { return m_options.clockPattern(); }
    QString dateTimePattern() const { return m_options.dateTimePattern(); }
    QString systemDateOrderLabel() const;
    QString systemTimeFormatLabel() const;
    Q_INVOKABLE QString preview(const QString &dateOrder, const QString &timeFormat) const;
signals:
    void formatChanged();
private:
    Core::DateTimeFormatOptions m_options { Core::systemDateTimeFormat() };
};
} // namespace OKILTV::App

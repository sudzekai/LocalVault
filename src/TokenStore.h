#pragma once
#include <QByteArray>
#include <QString>

namespace TokenStore {
QByteArray protect(const QByteArray &token, const QString &identity);
QByteArray restore(const QByteArray &protectedToken, const QString &identity);
void remove(const QByteArray &protectedToken, const QString &identity);
}

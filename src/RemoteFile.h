#pragma once

#include <QByteArray>
#include <QString>

namespace RemoteFile {
bool requestActive();
QByteArray read(const QString &source, const QByteArray &token);
void write(const QString &source, const QByteArray &token, const QByteArray &file, const QByteArray &revision);
}

#include "TokenStore.h"
#include "Vault.h"
#include <QCryptographicHash>
#include <QScopeGuard>
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#else
#include <QProcess>
#endif

namespace {
[[noreturn]] void failed()
{
    throw Failure{VaultError::Io, QStringLiteral("Не удалось сохранить или прочитать API-токен в защищённом хранилище ОС.")};
}
QByteArray key(const QString &identity)
{
    return QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex();
}
#ifndef Q_OS_WIN
QByteArray secretTool(const QStringList &arguments, const QByteArray &input = {}, bool allowMissing = false)
{
    QProcess process;
    process.start(QStringLiteral("secret-tool"), arguments);
    if (!process.waitForStarted(3000))
        throw Failure{VaultError::Io, QStringLiteral("Для запоминания токена установите secret-tool (libsecret-tools) и включите системную связку ключей Secret Service.")};
    if (!input.isEmpty())
        process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished();
        failed();
    }
    if (process.exitStatus() != QProcess::NormalExit || (process.exitCode() != 0 && !(allowMissing && process.exitCode() == 1)))
        failed();
    return process.readAllStandardOutput().trimmed();
}
#endif
}

QByteArray TokenStore::protect(const QByteArray &token, const QString &identity)
{
    if (token.isEmpty())
        return {};
#ifdef Q_OS_WIN
    auto entropy = key(identity);
    DATA_BLOB input{DWORD(token.size()), reinterpret_cast<BYTE *>(const_cast<char *>(token.constData()))};
    DATA_BLOB extra{DWORD(entropy.size()), reinterpret_cast<BYTE *>(entropy.data())};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"LocalVault API token", &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        failed();
    const auto cleanup = qScopeGuard([&] { SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); });
    return QByteArray("dpapi:") + QByteArray(reinterpret_cast<char *>(output.pbData), int(output.cbData)).toBase64();
#else
    secretTool({QStringLiteral("store"), QStringLiteral("--label=LocalVault API token"),
                QStringLiteral("application"), QStringLiteral("LocalVault"), QStringLiteral("source"), QString::fromLatin1(key(identity))}, token + '\n');
    return QByteArray("secret-service:") + key(identity);
#endif
}

QByteArray TokenStore::restore(const QByteArray &protectedToken, const QString &identity)
{
    if (protectedToken.isEmpty())
        return {};
#ifdef Q_OS_WIN
    if (!protectedToken.startsWith("dpapi:"))
        failed();
    auto bytes = QByteArray::fromBase64(protectedToken.mid(6));
    auto entropy = key(identity);
    DATA_BLOB input{DWORD(bytes.size()), reinterpret_cast<BYTE *>(bytes.data())};
    DATA_BLOB extra{DWORD(entropy.size()), reinterpret_cast<BYTE *>(entropy.data())};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        failed();
    const auto cleanup = qScopeGuard([&] { SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); });
    return QByteArray(reinterpret_cast<char *>(output.pbData), int(output.cbData));
#else
    if (protectedToken != QByteArray("secret-service:") + key(identity))
        failed();
    auto token = secretTool({QStringLiteral("lookup"), QStringLiteral("application"), QStringLiteral("LocalVault"),
                            QStringLiteral("source"), QString::fromLatin1(key(identity))});
    if (token.isEmpty())
        failed();
    return token;
#endif
}

void TokenStore::remove(const QByteArray &protectedToken, const QString &identity)
{
#ifdef Q_OS_WIN
    Q_UNUSED(protectedToken);
    Q_UNUSED(identity);
#else
    if (protectedToken.startsWith("secret-service:"))
        secretTool({QStringLiteral("clear"), QStringLiteral("application"), QStringLiteral("LocalVault"),
                    QStringLiteral("source"), QString::fromLatin1(key(identity))}, {}, true);
#endif
}

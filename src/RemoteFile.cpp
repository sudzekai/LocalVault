#include "RemoteFile.h"
#include "Vault.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QEventLoop>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace {
constexpr qsizetype maxResponse = (16 * 1024 * 1024) + 156;
bool active = false;

struct Response {
    int status;
    QByteArray body;
    QByteArray etag;
};

[[noreturn]] void fail(const QString &message)
{
    throw Failure{VaultError::Io, message};
}

QByteArray etag(const QByteArray &revision)
{
    return QByteArray("\"") + revision.toHex() + '"';
}

Response request(const QString &source, const QByteArray &token, const QByteArray *file,
                 const QByteArray &revision = {})
{
    if (active)
        fail(QStringLiteral("Дождитесь завершения запроса к хранилищу."));
    if (token.isEmpty() || token.size() > 1024 || token.contains('\r') || token.contains('\n'))
        fail(QStringLiteral("Введите API-токен в настройках источника или на экране авторизации."));
    const QUrl url(source, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment()
        || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")))
        fail(QStringLiteral("Укажите корректный URL HTTP или HTTPS без логина и пароля в адресе."));

    active = true;
    const bool graphical = qobject_cast<QGuiApplication *>(QCoreApplication::instance()) != nullptr;
    if (graphical)
        QGuiApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    const auto reset = qScopeGuard([&] {
        if (graphical)
            QGuiApplication::restoreOverrideCursor();
        active = false;
    });

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + token);
    request.setRawHeader("Accept", "application/octet-stream");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("Cache-Control", "no-store");
    request.setTransferTimeout(15000);
    if (file) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        request.setRawHeader("If-Match", etag(revision));
    }
    std::unique_ptr<QNetworkReply> reply(file ? manager.post(request, *file) : manager.get(request));
    reply->setReadBufferSize(64 * 1024);
    QByteArray bytes;
    bool oversized = false;
    const auto collect = [&] {
        const auto available = reply->bytesAvailable();
        if (available > maxResponse - bytes.size()) {
            oversized = true;
            reply->abort();
            return;
        }
        bytes.append(reply->readAll());
    };
    QObject::connect(reply.get(), &QIODevice::readyRead, reply.get(), collect);
    QEventLoop loop;
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, reply.get(), [&] { reply->abort(); });
    deadline.start(20000);
    if (!reply->isFinished())
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    deadline.stop();
    collect();
    if (oversized)
        fail(QStringLiteral("Ответ сервера превышает допустимый размер хранилища."));
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403)
        fail(QStringLiteral("Сервер отклонил API-токен."));
    if (status == 409 || status == 412)
        fail(QStringLiteral("Хранилище изменено на другом устройстве. Заблокируйте и откройте его снова."));
    if (status >= 300 && status < 400)
        fail(QStringLiteral("Сервер перенаправляет запрос. Укажите конечный URL хранилища."));
    if (reply->error() != QNetworkReply::NoError)
        fail(QStringLiteral("Ошибка доступа к серверу: %1").arg(reply->errorString()));
    return {status, std::move(bytes), reply->rawHeader("ETag")};
}
}

bool RemoteFile::requestActive()
{
    return active;
}

QByteArray RemoteFile::read(const QString &source, const QByteArray &token)
{
    auto response = request(source, token, nullptr);
    if (response.status != 200)
        fail(QStringLiteral("Сервер не вернул файл хранилища (HTTP %1).").arg(response.status));
    const auto revision = QCryptographicHash::hash(response.body, QCryptographicHash::Sha256);
    if (response.etag != etag(revision))
        fail(QStringLiteral("Сервер вернул файл с неверной или отсутствующей версией ETag."));
    return std::move(response.body);
}

void RemoteFile::write(const QString &source, const QByteArray &token, const QByteArray &file,
                       const QByteArray &revision)
{
    if (revision.size() != 32)
        fail(QStringLiteral("Откройте удалённое хранилище перед сохранением."));
    const auto expected = QCryptographicHash::hash(file, QCryptographicHash::Sha256);
    try {
        const auto response = request(source, token, &file, revision);
        if (response.status != 204 || response.etag != etag(expected))
            fail(QStringLiteral("Сервер не подтвердил сохранение хранилища."));
    } catch (const Failure &original) {
        // A disconnected reply may follow a successful atomic server write.
        try {
            const auto current = read(source, token);
            if (QCryptographicHash::hash(current, QCryptographicHash::Sha256) == expected)
                return;
        } catch (const Failure &) {
        }
        throw original;
    }
}

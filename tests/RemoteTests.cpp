#include "Vault.h"
#include "LegacyFixture.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<class Operation>
void expect(VaultError code, Operation operation)
{
    try {
        operation();
    } catch (const Failure &failure) {
        check(failure.code == code, "unexpected remote error code");
        return;
    }
    throw std::runtime_error("expected a remote error");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        CryptoManager::initialize();
        if (app.arguments().size() == 3 && app.arguments()[1] == QStringLiteral("--fixture")) {
            StorageManager(app.arguments()[2]).write(QByteArray::fromBase64(legacyVaultBase64));
            return 0;
        }
        const auto url = qEnvironmentVariable("APPVAULT_TEST_URL");
        const auto token = qgetenv("APPVAULT_TEST_TOKEN");
        check(!url.isEmpty() && !token.isEmpty(), "remote integration environment missing");
        const auto password = QStringLiteral("synthetic migration master");
        const auto importedPassword = QStringLiteral("imported remote master password");
        QTemporaryDir directory;
        check(directory.isValid(), "test directory missing");
        StorageManager remote(url, token);
        const auto legacy = remote.read();
        PasswordManager first(directory.filePath(QStringLiteral("unused.lvault")));
        first.selectPath(url, true, token);
        expect(VaultError::WrongPassword, [&] { first.open(QStringLiteral("incorrect")); });
        check(remote.read() == legacy, "wrong password changed server bytes");
        first.open(password);
        check(StorageManager::readHeader(remote.read()).version == 4, "remote migration was not posted");
        const auto beforeRejectedWrite = remote.read();
        const int previousIdle = first.data().idleMinutes;
        first.setApiToken(QByteArray("incorrect-token"));
        expect(VaultError::Io, [&] { first.setIdleMinutes(previousIdle + 1); });
        check(first.data().idleMinutes == previousIdle && remote.read() == beforeRejectedWrite,
              "rejected POST changed local or remote data");
        first.setApiToken(token);
        Group group;
        group.name = QStringLiteral("Remote synthetic group");
        const auto groupId = group.id;
        first.putGroup(std::move(group));
        PasswordManager second(directory.filePath(QStringLiteral("second-unused.lvault")));
        second.selectPath(url, true, token);
        second.open(password);

        Site site;
        site.name = QStringLiteral("Remote synthetic site");
        site.url = QStringLiteral("https://remote.example");
        site.groupId = groupId;
        const auto siteId = site.id;
        first.putSite(std::move(site));
        const auto afterFirst = remote.read();
        expect(VaultError::Io, [&] {
            Group stale;
            stale.name = QStringLiteral("Discarded stale group");
            second.putGroup(std::move(stale));
        });
        check(remote.read() == afterFirst && second.data().groups.size() == 1, "stale save changed server or local state");
        second.lock();
        second.open(password);
        check(second.data().sites.size() == 2, "second client did not reload updated data");
        second.lock();

        Entry account;
        account.siteId = siteId;
        account.login = QStringLiteral("remote-test-login");
        account.password = QStringLiteral("remote-test-secret");
        account.recoveryCodes = QStringLiteral("remote-recovery-test");
        first.put(std::move(account));
        const auto withAccount = remote.read();
        check(!withAccount.contains("remote-test-secret") && !withAccount.contains("remote-test-login"),
              "remote file contains plaintext");

        const auto backup = directory.filePath(QStringLiteral("backup.lvault"));
        first.exportTo(backup);
        check(StorageManager(backup).read() == withAccount, "remote export differs");
        expect(VaultError::Io, [&] { StorageManager(url, QByteArray("invalid")).read(); });
        PasswordManager imported(directory.filePath(QStringLiteral("import.lvault")));
        imported.create(importedPassword);
        Site replacement;
        replacement.name = QStringLiteral("Imported remote fixture");
        replacement.url = QStringLiteral("https://import.example");
        imported.putSite(std::move(replacement));
        imported.lock();
        first.importFrom(imported.path(), importedPassword);
        first.lock();
        first.open(importedPassword);
        check(first.path() == url && first.isRemote() && first.data().sites.size() == 1
              && first.data().sites[0].name == QStringLiteral("Imported remote fixture"), "remote import lost source or data");
        first.changePassword(importedPassword, password);
        first.lock();
        first.open(password);
        check(first.data().sites.size() == 1, "remote password rotation failed");
        check(!QFile::exists(directory.filePath(QStringLiteral("unused.lvault"))), "remote source created a local vault");
        std::cout << "Remote integration checks passed.\n";
    } catch (const Failure &failure) {
        std::cerr << failure.message.toStdString() << '\n';
        return 1;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}

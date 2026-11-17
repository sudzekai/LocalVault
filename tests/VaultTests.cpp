#include "Vault.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<class Function>
void expectError(VaultError expected, Function function)
{
    try {
        function();
    } catch (const Failure &failure) {
        check(failure.code == expected, "unexpected error code");
        return;
    }
    throw std::runtime_error("expected an error");
}

Site site(const QString &name, const QString &url)
{
    Site item;
    item.name = name;
    item.url = url;
    return item;
}

Entry account(const QUuid &siteId, const QString &login)
{
    Entry entry;
    entry.siteId = siteId;
    entry.login = login;
    entry.password = QStringLiteral("пароль-только-для-теста🔐");
    return entry;
}

void checkGenerator()
{
    const auto all = PasswordGenerator::generate({24, true, true, true, true});
    bool lower = false, upper = false, digit = false, symbol = false;
    for (auto c : all) {
        lower |= c.isLower();
        upper |= c.isUpper();
        digit |= c.isDigit();
        symbol |= !c.isLetterOrNumber();
    }
    check(all.size() == 24 && lower && upper && digit && symbol, "generator groups");
    const auto digits = PasswordGenerator::generate({8, false, false, true, false});
    for (auto c : digits)
        check(c.isDigit(), "generator violated enabled groups");
    expectError(VaultError::Invalid, [] { PasswordGenerator::generate({24, false, false, false, false}); });
    check(PasswordGenerator::strength(QStringLiteral("111111111111111111")) == 1, "weak password rating");
}

void checkVault()
{
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory");
    const auto path = dir.filePath(QStringLiteral("active.lvault"));
    const auto backup = dir.filePath(QStringLiteral("backup.lvault"));
    const auto broken = dir.filePath(QStringLiteral("broken.lvault"));
    const auto oldPassword = QStringLiteral("first test master password");
    const auto newPassword = QStringLiteral("next test master password");
    const auto mail = QStringLiteral("primary@example.com");
    StorageManager storage(path);
    PasswordManager manager(path);
    manager.create(oldPassword);
    check(manager.unlocked(), "create did not unlock");
    const auto firstFile = storage.read();
    check(qFromBigEndian<quint32>(firstFile.constData() + 4) == 3, "unexpected file format");
    expectError(VaultError::Io, [&] { manager.create(newPassword); });
    expectError(VaultError::Invalid, [&] { manager.put(account(QUuid::createUuid(), QStringLiteral("orphan"))); });

    auto firstSite = site(QStringLiteral("Пример"), QStringLiteral("https://example.com/"));
    const auto firstId = firstSite.id;
    manager.putSite(std::move(firstSite));
    expectError(VaultError::Invalid, [&] {
        manager.putSite(site(QStringLiteral("  ПРИМЕР "), QStringLiteral("https://other.example/")));
    });
    expectError(VaultError::Invalid, [&] {
        manager.putSite(site(QStringLiteral("Копия"), QStringLiteral("https://example.com")));
    });
    auto secondSite = site(QStringLiteral("Второй"), QStringLiteral("https://other.example/"));
    const auto secondId = secondSite.id;
    manager.putSite(std::move(secondSite));

    auto entry = account(firstId, QStringLiteral("test-login"));
    const auto entryId = entry.id;
    const auto created = entry.createdAt;
    check(created.isValid(), "new account has no date by default");
    entry.userName = QStringLiteral("Синтетическое имя");
    entry.phone = QStringLiteral("+1 202 555 0142");
    EmailAddress firstMail;
    firstMail.address = mail;
    firstMail.description = QStringLiteral("Личная");
    entry.emails.push_back(std::move(firstMail));
    EmailAddress secondMail;
    secondMail.address = QStringLiteral("secondary@example.com");
    entry.emails.push_back(std::move(secondMail));
    manager.put(std::move(entry));
    auto anonymous = account(firstId, QStringLiteral("anonymous-login"));
    anonymous.createdAt = {};
    const auto anonymousId = anonymous.id;
    manager.put(std::move(anonymous));
    manager.put(account(secondId, QStringLiteral("second-site-login")));
    manager.setIdleMinutes(17);
    auto renamed = site(QStringLiteral("Пример 2"), QStringLiteral("https://example.com/new"));
    renamed.id = firstId;
    manager.putSite(std::move(renamed));

    const auto current = storage.read();
    check(firstFile.mid(96, 12) != current.mid(96, 12), "nonce repeated");
    check(!current.contains("test-login") && !current.contains(mail.toUtf8()), "plaintext on disk");
    manager.lock();
    check(!manager.unlocked() && manager.data().sites.empty() && manager.data().entries.empty(), "lock retained data");
    expectError(VaultError::WrongPassword, [&] { manager.open(newPassword); });
    manager.open(oldPassword);
    const auto &saved = manager.data().entries.at(0);
    check(saved.id == entryId && saved.siteId == firstId && saved.login == QStringLiteral("test-login")
          && saved.password == account(firstId, {}).password, "account data did not round trip");
    check(saved.userName == QStringLiteral("Синтетическое имя") && saved.phone == QStringLiteral("+1 202 555 0142")
          && saved.createdAt == created && saved.emails.size() == 2 && saved.emails[0].address == mail
          && saved.emails[0].description == QStringLiteral("Личная") && saved.emails[1].description.isEmpty(),
          "optional fields did not round trip");
    check(!manager.data().entries[1].createdAt.isValid() && manager.data().entries[1].emails.empty()
          && manager.data().sites[0].name == QStringLiteral("Пример 2") && manager.data().idleMinutes == 17,
          "site or settings did not round trip");

    auto invalid = account(firstId, QStringLiteral("bad-email"));
    EmailAddress invalidEmail;
    invalidEmail.address = QStringLiteral("invalid");
    invalid.emails.push_back(std::move(invalidEmail));
    expectError(VaultError::Invalid, [&] { manager.put(std::move(invalid)); });
    manager.exportTo(backup);
    check(StorageManager(backup).read() == current, "backup differs");
    expectError(VaultError::Invalid, [&] { manager.exportTo(path); });
    auto tampered = current;
    tampered[110] = char(tampered[110] ^ 1);
    StorageManager(broken).write(tampered);
    expectError(VaultError::Damaged, [&] { manager.importFrom(broken, oldPassword); });
    tampered.chop(32);
    tampered += QCryptographicHash::hash(tampered, QCryptographicHash::Sha256);
    StorageManager(broken).write(tampered);
    expectError(VaultError::Damaged, [&] { manager.importFrom(broken, oldPassword); });
    auto older = current;
    older[7] = char(2);
    older.chop(32);
    older += QCryptographicHash::hash(older, QCryptographicHash::Sha256);
    StorageManager(broken).write(older);
    expectError(VaultError::Version, [&] { manager.importFrom(broken, oldPassword); });
    check(storage.read() == current, "failed import changed disk state");

    expectError(VaultError::WrongPassword, [&] { manager.changePassword(newPassword, oldPassword); });
    manager.changePassword(oldPassword, newPassword);
    manager.lock();
    expectError(VaultError::WrongPassword, [&] { manager.open(oldPassword); });
    manager.open(newPassword);
    manager.remove(anonymousId);
    check(manager.data().entries.size() == 2, "remove account failed");
    manager.removeSite(firstId);
    check(manager.data().sites.size() == 1 && manager.data().entries.size() == 1
          && manager.data().entries[0].siteId == secondId, "site deletion did not cascade");
    manager.importFrom(backup, oldPassword);
    manager.lock();
    manager.open(oldPassword);
    check(manager.data().sites.size() == 2 && manager.data().entries.size() == 3
          && manager.data().entries[0].emails.size() == 2, "verified backup was not restored");
}

void checkRollback()
{
    QTemporaryDir dir;
    check(dir.isValid() && QDir().mkpath(dir.filePath(QStringLiteral("active"))), "rollback directory");
    const auto path = dir.filePath(QStringLiteral("active/vault.lvault"));
    PasswordManager manager(path);
    manager.create(QStringLiteral("rollback test password"));
    auto group = site(QStringLiteral("Сайт"), QStringLiteral("https://example.com"));
    const auto id = group.id;
    manager.putSite(std::move(group));
    auto entry = account(id, QStringLiteral("retained"));
    const auto accountId = entry.id;
    manager.put(std::move(entry));
    check(QFile::rename(path, dir.filePath(QStringLiteral("moved.lvault"))), "rename active vault");
    expectError(VaultError::Io, [&] { manager.put(account(id, QStringLiteral("discarded"))); });
    expectError(VaultError::Io, [&] { manager.remove(accountId); });
    expectError(VaultError::Io, [&] { manager.removeSite(id); });
    expectError(VaultError::Io, [&] { manager.setIdleMinutes(19); });
    check(manager.data().sites.size() == 1 && manager.data().sites[0].id == id
          && manager.data().entries.size() == 1 && manager.data().entries[0].id == accountId
          && manager.data().idleMinutes == 5, "failed write changed in-memory state");
}

void checkExternalPath()
{
    QTemporaryDir dir;
    check(dir.isValid(), "external path test directory");
    const auto defaultPath = dir.filePath(QStringLiteral("default.lvault"));
    const auto externalPath = dir.filePath(QStringLiteral("shared.lvault"));
    const auto missingPath = dir.filePath(QStringLiteral("missing.lvault"));
    const auto password = QStringLiteral("external file test password");
    {
        PasswordManager original(externalPath);
        original.create(password);
        original.putSite(site(QStringLiteral("Shared"), QStringLiteral("https://shared.example")));
    }

    PasswordManager client(defaultPath);
    client.selectPath(missingPath, true);
    expectError(VaultError::Io, [&] { client.open(password); });
    expectError(VaultError::Invalid, [&] { client.create(password); });
    expectError(VaultError::Io, [&] { client.importFrom(externalPath, password); });
    check(!QFile::exists(missingPath) && !QFile::exists(defaultPath), "missing path created a vault");

    client.selectPath(externalPath, true);
    client.open(password);
    PasswordManager other(defaultPath);
    other.selectPath(externalPath, true);
    expectError(VaultError::Io, [&] { other.open(password); });
    const auto unavailable = dir.filePath(QStringLiteral("offline.lvault"));
    check(QFile::rename(externalPath, unavailable), "could not take external file offline");
    expectError(VaultError::Io, [&] {
        client.putSite(site(QStringLiteral("Discarded"), QStringLiteral("https://discarded.example")));
    });
    check(!QFile::exists(externalPath) && client.data().sites.size() == 1, "offline write changed state");
    check(QFile::rename(unavailable, externalPath), "could not restore external file");
    const auto originalBytes = StorageManager(externalPath).read();
    auto modifiedBytes = originalBytes;
    modifiedBytes[110] = char(modifiedBytes[110] ^ 1);
    StorageManager(externalPath).write(modifiedBytes);
    expectError(VaultError::Io, [&] { client.setIdleMinutes(18); });
    check(StorageManager(externalPath).read() == modifiedBytes && client.data().idleMinutes == 5,
          "unexpected external modification was overwritten");
    StorageManager(externalPath).write(originalBytes);
    client.putSite(site(QStringLiteral("Next"), QStringLiteral("https://next.example")));
    client.lock();
    other.open(password);
    check(other.data().sites.size() == 2 && !QFile::exists(defaultPath), "changes were not saved directly");
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    try {
        CryptoManager::initialize();
        checkVault();
        checkRollback();
        checkExternalPath();
        checkGenerator();
        std::cout << "Vault checks passed.\n";
    } catch (const Failure &failure) {
        std::cerr << failure.message.toStdString() << '\n';
        return 1;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}

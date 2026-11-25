#include "Vault.h"
#include "LegacyFixture.h"

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
    check(qFromBigEndian<quint32>(firstFile.constData() + 4) == 4, "unexpected file format");
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

void checkGroupsAndAlternativeSignIn()
{
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("groups.lvault"));
    const auto password = QStringLiteral("test master for groups");
    PasswordManager manager(path);
    manager.create(password);
    Group group;
    group.name = QStringLiteral("СОЦСЕТИ");
    group.iconName = QStringLiteral("people");
    const auto groupId = group.id;
    manager.putGroup(std::move(group));
    Group duplicate;
    duplicate.name = QStringLiteral(" соцсети ");
    expectError(VaultError::Invalid, [&] { manager.putGroup(std::move(duplicate)); });
    auto telegram = site(QStringLiteral("Telegram"), QStringLiteral("https://telegram.org"));
    const auto siteId = telegram.id;
    telegram.groupId = groupId;
    telegram.iconName = QStringLiteral("telegram");
    manager.putSite(std::move(telegram));
    auto oauth = account(siteId, QStringLiteral("oauth-test"));
    oauth.password.clear();
    expectError(VaultError::Invalid, [&] {
        auto missing = account(siteId, QStringLiteral("missing"));
        missing.password.clear();
        manager.put(std::move(missing));
    });
    oauth.alternativeSignIn = true;
    expectError(VaultError::Invalid, [&] {
        auto missing = account(siteId, QStringLiteral("missing-description"));
        missing.password.clear();
        missing.alternativeSignIn = true;
        manager.put(std::move(missing));
    });
    oauth.signInMethod = QStringLiteral("OAuth через Google");
    oauth.recoveryCodes = QStringLiteral("SYNTHETIC-RECOVERY-001\nSYNTHETIC-RECOVERY-002");
    manager.put(std::move(oauth));
    const auto encrypted = StorageManager(path).read();
    check(!encrypted.contains("SYNTHETIC-RECOVERY"), "recovery codes visible on disk");
    manager.lock();
    manager.open(password);
    check(manager.data().groups.size() == 1 && manager.data().groups[0].iconName == QStringLiteral("people")
          && manager.data().sites[0].groupId == groupId
          && manager.data().sites[0].iconName == QStringLiteral("telegram"), "group and icon round trip failed");
    const auto &saved = manager.data().entries[0];
    check(saved.password.isEmpty() && saved.alternativeSignIn
          && saved.signInMethod == QStringLiteral("OAuth через Google"), "alternative sign in round trip failed");
    check(saved.recoveryCodes == QStringLiteral("SYNTHETIC-RECOVERY-001\nSYNTHETIC-RECOVERY-002"),
          "recovery codes round trip failed");
    const auto backup = dir.filePath(QStringLiteral("recovery-backup.lvault"));
    manager.exportTo(backup);
    PasswordManager restored(dir.filePath(QStringLiteral("restored.lvault")));
    restored.importFrom(backup, password);
    const auto nextPassword = QStringLiteral("recovery codes new master");
    restored.changePassword(password, nextPassword);
    restored.lock();
    restored.open(nextPassword);
    check(restored.data().entries[0].recoveryCodes == saved.recoveryCodes,
          "import or password rotation lost recovery codes");
    manager.removeGroup(groupId);
    check(manager.data().groups.empty() && manager.data().sites.size() == 1
          && manager.data().sites[0].groupId.isNull() && manager.data().entries.size() == 1,
          "group removal deleted associated data");
}

void checkPreviousVersionMigration()
{
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("legacy.lvault"));
    const auto original = QByteArray::fromBase64(legacyVaultBase64);
    const auto password = QStringLiteral("synthetic migration master");
    StorageManager storage(path);
    storage.write(original);
    PasswordManager manager(path);
    expectError(VaultError::WrongPassword, [&] { manager.open(QStringLiteral("incorrect")); });
    check(storage.read() == original && !manager.unlocked(), "wrong password modified the legacy file");

    auto damaged = original;
    damaged[110] = char(damaged[110] ^ 1);
    damaged.chop(32);
    damaged += QCryptographicHash::hash(damaged, QCryptographicHash::Sha256);
    storage.write(damaged);
    expectError(VaultError::Damaged, [&] { manager.open(password); });
    check(storage.read() == damaged && !manager.unlocked(), "corrupted legacy file was rewritten");
    storage.write(original);
    manager.open(password);
    const auto migrated = storage.read();
    check(StorageManager::readHeader(migrated).version == 4 && migrated != original,
          "legacy file was not migrated after unlock");
    const auto &data = manager.data();
    check(data.groups.empty() && data.sites.size() == 1 && data.entries.size() == 1 && data.idleMinutes == 17,
          "legacy record counts or settings lost");
    const auto &entry = data.entries[0];
    check(entry.id == QUuid(QStringLiteral("{27be0cd2-5380-431d-a748-a5a376743a13}"))
          && entry.siteId == data.sites[0].id && data.sites[0].groupId.isNull()
          && entry.login == QStringLiteral("legacy synthetic login")
          && entry.password == QStringLiteral("legacy synthetic secret")
          && entry.userName == QStringLiteral("Test user") && entry.phone == QStringLiteral("+1 202 555 0142")
          && entry.createdAt.toMSecsSinceEpoch() == 1700000000123
          && entry.emails.size() == 1 && entry.emails[0].description == QStringLiteral("Fixture")
          && !entry.alternativeSignIn && entry.signInMethod.isEmpty(), "legacy fields changed");
    manager.lock();
    manager.open(password);
    check(storage.read() == migrated, "current format was migrated again");
    manager.lock();
    const auto backupPath = dir.filePath(QStringLiteral("import-source.lvault"));
    StorageManager(backupPath).write(original);
    PasswordManager imported(dir.filePath(QStringLiteral("imported.lvault")));
    imported.importFrom(backupPath, password);
    check(StorageManager::readHeader(StorageManager(imported.path()).read()).version == 4
          && StorageManager(backupPath).read() == original, "legacy import failed");
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
        checkGroupsAndAlternativeSignIn();
        checkPreviousVersionMigration();
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

#include "Vault.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryDir>

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

Entry sample(const QString &login)
{
    Entry entry;
    entry.site = QStringLiteral("Пример");
    entry.url = QStringLiteral("https://example.com/secret");
    entry.login = login;
    entry.password = QStringLiteral("пароль-тільки-для-тесту🔐");
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
    const auto onlyDigits = PasswordGenerator::generate({8, false, false, true, false});
    for (auto c : onlyDigits)
        check(c.isDigit(), "generator violated enabled groups");
    expectError(VaultError::Invalid, [] { PasswordGenerator::generate({24, false, false, false, false}); });
    check(PasswordGenerator::strength(QStringLiteral("111111111111111111")) == 1, "weak password rating");
}

void checkVault()
{
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory");
    const auto active = dir.filePath(QStringLiteral("active.lvault"));
    const auto backup = dir.filePath(QStringLiteral("backup.lvault"));
    const auto broken = dir.filePath(QStringLiteral("broken.lvault"));
    const auto previous = QStringLiteral("first master password");
    const auto next = QStringLiteral("another master password");
    const auto wrong = QStringLiteral("wrong master password");
    StorageManager storage(active);
    PasswordManager manager(active);
    manager.create(previous);
    const auto firstFile = storage.read();
    check(manager.unlocked(), "create did not unlock");
    expectError(VaultError::Io, [&] { manager.create(next); });

    auto first = sample(QStringLiteral("account-one@example.com"));
    const auto id = first.id;
    manager.put(std::move(first));
    manager.put(sample(QStringLiteral("account-two@example.com")));
    const auto before = storage.read();
    check(!before.contains("account-one@example.com") && !before.contains("example.com/secret"), "plaintext on disk");
    check(firstFile.mid(96, 12) != before.mid(96, 12), "nonce repeated");
    manager.setIdleMinutes(17);
    auto changed = sample(QStringLiteral("changed@example.com"));
    changed.id = id;
    manager.put(std::move(changed));

    manager.lock();
    check(!manager.unlocked() && manager.data().entries.empty(), "lock retained entries");
    expectError(VaultError::WrongPassword, [&] { manager.open(wrong); });
    check(!manager.unlocked(), "wrong password opened session");
    manager.open(previous);
    check(manager.data().entries.size() == 2 && manager.data().entries[0].id == id, "record IDs");
    check(manager.data().entries[0].login == QStringLiteral("changed@example.com"), "editing/reopen");
    check(manager.data().entries[0].password == sample({}).password, "unicode round trip");
    check(manager.data().idleMinutes == 17, "settings persistence");

    manager.exportTo(backup);
    check(StorageManager(backup).read() == storage.read(), "backup differs");
    expectError(VaultError::Invalid, [&] { manager.exportTo(active); });
    expectError(VaultError::WrongPassword, [&] { manager.changePassword(wrong, next); });
    manager.changePassword(previous, next);
    check(StorageManager::readHeader(storage.read()).salt != StorageManager::readHeader(before).salt, "salt reused");
    manager.lock();
    expectError(VaultError::WrongPassword, [&] { manager.open(previous); });
    manager.open(next);
    manager.remove(id);
    check(manager.data().entries.size() == 1 && manager.data().entries[0].id != id, "remove");
    const auto current = storage.read();

    auto tampered = StorageManager(backup).read();
    tampered[110] = char(tampered[110] ^ 1);
    StorageManager(broken).write(tampered);
    expectError(VaultError::Damaged, [&] { manager.importFrom(broken, previous); });
    tampered.chop(32);
    tampered += QCryptographicHash::hash(tampered, QCryptographicHash::Sha256);
    StorageManager(broken).write(tampered);
    expectError(VaultError::Damaged, [&] { manager.importFrom(broken, previous); });
    expectError(VaultError::WrongPassword, [&] { manager.importFrom(backup, next); });
    check(storage.read() == current && manager.data().entries.size() == 1, "failed import changed state");
    manager.importFrom(backup, previous);
    check(manager.data().entries.size() == 2, "import did not restore");
    manager.lock();
    manager.open(previous);

    const auto subdir = dir.filePath(QStringLiteral("rollback"));
    check(QDir().mkpath(subdir), "rollback directory");
    PasswordManager rollback(QDir(subdir).filePath(QStringLiteral("vault.lvault")));
    rollback.create(previous);
    auto existing = sample(QStringLiteral("retained"));
    const auto retainedId = existing.id;
    rollback.put(std::move(existing));
    check(QDir(dir.path()).rename(QStringLiteral("rollback"), QStringLiteral("moved")), "rename rollback directory");
    expectError(VaultError::Io, [&] { rollback.put(sample(QStringLiteral("discarded"))); });
    expectError(VaultError::Io, [&] { rollback.remove(retainedId); });
    expectError(VaultError::Io, [&] { rollback.setIdleMinutes(19); });
    check(rollback.data().entries.size() == 1 && rollback.data().entries[0].id == retainedId
          && rollback.data().idleMinutes == 5, "failed write changed in-memory state");
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    try {
        CryptoManager::initialize();
        checkVault();
        checkGenerator();
        std::cout << "Vault checks passed\n";
    } catch (const Failure &failure) {
        std::cerr << failure.message.toStdString() << '\n';
        return 1;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}

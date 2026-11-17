#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUuid>

#include <memory>
#include <optional>
#include <vector>

enum class VaultError { WrongPassword, Damaged, Version, Io, Crypto, Limit, Invalid };

struct Failure {
    VaultError code;
    QString message;
};

void wipe(QString &value);
void wipe(QByteArray &value);

class SecureKey {
public:
    SecureKey();
    unsigned char *data() { return bytes.get(); }
    const unsigned char *data() const { return bytes.get(); }

private:
    struct Deleter { void operator()(unsigned char *data) const; };
    std::unique_ptr<unsigned char, Deleter> bytes;
};

struct Site {
    QUuid id = QUuid::createUuid();
    QString name;
    QString url;

    Site() = default;
    Site(Site &&) noexcept = default;
    Site &operator=(Site &&other) noexcept;
    ~Site();
};

struct EmailAddress {
    QString address;
    QString description;

    EmailAddress() = default;
    EmailAddress(EmailAddress &&) noexcept = default;
    EmailAddress &operator=(EmailAddress &&other) noexcept;
    ~EmailAddress();
};

struct Entry {
    static constexpr int maxEmails = 64;

    QUuid id = QUuid::createUuid();
    QUuid siteId;
    QString login;
    QString password;
    QString userName;
    QString phone;
    QDateTime createdAt = QDateTime::currentDateTimeUtc();
    std::vector<EmailAddress> emails;

    Entry() = default;
    Entry(const Entry &) = delete;
    Entry &operator=(const Entry &) = delete;
    Entry(Entry &&) noexcept = default;
    Entry &operator=(Entry &&other) noexcept;
    ~Entry();
};

struct VaultData {
    std::vector<Site> sites;
    std::vector<Entry> entries;
    int idleMinutes = 5;
};

struct VaultHeader {
    QByteArray salt;
    quint32 operations = 3;
    quint32 memoryKiB = 64 * 1024;
    QByteArray verifier;
};

struct Session {
    VaultHeader header;
    SecureKey key;
};

class CryptoManager {
public:
    static void initialize();
    static Session derive(VaultHeader header, const QString &password);
    static QByteArray encrypt(const Session &session, const VaultData &data);
    static VaultData decrypt(const Session &session, const QByteArray &file);
};

class StorageManager {
public:
    explicit StorageManager(QString path);
    bool exists() const;
    QByteArray read() const;
    void write(const QByteArray &bytes) const;
    static VaultHeader readHeader(const QByteArray &file);

private:
    QString path;
};

class AuthManager {
public:
    static Session create(const QString &password);
    static Session unlock(const QByteArray &file, const QString &password);
};

class PasswordManager {
public:
    explicit PasswordManager(QString path);
    bool exists() const;
    bool unlocked() const { return session.has_value(); }
    void open(const QString &password);
    void create(const QString &password);
    void lock();
    void changePassword(const QString &current, const QString &next);
    void importFrom(const QString &path, const QString &password);
    void exportTo(const QString &path) const;
    void putSite(Site site);
    void removeSite(const QUuid &id);
    void put(Entry entry);
    void remove(const QUuid &id);
    void setIdleMinutes(int minutes);
    const VaultData &data() const { return records; }

private:
    const Session &activeSession() const;
    void save() const;
    StorageManager storage;
    QString activePath;
    VaultData records;
    std::optional<Session> session;
};

struct GeneratorOptions {
    int length = 24;
    bool lowercase = true;
    bool uppercase = true;
    bool digits = true;
    bool symbols = true;
};

class PasswordGenerator {
public:
    static QString generate(GeneratorOptions options);
    static int strength(const QString &password);
};

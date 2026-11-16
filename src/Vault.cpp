#include "Vault.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QtEndian>

#include <openssl/evp.h>
#include <sodium.h>

#include <algorithm>
#include <utility>

namespace {
constexpr quint32 originalFormat = 1;
constexpr quint32 emailFormat = 2;
constexpr int keySize = 32;
constexpr int saltSize = crypto_pwhash_SALTBYTES;
constexpr int nonceSize = 12;
constexpr int tagSize = 16;
constexpr int checksumSize = 32;
constexpr int paramsSize = 8 + 4 + 4 + saltSize;
constexpr int headerSize = paramsSize + keySize + keySize + nonceSize;
constexpr qsizetype maxPayload = 16 * 1024 * 1024;
constexpr qsizetype maxFile = maxPayload + headerSize + tagSize + checksumSize;
constexpr int maxEntries = 10000;
constexpr int maxField = 2048;

[[noreturn]] void fail(VaultError code, const QString &message)
{
    throw Failure{code, message};
}

[[noreturn]] void damaged()
{
    fail(VaultError::Damaged, QStringLiteral("Файл хранилища повреждён или был изменён."));
}

const unsigned char *unsignedData(const QByteArray &value)
{
    return reinterpret_cast<const unsigned char *>(value.constData());
}

unsigned char *unsignedData(QByteArray &value)
{
    return reinterpret_cast<unsigned char *>(value.data());
}

struct SecretBytes {
    QByteArray value;
    ~SecretBytes() { wipe(value); }
};

QByteArray randomBytes(int size)
{
    QByteArray result(size, Qt::Uninitialized);
    randombytes_buf(result.data(), size);
    return result;
}

QByteArray headerParameters(const VaultHeader &header, quint32 version)
{
    QByteArray bytes("LVLT", 4);
    QDataStream stream(&bytes, QIODevice::Append);
    stream << version << header.operations << header.memoryKiB;
    bytes.append(header.salt);
    return bytes;
}

bool validEmail(const QString &email)
{
    static const QRegularExpression address(QStringLiteral(R"(\A[^\s@<>]+@[^\s@<>]+\z)"),
                                            QRegularExpression::UseUnicodePropertiesOption);
    return email.isEmpty() || (email.size() <= maxField && address.match(email).hasMatch());
}

bool validEntry(const Entry &entry)
{
    const QUrl url(entry.url, QUrl::StrictMode);
    return !entry.id.isNull() && !entry.site.trimmed().isEmpty() && !entry.login.trimmed().isEmpty()
        && !entry.password.isEmpty() && entry.site.size() <= maxField && entry.url.size() <= maxField
        && entry.login.size() <= maxField && entry.password.size() <= maxField
        && url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty()
        && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"));
}

void writeField(QDataStream &stream, const QString &field)
{
    stream << quint16(field.size());
    for (const auto character : field)
        stream << character.unicode();
}

void readField(QDataStream &stream, QString &field)
{
    quint16 size = 0;
    stream >> size;
    if (size > maxField || stream.device()->bytesAvailable() < size * 2)
        damaged();
    field.resize(size);
    for (auto &character : field) {
        quint16 code = 0;
        stream >> code;
        character = QChar(code);
    }
}

void serialize(const VaultData &data, QByteArray &payload, bool includeEmail)
{
    if (data.entries.size() > maxEntries)
        fail(VaultError::Limit, QStringLiteral("Достигнут предел количества записей."));
    qsizetype size = 8;
    for (const auto &entry : data.entries) {
        size += 24 + 2 * (entry.site.size() + entry.url.size()
                        + entry.login.size() + entry.password.size());
        if (includeEmail)
            size += 2 + 2 * entry.email.size();
    }
    if (size > maxPayload)
        fail(VaultError::Limit, QStringLiteral("Достигнут предельный размер хранилища."));
    payload.reserve(size);
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint32(data.idleMinutes) << quint32(data.entries.size());
    for (const auto &entry : data.entries) {
        stream << entry.id;
        writeField(stream, entry.site);
        writeField(stream, entry.url);
        writeField(stream, entry.login);
        writeField(stream, entry.password);
        if (includeEmail)
            writeField(stream, entry.email);
    }
    if (stream.status() != QDataStream::Ok)
        fail(VaultError::Io, QStringLiteral("Не удалось подготовить записи к сохранению."));
}

VaultData deserialize(const QByteArray &payload, bool includeEmail)
{
    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 idle = 0;
    quint32 count = 0;
    stream >> idle >> count;
    if (idle < 1 || idle > 120 || count > maxEntries)
        damaged();
    VaultData data;
    data.idleMinutes = int(idle);
    data.entries.reserve(count);
    QSet<QUuid> ids;
    while (count--) {
        Entry entry;
        stream >> entry.id;
        readField(stream, entry.site);
        readField(stream, entry.url);
        readField(stream, entry.login);
        readField(stream, entry.password);
        if (includeEmail)
            readField(stream, entry.email);
        if (stream.status() != QDataStream::Ok || !validEntry(entry)
            || !validEmail(entry.email) || ids.contains(entry.id))
            damaged();
        ids.insert(entry.id);
        data.entries.push_back(std::move(entry));
    }
    if (!stream.atEnd() || stream.status() != QDataStream::Ok)
        damaged();
    return data;
}

void deriveWriteKey(SecureKey &output, const SecureKey &base, const QByteArray &diversifier)
{
    if (crypto_generichash(output.data(), keySize, unsignedData(diversifier), diversifier.size(),
                           base.data(), keySize) != 0)
        fail(VaultError::Crypto, QStringLiteral("Не удалось получить ключ шифрования файла."));
}

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

CipherContext aesGcm(const SecureKey &key, const unsigned char *nonce, bool encrypt)
{
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context || EVP_CipherInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr, encrypt) != 1
        || EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN, nonceSize, nullptr) != 1
        || EVP_CipherInit_ex(context.get(), nullptr, nullptr, key.data(), nonce, encrypt) != 1)
        fail(VaultError::Crypto, QStringLiteral("Не удалось инициализировать AES-256-GCM."));
    return context;
}

QString comparablePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}
}

void wipe(QString &value)
{
    if (!value.isEmpty())
        sodium_memzero(value.data(), size_t(value.size()) * sizeof(QChar));
    value.clear();
}

void wipe(QByteArray &value)
{
    if (!value.isEmpty())
        sodium_memzero(value.data(), size_t(value.size()));
    value.clear();
}

SecureKey::SecureKey() : bytes(static_cast<unsigned char *>(sodium_malloc(keySize)))
{
    if (!bytes || sodium_mlock(bytes.get(), keySize) != 0)
        fail(VaultError::Crypto, QStringLiteral("Не удалось выделить защищённую память для ключа."));
}

void SecureKey::Deleter::operator()(unsigned char *data) const
{
    sodium_free(data);
}

Entry &Entry::operator=(Entry &&other) noexcept
{
    if (this != &other) {
        id = other.id;
        site.swap(other.site);
        url.swap(other.url);
        login.swap(other.login);
        password.swap(other.password);
        email.swap(other.email);
    }
    return *this;
}

Entry::~Entry()
{
    wipe(site);
    wipe(url);
    wipe(login);
    wipe(password);
    wipe(email);
}

void CryptoManager::initialize()
{
    if (sodium_init() < 0 || OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr) != 1)
        fail(VaultError::Crypto, QStringLiteral("Не удалось инициализировать криптографические библиотеки."));
}

Session CryptoManager::derive(VaultHeader header, const QString &password)
{
    if (header.salt.size() != saltSize
        || (!header.verifier.isEmpty() && header.verifier.size() != keySize))
        damaged();
    SecureKey master;
    {
        SecretBytes utf8{password.toUtf8()};
        if (crypto_pwhash(master.data(), keySize, utf8.value.constData(), utf8.value.size(),
                          unsignedData(std::as_const(header.salt)), header.operations,
                          size_t(header.memoryKiB) * 1024, crypto_pwhash_ALG_ARGON2ID13) != 0)
            fail(VaultError::Crypto, QStringLiteral("Не удалось вычислить ключ Argon2id."));
    }

    Session session{std::move(header), SecureKey{}};
    SecureKey check;
    if (crypto_kdf_derive_from_key(session.key.data(), keySize, 1, "LVault01", master.data()) != 0
        || crypto_kdf_derive_from_key(check.data(), keySize, 2, "LVault01", master.data()) != 0)
        fail(VaultError::Crypto, QStringLiteral("Не удалось разделить ключи хранилища."));

    // Both formats use the original verifier encoding. AES-GCM authenticates the actual file version.
    const QByteArray parameters = headerParameters(session.header, originalFormat);
    QByteArray verifier(keySize, Qt::Uninitialized);
    if (crypto_auth_hmacsha256(unsignedData(verifier), unsignedData(parameters), parameters.size(),
                               check.data()) != 0)
        fail(VaultError::Crypto, QStringLiteral("Не удалось вычислить проверочное значение."));

    if (!session.header.verifier.isEmpty()
        && sodium_memcmp(session.header.verifier.constData(), verifier.constData(), keySize) != 0)
        fail(VaultError::WrongPassword, QStringLiteral("Неверный мастер-пароль."));
    session.header.verifier = std::move(verifier);
    return session;
}

QByteArray CryptoManager::encrypt(const Session &session, const VaultData &data)
{
    const QByteArray diversifier = randomBytes(keySize);
    const QByteArray nonce = randomBytes(nonceSize);
    SecureKey writeKey;
    deriveWriteKey(writeKey, session.key, diversifier);

    const bool includeEmail = std::any_of(data.entries.begin(), data.entries.end(), [](const Entry &entry) {
        return !entry.email.isEmpty();
    });
    QByteArray file = headerParameters(session.header, includeEmail ? emailFormat : originalFormat)
        + session.header.verifier + diversifier + nonce;
    SecretBytes payload;
    serialize(data, payload.value, includeEmail);
    file.resize(headerSize + payload.value.size() + tagSize);
    auto *bytes = unsignedData(file);
    const auto context = aesGcm(writeKey, unsignedData(nonce), true);
    int written = 0;
    int tail = 0;
    if (EVP_EncryptUpdate(context.get(), nullptr, &written, bytes, headerSize) != 1
        || EVP_EncryptUpdate(context.get(), bytes + headerSize, &written,
                             unsignedData(std::as_const(payload.value)), int(payload.value.size())) != 1
        || written != payload.value.size()
        || EVP_EncryptFinal_ex(context.get(), bytes + headerSize + written, &tail) != 1
        || tail != 0
        || EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG, tagSize,
                               bytes + headerSize + written) != 1)
        fail(VaultError::Crypto, QStringLiteral("Не удалось зашифровать хранилище."));
    file.append(QCryptographicHash::hash(file, QCryptographicHash::Sha256));
    return file;
}

VaultData CryptoManager::decrypt(const Session &session, const QByteArray &file)
{
    if (file.size() < headerSize + tagSize + checksumSize + 8 || file.size() > maxFile)
        damaged();
    SecureKey writeKey;
    deriveWriteKey(writeKey, session.key, file.mid(paramsSize + keySize, keySize));
    const auto *bytes = unsignedData(file);
    const int cipherSize = int(file.size() - headerSize - checksumSize - tagSize);
    SecretBytes payload{QByteArray(cipherSize + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized)};
    auto tag = file.mid(headerSize + cipherSize, tagSize);
    const auto context = aesGcm(writeKey, bytes + headerSize - nonceSize, false);
    int written = 0;
    int tail = 0;
    if (EVP_DecryptUpdate(context.get(), nullptr, &written, bytes, headerSize) != 1
        || EVP_DecryptUpdate(context.get(), unsignedData(payload.value), &written,
                             bytes + headerSize, cipherSize) != 1
        || written != cipherSize
        || EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG, tagSize, tag.data()) != 1)
        fail(VaultError::Crypto, QStringLiteral("Не удалось проверить шифротекст AES-256-GCM."));
    if (EVP_DecryptFinal_ex(context.get(), unsignedData(payload.value) + written, &tail) != 1 || tail != 0)
        damaged();
    sodium_memzero(payload.value.data() + written, EVP_MAX_BLOCK_LENGTH);
    payload.value.chop(EVP_MAX_BLOCK_LENGTH);
    const auto version = qFromBigEndian<quint32>(bytes + 4);
    return deserialize(payload.value, version == emailFormat);
}

StorageManager::StorageManager(QString path) : path(std::move(path)) {}

bool StorageManager::exists() const
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

QByteArray StorageManager::read() const
{
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly))
        fail(VaultError::Io, QStringLiteral("Не удалось открыть файл: %1").arg(path));
    if (file.size() > maxFile)
        damaged();
    const QByteArray bytes = file.read(maxFile + 1);
    if (file.error() != QFileDevice::NoError)
        fail(VaultError::Io, QStringLiteral("Не удалось прочитать файл: %1").arg(file.errorString()));
    if (bytes.size() > maxFile)
        damaged();
    return bytes;
}

void StorageManager::write(const QByteArray &bytes) const
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(VaultError::Io, QStringLiteral("Не удалось сохранить файл: %1").arg(file.errorString()));
}

VaultHeader StorageManager::readHeader(const QByteArray &file)
{
    if (file.size() < headerSize + tagSize + checksumSize + 8 || file.size() > maxFile
        || !file.startsWith("LVLT"))
        damaged();
    QDataStream stream(file);
    stream.skipRawData(4);
    quint32 version = 0;
    VaultHeader header;
    stream >> version >> header.operations >> header.memoryKiB;
    if (QCryptographicHash::hash(QByteArrayView(file).chopped(checksumSize), QCryptographicHash::Sha256)
            != file.last(checksumSize))
        damaged();
    if (version != originalFormat && version != emailFormat)
        fail(VaultError::Version, QStringLiteral("Версия формата хранилища не поддерживается."));
    if (header.operations < 2 || header.operations > 6
        || header.memoryKiB < 32 * 1024 || header.memoryKiB > 256 * 1024)
        damaged();
    header.salt = file.mid(paramsSize - saltSize, saltSize);
    header.verifier = file.mid(paramsSize, keySize);
    return header;
}

Session AuthManager::create(const QString &password)
{
    if (password.size() < 12 || password.size() > 1024)
        fail(VaultError::Invalid, QStringLiteral("Мастер-пароль должен содержать от 12 до 1024 символов."));
    VaultHeader header;
    header.salt = randomBytes(saltSize);
    return CryptoManager::derive(std::move(header), password);
}

Session AuthManager::unlock(const QByteArray &file, const QString &password)
{
    return CryptoManager::derive(StorageManager::readHeader(file), password);
}

PasswordManager::PasswordManager(QString path) : storage(path), activePath(std::move(path)) {}

bool PasswordManager::exists() const
{
    return storage.exists();
}

void PasswordManager::open(const QString &password)
{
    const QByteArray file = storage.read();
    auto candidate = AuthManager::unlock(file, password);
    auto decoded = CryptoManager::decrypt(candidate, file);
    records = std::move(decoded);
    session.emplace(std::move(candidate));
}

void PasswordManager::create(const QString &password)
{
    if (exists())
        fail(VaultError::Io, QStringLiteral("Хранилище уже существует."));
    auto candidate = AuthManager::create(password);
    storage.write(CryptoManager::encrypt(candidate, records));
    session.emplace(std::move(candidate));
}

void PasswordManager::lock()
{
    session.reset();
    records = VaultData{};
}

void PasswordManager::changePassword(const QString &current, const QString &next)
{
    activeSession();
    const auto file = storage.read();
    const auto verified = AuthManager::unlock(file, current);
    CryptoManager::decrypt(verified, file);
    auto candidate = AuthManager::create(next);
    storage.write(CryptoManager::encrypt(candidate, records));
    session.emplace(std::move(candidate));
}

void PasswordManager::importFrom(const QString &path, const QString &password)
{
    if (storage.exists())
        activeSession();
    const QByteArray file = StorageManager(path).read();
    auto candidate = AuthManager::unlock(file, password);
    auto decoded = CryptoManager::decrypt(candidate, file);
    storage.write(file);
    records = std::move(decoded);
    session.emplace(std::move(candidate));
}

void PasswordManager::exportTo(const QString &path) const
{
    const auto &current = activeSession();
    if (comparablePath(path).compare(comparablePath(activePath),
#ifdef Q_OS_WIN
            Qt::CaseInsensitive
#else
            Qt::CaseSensitive
#endif
            ) == 0)
        fail(VaultError::Invalid, QStringLiteral("Выберите другой файл для резервной копии."));
    const QByteArray file = storage.read();
    StorageManager::readHeader(file);
    CryptoManager::decrypt(current, file);
    StorageManager(path).write(file);
}

const Session &PasswordManager::activeSession() const
{
    if (!session)
        fail(VaultError::Invalid, QStringLiteral("Хранилище заблокировано."));
    return *session;
}

void PasswordManager::save() const
{
    storage.write(CryptoManager::encrypt(activeSession(), records));
}

void PasswordManager::put(Entry entry)
{
    if (!validEntry(entry))
        fail(VaultError::Invalid, QStringLiteral("Укажите сайт, логин, пароль и ссылку на сайт с https:// или http://."));
    if (!validEmail(entry.email))
        fail(VaultError::Invalid, QStringLiteral("Введите адрес электронной почты в формате имя@домен."));
    auto &entries = records.entries;
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry &item) {
        return item.id == entry.id;
    });
    if (it == entries.end()) {
        entries.push_back(std::move(entry));
        try {
            save();
        } catch (...) {
            entries.pop_back();
            throw;
        }
        return;
    }
    Entry previous(std::move(*it));
    *it = std::move(entry);
    try {
        save();
    } catch (...) {
        *it = std::move(previous);
        throw;
    }
}

void PasswordManager::remove(const QUuid &id)
{
    auto &entries = records.entries;
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry &item) {
        return item.id == id;
    });
    if (it == entries.end())
        return;
    const auto index = it - entries.begin();
    Entry previous(std::move(*it));
    entries.erase(it);
    try {
        save();
    } catch (...) {
        entries.insert(entries.begin() + index, std::move(previous));
        throw;
    }
}

void PasswordManager::setIdleMinutes(int minutes)
{
    if (minutes < 1 || minutes > 120)
        fail(VaultError::Invalid, QStringLiteral("Укажите время от 1 до 120 минут."));
    const int previous = std::exchange(records.idleMinutes, minutes);
    try {
        save();
    } catch (...) {
        records.idleMinutes = previous;
        throw;
    }
}

QString PasswordGenerator::generate(GeneratorOptions options)
{
    QList<QString> groups;
    if (options.lowercase)
        groups.append(QStringLiteral("abcdefghijklmnopqrstuvwxyz"));
    if (options.uppercase)
        groups.append(QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
    if (options.digits)
        groups.append(QStringLiteral("0123456789"));
    if (options.symbols)
        groups.append(QStringLiteral("!@#$%^&*()-_=+[]{};:,.?"));
    if (groups.isEmpty() || options.length < groups.size() || options.length > 256)
        fail(VaultError::Invalid, QStringLiteral("Выберите наборы символов и допустимую длину."));
    QString alphabet;
    QString result;
    result.reserve(options.length);
    QList<int> groupForCharacter;
    for (int i = 0; i < groups.size(); ++i) {
        const auto &group = groups.at(i);
        alphabet += group;
        groupForCharacter.append(QList<int>(group.size(), i));
    }
    result.resize(options.length);
    int usedGroups = 0;
    const int allGroups = (1 << groups.size()) - 1;
    do {
        usedGroups = 0;
        for (auto &character : result) {
            const auto index = randombytes_uniform(quint32(alphabet.size()));
            character = alphabet.at(index);
            usedGroups |= 1 << groupForCharacter.at(index);
        }
    } while (usedGroups != allGroups);
    return result;
}

int PasswordGenerator::strength(const QString &password)
{
    if (password.isEmpty())
        return 0;
    bool lower = false, upper = false, digit = false, other = false;
    QSet<QChar> distinct;
    for (const auto character : password) {
        distinct.insert(character);
        lower |= character.isLower();
        upper |= character.isUpper();
        digit |= character.isDigit();
        other |= !character.isLetterOrNumber();
    }
    if (password.size() < 8 || distinct.size() < 5)
        return 1;
    if (password.size() < 12 || distinct.size() < 8)
        return 2;
    if (password.size() >= 18 && distinct.size() >= 12 && lower + upper + digit + other >= 2)
        return 4;
    return 3;
}

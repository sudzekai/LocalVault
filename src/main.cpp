#include "MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QLibraryInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("LocalVault"));
    app.setDesktopFileName(QStringLiteral("localvault"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/localvault.png")));
    app.setStyle(QStringLiteral("Fusion"));
    QTranslator translator;
    if (translator.load(QStringLiteral("qt_ru"), app.applicationDirPath() + QStringLiteral("/translations"))
        || translator.load(QStringLiteral("qtbase_ru"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&translator);
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        QMessageBox::critical(nullptr, QStringLiteral("LocalVault"),
                              QStringLiteral("Не удалось создать каталог хранилища."));
        return 1;
    }

    QLockFile lock(QDir(directory).filePath(QStringLiteral("vault.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        QMessageBox::critical(nullptr, QStringLiteral("LocalVault"),
                              QStringLiteral("Хранилище уже открыто или каталог недоступен для записи."));
        return 1;
    }

    try {
        CryptoManager::initialize();
        MainWindow window(QDir(directory).filePath(QStringLiteral("vault.lvault")));
        window.show();
        return app.exec();
    } catch (const Failure &error) {
        QMessageBox::critical(nullptr, QStringLiteral("LocalVault"), error.message);
        return 1;
    }
}

#include "MainWindow.h"
#include "Dialogs.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QCollator>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QScopeGuard>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>

#include <algorithm>
#include <QUrl>

namespace {
constexpr char tokenFormat[] = "application/x-localvault-copy-token";
}

MainWindow::MainWindow(const QString &path)
    : defaultVaultPath(path),
      locationSettingsPath(QFileInfo(path).absoluteDir().filePath(QStringLiteral("vault-location.ini"))),
      manager(path)
{
    QSettings location(locationSettingsPath, QSettings::IniFormat);
    const auto customPath = location.value(QStringLiteral("vaultPath")).toString();
    if (location.status() != QSettings::NoError || (!customPath.isEmpty() && !QDir::isAbsolutePath(customPath)))
        locationError = QStringLiteral("Не удалось прочитать сохранённый путь. Выберите файл или сбросьте путь в настройках.");
    else if (!customPath.isEmpty())
        manager.selectPath(customPath, true);

    setWindowTitle(QStringLiteral("LocalVault"));
    setMinimumSize(780, 520);
    resize(1080, 690);
    auto theme = palette();
    theme.setColor(QPalette::Window, QColor("#1a1a1a"));
    theme.setColor(QPalette::Base, QColor("#1b1b1b"));
    theme.setColor(QPalette::AlternateBase, QColor("#202020"));
    theme.setColor(QPalette::Button, QColor("#2c2c2c"));
    theme.setColor(QPalette::Highlight, QColor("#484848"));
    theme.setColor(QPalette::HighlightedText, Qt::white);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        theme.setColor(role, QColor("#f2f2f2"));
    theme.setColor(QPalette::PlaceholderText, QColor("#a8a8a8"));
    theme.setColor(QPalette::Disabled, QPalette::Text, QColor("#858585"));
    setPalette(theme);
    setStyleSheet(QStringLiteral(R"(
        QWidget { color: #f2f2f2; background: #1a1a1a; font-size: 14px; }
        QLabel, QCheckBox { background: transparent; }
        QLabel#heading { font-size: 26px; font-weight: 700; }
        QLabel#brand { font-size: 19px; font-weight: 700; }
        QLabel#cardTitle { font-size: 17px; font-weight: 600; }
        QLabel#secondary { color: #b3b3b3; }
        QLabel#error { color: #ffaba9; }
        QWidget#sidebar { background: #181818; border-right: 1px solid #2c2c2c; }
        QWidget#accountCard { background: #202020; border: 1px solid #343434; border-radius: 12px; }
        QWidget#accountCard QLabel { border: none; background: transparent; }
        QPushButton { background: #2c2c2c; border: 1px solid #434343; padding: 6px; border-radius: 7px; }
        QPushButton:hover, QPushButton:checked { background: #3d3d3d; }
        QPushButton:disabled { background: #202020; color: #858585; border-color: #343434; }
        QPushButton#primary { background: #414141; color: #ffffff; border-color: #646464; font-weight: 600; }
        QPushButton#primary:hover { background: #515151; }
        QLineEdit, QSpinBox, QDateTimeEdit { background: #1b1b1b; border: 1px solid #454545; border-radius: 7px;
            padding: 7px; selection-background-color: #505050; }
        QLineEdit:focus, QSpinBox:focus, QDateTimeEdit:focus, QPushButton:focus, QPushButton#primary:focus {
            border-color: #a3a3a3; }
        QListWidget, QScrollArea { background: transparent; border: 0; }
        QListWidget::item { min-height: 30px; padding: 4px 8px; border-radius: 6px; }
        QListWidget::item:hover { background: #2c2c2c; }
        QListWidget::item:selected { background: #343434; color: #ffffff; }
        QProgressBar { background: #202020; border: 1px solid #434343; text-align: center; }
        QProgressBar::chunk { background: #555555; }
        QToolTip { background: #2c2c2c; color: #f2f2f2; border: 1px solid #555555; }
    )"));

    pages = new QStackedWidget(this);
    setCentralWidget(pages);
    lockedPage = new QWidget(pages);
    auto *lockedLayout = new QVBoxLayout(lockedPage);
    auto *card = new QWidget(lockedPage);
    card->setFixedWidth(470);
    auto *auth = new QVBoxLayout(card);
    auth->setSpacing(12);
    authTitle = label({}, card, "heading");
    authHint = label({}, card);
    authError = label({}, card, "error");
    master = secretField(card, 1024);
    master->setPlaceholderText(QStringLiteral("Мастер-пароль"));
    repeat = secretField(card, 1024);
    repeat->setPlaceholderText(QStringLiteral("Повторите мастер-пароль"));
    for (auto *widget : QList<QWidget *>{authTitle, authHint, master, repeat, authError})
        auth->addWidget(widget);
    auto *authRow = new QHBoxLayout;
    auth->addLayout(authRow);
    authButton = button(authRow, card, {}, [this] {
        authError->clear();
        QString password = takeText(master);
        QString again = takeText(repeat);
        const auto cleanup = qScopeGuard([&] { wipe(password); wipe(again); });
        try {
            if (manager.usesCustomPath() || manager.exists()) {
                manager.open(password);
            } else {
                if (password != again)
                    throw Failure{VaultError::Invalid, QStringLiteral("Мастер-пароли не совпадают.")};
                manager.create(password);
            }
            showUnlocked();
        } catch (const Failure &failure) {
            authError->setText(failure.message);
            master->setFocus();
        }
    }, true);
    authRow->addStretch();
    button(authRow, card, QStringLiteral("Импортировать копию"), [this] { importVault(); });
    button(authRow, card, QStringLiteral("Настройки"), [this] { vaultLocationDialog(); });
    connect(master, &QLineEdit::returnPressed, this, [this] {
        if (repeat->isVisible())
            repeat->setFocus();
        else
            authButton->click();
    });
    connect(repeat, &QLineEdit::returnPressed, authButton, &QPushButton::click);
    lockedLayout->addStretch();
    lockedLayout->addWidget(card, 0, Qt::AlignHCenter);
    lockedLayout->addStretch();
    pages->addWidget(lockedPage);

    vaultPage = new QWidget(pages);
    auto *shell = new QHBoxLayout(vaultPage);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    auto *sidebar = new QWidget(vaultPage);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(246);
    auto *navigation = new QVBoxLayout(sidebar);
    navigation->setContentsMargins(16, 18, 16, 16);
    navigation->setSpacing(12);
    auto *navHeader = new QHBoxLayout;
    navHeader->addWidget(label(QStringLiteral("Сайты"), sidebar, "brand"));
    navHeader->addStretch();
    iconButton(navHeader, sidebar, QStringLiteral("Добавить сайт"), "plus-lg", [this] { siteDialog(); });
    navigation->addLayout(navHeader);
    search = new QLineEdit(sidebar);
    search->setPlaceholderText(QStringLiteral("Поиск по названию"));
    search->setAccessibleName(QStringLiteral("Поиск сайтов"));
    search->setClearButtonEnabled(true);
    search->setMaxLength(2048);
    navigation->addWidget(search);
    sites = new QListWidget(sidebar);
    sites->setAccessibleName(QStringLiteral("Сайты"));
    sites->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation->addWidget(sites, 1);
    shell->addWidget(sidebar);

    auto *content = new QWidget(vaultPage);
    auto *vault = new QVBoxLayout(content);
    vault->setContentsMargins(24, 18, 24, 14);
    vault->setSpacing(16);
    auto *top = new QHBoxLayout;
    top->addWidget(label(QStringLiteral("LocalVault"), content, "brand"));
    top->addStretch();
    iconButton(top, content, QStringLiteral("Генератор"), "shuffle", [this] {
        GeneratorDialog dialog(this, QStringLiteral("Копировать пароль"));
        if (dialog.exec() == QDialog::Accepted && manager.unlocked()) {
            QString value = dialog.takePassword();
            copy(value);
            wipe(value);
        }
    });
    iconButton(top, content, QStringLiteral("Импорт"), "box-arrow-in-down", [this] { importVault(); });
    iconButton(top, content, QStringLiteral("Экспорт"), "box-arrow-up", [this] { exportVault(); });
    iconButton(top, content, QStringLiteral("Настройки"), "gear", [this] { settings(); });
    iconButton(top, content, QStringLiteral("Заблокировать"), "lock", [this] { lock(); });
    vault->addLayout(top);

    auto *siteHeader = new QHBoxLayout;
    auto *siteText = new QVBoxLayout;
    siteText->setSpacing(2);
    siteTitle = label({}, content, "heading");
    siteAddress = label({}, content);
    siteText->addWidget(siteTitle);
    siteText->addWidget(siteAddress);
    siteHeader->addLayout(siteText, 1);
    siteActions = {
        iconButton(siteHeader, content, QStringLiteral("Открыть сайт"), "box-arrow-up-right", [this] {
            if (const auto *site = currentSite()) {
                if (!QDesktopServices::openUrl(QUrl(site->url, QUrl::StrictMode)))
                    showError({VaultError::Io, QStringLiteral("Не удалось открыть ссылку в браузере.")});
            }
        }),
        iconButton(siteHeader, content, QStringLiteral("Изменить сайт"), "pencil", [this] {
            if (const auto *site = currentSite())
                siteDialog(site);
        }),
        iconButton(siteHeader, content, QStringLiteral("Удалить сайт"), "trash", [this] { removeSite(); }),
        iconButton(siteHeader, content, QStringLiteral("Добавить аккаунт"), "plus-lg",
                   [this] { if (currentSite()) entryDialog(nullptr); })
    };
    vault->addLayout(siteHeader);
    cards = new QScrollArea(content);
    cards->setObjectName(QStringLiteral("accountCards"));
    cards->setWidgetResizable(true);
    cards->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    vault->addWidget(cards, 1);

    auto *footer = new QHBoxLayout;
    status = label({}, content);
    idleLabel = label({}, content);
    status->setWordWrap(false);
    idleLabel->setWordWrap(false);
    footer->addWidget(status);
    footer->addStretch();
    footer->addWidget(idleLabel);
    vault->addLayout(footer);
    shell->addWidget(content, 1);
    connect(sites, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        selectedSite = item ? item->data(Qt::UserRole).toUuid() : QUuid{};
        renderSite();
    });
    connect(search, &QLineEdit::textChanged, this, [this] { refresh(); });
    pages->addWidget(vaultPage);

    idleTimer.setInterval(500);
    connect(&idleTimer, &QTimer::timeout, this, [this] {
        if (activity.elapsed() >= qint64(manager.data().idleMinutes) * 60000)
            lock();
    });
    clipboardTimer.setSingleShot(true);
    connect(&clipboardTimer, &QTimer::timeout, this, [this] { clearClipboard(); });
    qApp->installEventFilter(this);
    showLocked();
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    clearClipboard();
    clearText(master);
    clearText(repeat);
    lock();
}

bool MainWindow::eventFilter(QObject *object, QEvent *event)
{
    if (manager.unlocked()) {
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseMove
            || type == QEvent::KeyPress || type == QEvent::Wheel || type == QEvent::TouchBegin) {
            if (activity.elapsed() >= qint64(manager.data().idleMinutes) * 60000) {
                lock();
                return true;
            }
            activity.restart();
        }
    }
    return QMainWindow::eventFilter(object, event);
}

void MainWindow::showLocked()
{
    pages->setCurrentWidget(lockedPage);
    const bool existing = !locationError.isEmpty() || manager.usesCustomPath() || manager.exists();
    authTitle->setText(existing ? QStringLiteral("Хранилище заблокировано") : QStringLiteral("Создайте хранилище"));
    authHint->setText(existing ? QStringLiteral("Введите мастер-пароль, чтобы открыть записи.")
                               : QStringLiteral("Установите мастер-пароль длиной не менее 12 символов."));
    authButton->setText(existing ? QStringLiteral("Разблокировать") : QStringLiteral("Создать"));
    authButton->setEnabled(locationError.isEmpty());
    repeat->setVisible(!existing);
    authError->setText(locationError);
    try {
        manager.checkAvailability();
    } catch (const Failure &error) {
        authError->setText(error.message);
    }
    master->setFocus();
}

void MainWindow::showUnlocked()
{
    activity.restart();
    idleTimer.start();
    pages->setCurrentWidget(vaultPage);
    idleLabel->setText(QStringLiteral("Автоблокировка через %1 мин.").arg(manager.data().idleMinutes));
    refresh();
    search->setFocus();
}

void MainWindow::lock()
{
    idleTimer.stop();
    selectedSite = QUuid{};
    {
        const QSignalBlocker blockSites(sites);
        const QSignalBlocker blockSearch(search);
        sites->clear();
        clearText(search);
    }
    renderSite();
    clearClipboard();
    manager.lock();
    for (auto *dialog : findChildren<QDialog *>()) {
        if (dialog->isVisible())
            dialog->reject();
    }
    showLocked();
}

void MainWindow::refresh(const QUuid &siteId)
{
    if (!siteId.isNull())
        selectedSite = siteId;

    QString query = search->text();
    const auto cleanup = qScopeGuard([&] { wipe(query); });
    const auto filter = QStringView(query).trimmed();
    std::vector<const Site *> matching;
    for (const auto &site : manager.data().sites) {
        if (site.name.contains(filter, Qt::CaseInsensitive))
            matching.push_back(&site);
    }
    QCollator collator(QLocale(QLocale::Russian, QLocale::Russia));
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(matching.begin(), matching.end(), [&](const Site *left, const Site *right) {
        return collator.compare(left->name, right->name) < 0;
    });

    const QSignalBlocker blocker(sites);
    sites->clear();
    const auto globe = actionIcon("globe2");
    QListWidgetItem *active = nullptr;
    for (const auto *site : matching) {
        auto *item = new QListWidgetItem(globe, site->name, sites);
        item->setData(Qt::UserRole, site->id);
        if (site->id == selectedSite)
            active = item;
    }
    if (!active && sites->count() > 0)
        active = sites->item(0);
    sites->setCurrentItem(active);
    selectedSite = active ? active->data(Qt::UserRole).toUuid() : QUuid{};
    renderSite();
}

const Site *MainWindow::currentSite() const
{
    for (const auto &site : manager.data().sites) {
        if (site.id == selectedSite)
            return &site;
    }
    return nullptr;
}

const Entry *MainWindow::account(const QUuid &id) const
{
    for (const auto &entry : manager.data().entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

void MainWindow::renderSite()
{
    if (auto *previous = cards->takeWidget()) {
        previous->hide();
        for (auto *text : previous->findChildren<QLabel *>())
            text->clear();
        previous->setParent(cards);
        previous->deleteLater();
    }

    const auto *site = currentSite();
    for (auto *action : siteActions)
        action->setVisible(site != nullptr);
    accountCount = 0;
    auto *body = new QWidget(cards);
    auto *list = new QVBoxLayout(body);
    list->setContentsMargins(0, 0, 8, 4);
    list->setSpacing(10);
    cards->setWidget(body);

    if (!site) {
        const bool filtered = !search->text().trimmed().isEmpty();
        siteTitle->setText(filtered ? QStringLiteral("Сайты не найдены") : QStringLiteral("Хранилище"));
        siteAddress->setText(filtered ? QStringLiteral("Попробуйте другое название.")
                                     : QStringLiteral("Добавьте сайт, чтобы сохранить первый аккаунт."));
        updateAccountCount();
        return;
    }

    siteTitle->setText(site->name);
    siteAddress->setText(site->url);
    for (const auto &entry : manager.data().entries) {
        if (entry.siteId != site->id)
            continue;
        ++accountCount;
        auto *card = new QWidget(body);
        card->setObjectName(QStringLiteral("accountCard"));
        card->setAttribute(Qt::WA_StyledBackground);
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(5);
        auto *row = new QHBoxLayout;
        auto *identity = new QVBoxLayout;
        identity->setSpacing(3);
        const bool hasName = !entry.userName.isEmpty();
        identity->addWidget(label(hasName ? entry.userName : entry.login, card, "cardTitle"));
        if (hasName)
            identity->addWidget(label(entry.login, card));
        row->addLayout(identity, 1);
        const auto id = entry.id;
        iconButton(row, card, QStringLiteral("Просмотр аккаунта"), "eye", [this, id] {
            if (const auto *item = account(id))
                entryDialog(item, true);
        });
        iconButton(row, card, QStringLiteral("Копировать логин"), "copy", [this, id] {
            if (const auto *item = account(id))
                copy(item->login);
        });
        iconButton(row, card, QStringLiteral("Копировать пароль"), "key", [this, id] {
            if (const auto *item = account(id))
                copy(item->password);
        });
        iconButton(row, card, QStringLiteral("Изменить аккаунт"), "pencil", [this, id] {
            if (const auto *item = account(id))
                entryDialog(item);
        });
        iconButton(row, card, QStringLiteral("Удалить аккаунт"), "trash", [this, id] { removeEntry(id); });
        layout->addLayout(row);
        if (!entry.emails.empty()) {
            QString summary = entry.emails.front().address;
            if (entry.emails.size() > 1)
                summary += QStringLiteral("  +%1").arg(entry.emails.size() - 1);
            layout->addWidget(label(summary, card));
            wipe(summary);
        }
        list->addWidget(card);
    }
    if (accountCount == 0)
        list->addWidget(label(QStringLiteral("У этого сайта пока нет аккаунтов."), body));
    list->addStretch();
    updateAccountCount();
}

void MainWindow::updateAccountCount()
{
    status->setText(selectedSite.isNull() ? QString{} : QStringLiteral("Аккаунтов: %1").arg(accountCount));
}

void MainWindow::copy(const QString &value)
{
    clipboardToken = QUuid::createUuid().toRfc4122();
    auto *data = new QMimeData;
    data->setText(value);
    data->setData(QString::fromLatin1(tokenFormat), clipboardToken);
#ifdef Q_OS_WIN
    data->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanIncludeInClipboardHistory\""), QByteArray(4, '\0'));
    data->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanUploadToCloudClipboard\""), QByteArray(4, '\0'));
#endif
    QApplication::clipboard()->setMimeData(data);
    clipboardTimer.start(25000);
    status->setText(QStringLiteral("Скопировано · очистка через 25 секунд"));
}

void MainWindow::clearClipboard()
{
    clipboardTimer.stop();
    if (clipboardToken.isEmpty())
        return;
    auto *clipboard = QApplication::clipboard();
    const auto *data = clipboard->mimeData();
    if (data && data->data(QString::fromLatin1(tokenFormat)) == clipboardToken)
        clipboard->clear();
    clipboardToken.clear();
    if (manager.unlocked())
        updateAccountCount();
}

void MainWindow::showError(const Failure &error)
{
    QMessageBox dialog(QMessageBox::Warning, QStringLiteral("LocalVault"), error.message, QMessageBox::Ok, this);
    dialog.setTextFormat(Qt::PlainText);
    setAction(dialog.button(QMessageBox::Ok), QStringLiteral("Закрыть"), "check-lg");
    dialog.exec();
}

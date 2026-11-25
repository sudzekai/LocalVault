#include "MainWindow.h"
#include "Dialogs.h"
#include "IconPicker.h"
#include "RemoteFile.h"
#include "TokenStore.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QCollator>
#include <QTreeWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QLocale>
#include <QMessageBox>
#include <QMenu>
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
constexpr int groupRole = Qt::UserRole + 1;

class SiteTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override
    {
        painter->fillRect(rect, QColor(QStringLiteral("#181818")));
        if (index.data(groupRole).toBool()) {
            const auto icon = siteIcon(isExpanded(index) ? QStringLiteral("chevron-down")
                                                        : QStringLiteral("chevron-right"));
            const int x = rect.right() - indentation() / 2 - 5;
            icon.paint(painter, QRect(x, rect.center().y() - 5, 10, 10));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        auto *item = itemAt(event->position().toPoint());
        if (event->button() == Qt::LeftButton && item && item->data(0, groupRole).toBool()) {
            event->accept();
            setCurrentItem(item);
            item->setExpanded(!item->isExpanded());
            return;
        }
        QTreeWidget::mousePressEvent(event);
    }
};
}

MainWindow::MainWindow(const QString &path)
    : defaultVaultPath(path),
      locationSettingsPath(QFileInfo(path).absoluteDir().filePath(QStringLiteral("vault-location.ini"))),
      manager(path)
{
    QSettings location(locationSettingsPath, QSettings::IniFormat);
    const auto customPath = location.value(QStringLiteral("vaultPath")).toString();
    if (location.status() != QSettings::NoError
        || (!customPath.isEmpty() && !StorageManager::isRemoteSource(customPath) && !QDir::isAbsolutePath(customPath)))
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
        QPushButton#siteCard { background: #202020; border: 1px solid #343434; border-radius: 12px; padding: 0; }
        QPushButton#siteCard:hover { background: #2c2c2c; border-color: #555555; }
        QPushButton#siteCard:focus { border-color: #a3a3a3; }
        QPushButton#siteCard QLabel { background: transparent; border: none; }
        QPushButton { background: #2c2c2c; border: 1px solid #434343; padding: 6px; border-radius: 7px; }
        QPushButton:hover, QPushButton:checked { background: #3d3d3d; }
        QPushButton:disabled { background: #202020; color: #858585; border-color: #343434; }
        QPushButton#primary { background: #414141; color: #ffffff; border-color: #646464; font-weight: 600; }
        QPushButton#primary:hover { background: #515151; }
        QLineEdit, QPlainTextEdit, QSpinBox, QDateTimeEdit, QComboBox { background: #1b1b1b; border: 1px solid #454545; border-radius: 7px;
            padding: 7px; selection-background-color: #505050; }
        QLineEdit:focus, QSpinBox:focus, QDateTimeEdit:focus, QPushButton:focus, QPushButton#primary:focus {
            border-color: #a3a3a3; }
        QTreeWidget, QScrollArea { background: transparent; border: 0; }
        QTreeWidget { outline: 0; }
        QTreeWidget::branch, QTreeWidget::branch:selected { background: #181818; border: 0; }
        QTreeWidget::item { min-height: 30px; padding: 4px 8px; border-radius: 6px; }
        QTreeWidget::item:hover { background: #2c2c2c; }
        QTreeWidget::item:selected { background: #343434; color: #ffffff; }
        QProgressBar { background: #202020; border: 1px solid #434343; text-align: center; }
        QProgressBar::chunk { background: #555555; }
        QToolTip { background: #2c2c2c; color: #f2f2f2; border: 1px solid #555555; }
        QMenu { background: #202020; color: #f2f2f2; border: 1px solid #454545; border-radius: 9px; padding: 5px; }
        QMenu::item { background: transparent; padding: 8px 26px 8px 12px; border-radius: 5px; }
        QMenu::item:selected { background: #343434; color: #ffffff; }
        QMenu::item:disabled { color: #858585; }
        QMenu::separator { height: 1px; background: #454545; margin: 5px 8px; }
        QMenu::icon { padding-left: 5px; }
    )"));

    pages = new QStackedWidget(this);
    setCentralWidget(pages);
    lockedPage = new QWidget(pages);
    auto *lockedLayout = new QVBoxLayout(lockedPage);
    auto *card = new QWidget(lockedPage);
    card->setFixedWidth(510);
    auto *auth = new QVBoxLayout(card);
    auth->setSpacing(12);
    authTitle = label({}, card, "heading");
    authHint = label({}, card);
    authError = label({}, card, "error");
    master = secretField(card, 1024);
    master->setPlaceholderText(QStringLiteral("Мастер-пароль"));
    repeat = secretField(card, 1024);
    repeat->setPlaceholderText(QStringLiteral("Повторите мастер-пароль"));
    apiTokenField = secretField(card, 1024);
    apiTokenField->setPlaceholderText(QStringLiteral("API-токен сервера"));
    apiTokenField->setAccessibleName(QStringLiteral("API-токен сервера"));
    QString tokenError;
    if (manager.isRemote()) {
        try {
            auto token = TokenStore::restore(location.value(QStringLiteral("apiTokenProtected")).toByteArray(),
                                            locationSettingsPath + '\n' + manager.path());
            apiTokenField->setText(QString::fromUtf8(token));
            manager.setApiToken(token);
            wipe(token);
        } catch (const Failure &failure) {
            tokenError = failure.message;
        }
    }
    for (auto *widget : QList<QWidget *>{authTitle, authHint, apiTokenField, master, repeat, authError})
        auth->addWidget(widget);
    auto *authRow = new QHBoxLayout;
    auth->addLayout(authRow);
    authButton = button(authRow, card, {}, [this] {
        authError->clear();
        QString password = takeText(master);
        QString again = takeText(repeat);
        const auto cleanup = qScopeGuard([&] { wipe(password); wipe(again); });
        try {
            if (manager.isRemote()) {
                QString token = apiTokenField->text();
                manager.setApiToken(token.toUtf8());
                wipe(token);
            }
            if (manager.usesCustomPath() || manager.exists()) {
                manager.open(password);
            } else {
                if (password != again)
                    throw Failure{VaultError::Invalid, QStringLiteral("Мастер-пароли не совпадают.")};
                manager.create(password);
            }
            if (manager.isRemote()) {
                QString token = apiTokenField->text();
                const auto cleanupToken = qScopeGuard([&] { wipe(token); });
                QSettings settings(locationSettingsPath, QSettings::IniFormat);
                try {
                    settings.setValue(QStringLiteral("apiTokenProtected"), TokenStore::protect(token.toUtf8(), locationSettingsPath + '\n' + manager.path()));
                    settings.sync();
                    if (settings.status() != QSettings::NoError)
                        throw Failure{VaultError::Io, QStringLiteral("Не удалось сохранить API-токен.")};
                } catch (...) {
                    manager.lock();
                    throw;
                }
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

    sidebar = new QWidget(vaultPage);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(246);
    auto *navigation = new QVBoxLayout(sidebar);
    navigation->setContentsMargins(16, 18, 16, 16);
    navigation->setSpacing(12);
    auto *navHeader = new QHBoxLayout;
    navHeader->addWidget(label(QStringLiteral("LocalVault"), sidebar, "brand"));
    navHeader->addStretch();
    navigation->addLayout(navHeader);
    search = new QLineEdit(sidebar);
    search->setPlaceholderText(QStringLiteral("Поиск по названию"));
    search->setAccessibleName(QStringLiteral("Поиск сайтов"));
    search->setClearButtonEnabled(true);
    search->setMaxLength(2048);
    navigation->addWidget(search);
    auto *tree = new SiteTree(sidebar);
    sites = tree;
    sites->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(sites, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        sidebarMenu(sites->viewport()->mapToGlobal(point), sites->itemAt(point));
    });
    sidebar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(sidebar, &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        sidebarMenu(sidebar->mapToGlobal(point), nullptr);
    });
    sites->setAccessibleName(QStringLiteral("Сайты"));
    sites->setHeaderHidden(true);
    sites->setIndentation(18);
    sites->setAnimated(false);
    sites->setExpandsOnDoubleClick(false);
    sites->setUniformRowHeights(true);
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
    connect(sites, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        const bool group = item && item->data(0, groupRole).toBool();
        selectedSite = item && !group ? item->data(0, Qt::UserRole).toUuid() : QUuid{};
        selectedGroup = group ? item->data(0, Qt::UserRole).toUuid()
            : item && item->parent() ? item->parent()->data(0, Qt::UserRole).toUuid() : QUuid{};
        renderSite();
    });
    connect(search, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(sites, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        expandedGroups.insert(item->data(0, Qt::UserRole).toUuid());
    });
    connect(sites, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        expandedGroups.remove(item->data(0, Qt::UserRole).toUuid());
    });
    pages->addWidget(vaultPage);

    idleTimer.setInterval(500);
    connect(&idleTimer, &QTimer::timeout, this, [this] {
        if (RemoteFile::requestActive())
            return;
        if (activity.elapsed() >= qint64(manager.data().idleMinutes) * 60000)
            lock();
    });
    clipboardTimer.setSingleShot(true);
    connect(&clipboardTimer, &QTimer::timeout, this, [this] { clearClipboard(); });
    qApp->installEventFilter(this);
    showLocked();
    if (!tokenError.isEmpty())
        authError->setText(tokenError + QStringLiteral(" Введите токен заново."));
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    clearClipboard();
    clearText(master);
    clearText(repeat);
    clearText(apiTokenField);
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
    if (manager.isRemote())
        authHint->setText(QStringLiteral("Введите API-токен сервера и мастер-пароль хранилища."));
    apiTokenField->setVisible(manager.isRemote());
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
    selectedGroup = QUuid{};
    expandedGroups.clear();
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
    QHash<QUuid, QTreeWidgetItem *> groupItems;
    QList<QTreeWidgetItem *> roots;
    for (const auto &group : manager.data().groups) {
        auto *item = new QTreeWidgetItem;
        item->setText(0, group.name);
        item->setIcon(0, siteIcon(group.iconName));
        item->setData(0, Qt::UserRole, group.id);
        item->setData(0, groupRole, true);
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        groupItems.insert(group.id, item);
        roots.append(item);
    }
    QTreeWidgetItem *active = nullptr;
    for (const auto *site : matching) {
        const auto icon = siteIcon(site->iconName);
        auto *item = new QTreeWidgetItem;
        item->setText(0, site->name);
        item->setIcon(0, icon.isNull() ? globe : icon);
        item->setData(0, Qt::UserRole, site->id);
        if (auto *parent = groupItems.value(site->groupId))
            parent->addChild(item);
        else
            roots.append(item);
        if (site->id == selectedSite)
            active = item;
    }
    for (auto it = roots.begin(); it != roots.end();) {
        auto *item = *it;
        if (item->data(0, groupRole).toBool() && item->childCount() == 0 && !filter.isEmpty()) {
            delete item;
            it = roots.erase(it);
        } else {
            ++it;
        }
    }
    std::sort(roots.begin(), roots.end(), [&](const QTreeWidgetItem *left, const QTreeWidgetItem *right) {
        return collator.compare(left->text(0), right->text(0)) < 0;
    });
    sites->addTopLevelItems(roots);
    for (auto *item : roots) {
        if (!item->data(0, groupRole).toBool())
            continue;
        const auto id = item->data(0, Qt::UserRole).toUuid();
        item->setExpanded(expandedGroups.contains(id) || !filter.isEmpty());
        if (!active && id == selectedGroup)
            active = item;
    }
    if (!active && !roots.isEmpty())
        active = roots.front();
    if (active && active->parent() && (!siteId.isNull() || !filter.isEmpty())) {
        active->parent()->setExpanded(true);
        expandedGroups.insert(active->parent()->data(0, Qt::UserRole).toUuid());
    }
    sites->setCurrentItem(active);
    const bool group = active && active->data(0, groupRole).toBool();
    selectedSite = active && !group ? active->data(0, Qt::UserRole).toUuid() : QUuid{};
    selectedGroup = group ? active->data(0, Qt::UserRole).toUuid()
        : active && active->parent() ? active->parent()->data(0, Qt::UserRole).toUuid() : QUuid{};
    renderSite();
}

void MainWindow::createSiteInGroup(const QUuid &groupId)
{
    if (!manager.unlocked())
        return;
    const auto previousSite = selectedSite;
    const auto previousGroup = selectedGroup;
    selectedSite = QUuid{};
    selectedGroup = groupId;
    siteDialog();
    if (manager.unlocked() && selectedSite.isNull()) {
        selectedSite = previousSite;
        selectedGroup = previousGroup;
    }
}

void MainWindow::sidebarMenu(const QPoint &position, QTreeWidgetItem *item)
{
    if (!manager.unlocked())
        return;
    sites->setCurrentItem(item);
    QMenu menu(this);
    menu.setWindowFlag(Qt::FramelessWindowHint);
    menu.setAttribute(Qt::WA_TranslucentBackground);
    const auto add = [&](const QString &text, const char *icon, const std::function<void()> &action) {
        auto *entry = menu.addAction(actionIcon(icon), text);
        connect(entry, &QAction::triggered, this, [this, action] {
            if (manager.unlocked())
                action();
        });
    };
    const bool group = item && item->data(0, groupRole).toBool();
    const auto groupId = group ? item->data(0, Qt::UserRole).toUuid()
        : item && item->parent() ? item->parent()->data(0, Qt::UserRole).toUuid() : QUuid{};
    add(QStringLiteral("Добавить сайт"), "plus-lg", [this, groupId] { createSiteInGroup(groupId); });
    if (!item) {
        add(QStringLiteral("Создать группу"), "plus-lg", [this] { groupDialog(); });
    } else if (group) {
        menu.addSeparator();
        add(QStringLiteral("Изменить группу"), "pencil", [this] { groupDialog(true); });
        add(QStringLiteral("Удалить группу"), "trash", [this] { removeGroup(); });
    } else {
        menu.addSeparator();
        add(QStringLiteral("Изменить сайт"), "pencil", [this] {
            if (const auto *site = currentSite())
                siteDialog(site);
        });
        add(QStringLiteral("Удалить сайт"), "trash", [this] { removeSite(); });
    }
    menu.exec(position);
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
        if (!selectedGroup.isNull()) {
            for (const auto &group : manager.data().groups) {
                if (group.id == selectedGroup) {
                    siteTitle->setText(group.name);
                    siteAddress->clear();
                    std::vector<const Site *> groupedSites;
                    for (const auto &item : manager.data().sites) {
                        if (item.groupId == group.id)
                            groupedSites.push_back(&item);
                    }
                    QCollator order(QLocale(QLocale::Russian, QLocale::Russia));
                    order.setCaseSensitivity(Qt::CaseInsensitive);
                    std::sort(groupedSites.begin(), groupedSites.end(), [&](const Site *left, const Site *right) {
                        return order.compare(left->name, right->name) < 0;
                    });
                    for (const auto *item : groupedSites) {
                        auto *card = new QPushButton(body);
                        card->setObjectName(QStringLiteral("siteCard"));
                        card->setAccessibleName(QStringLiteral("Открыть сайт %1").arg(item->name));
                        card->setCursor(Qt::PointingHandCursor);
                        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
                        auto *row = new QHBoxLayout(card);
                        row->setContentsMargins(16, 14, 16, 14);
                        row->setSpacing(12);
                        auto *icon = new QLabel(card);
                        const auto chosen = siteIcon(item->iconName);
                        icon->setPixmap((chosen.isNull() ? actionIcon("globe2") : chosen).pixmap(24, 24));
                        row->addWidget(icon);
                        auto *text = new QVBoxLayout;
                        text->setSpacing(4);
                        text->addWidget(label(item->name, card, "cardTitle"));
                        text->addWidget(label(item->url, card));
                        row->addLayout(text, 1);
                        auto *arrow = new QLabel(card);
                        arrow->setPixmap(actionIcon("box-arrow-up-right").pixmap(18, 18));
                        row->addWidget(arrow);
                        for (auto *part : card->findChildren<QLabel *>())
                            part->setAttribute(Qt::WA_TransparentForMouseEvents);
                        const auto id = item->id;
                        connect(card, &QPushButton::clicked, this, [this, id] {
                            const QSignalBlocker block(search);
                            clearText(search);
                            refresh(id);
                        });
                        list->addWidget(card);
                    }
                    if (groupedSites.empty())
                        list->addWidget(label(QStringLiteral("В этой группе пока нет сайтов."), body));
                    list->addStretch();
                    status->setText(QStringLiteral("Сайтов: %1").arg(groupedSites.size()));
                    return;
                }
            }
        }
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
        auto *copyPassword = iconButton(row, card, QStringLiteral("Копировать пароль"), "key", [this, id] {
            if (const auto *item = account(id))
                copy(item->password);
        });
        copyPassword->setEnabled(!entry.password.isEmpty());
        iconButton(row, card, QStringLiteral("Изменить аккаунт"), "pencil", [this, id] {
            if (const auto *item = account(id))
                entryDialog(item);
        });
        iconButton(row, card, QStringLiteral("Удалить аккаунт"), "trash", [this, id] { removeEntry(id); });
        layout->addLayout(row);
        if (entry.alternativeSignIn)
            layout->addWidget(label(QStringLiteral("Вход: %1").arg(entry.signInMethod), card));
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

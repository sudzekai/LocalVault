#include "MainWindow.h"
#include "Dialogs.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QHeaderView>
#include <QMessageBox>
#include <QMimeData>
#include <QScopeGuard>
#include <QStackedWidget>
#include <QTableWidget>
#include <QUrl>

namespace {
constexpr char tokenFormat[] = "application/x-localvault-copy-token";
}

MainWindow::MainWindow(const QString &path) : manager(path)
{
    setWindowTitle(QStringLiteral("LocalVault"));
    setMinimumSize(820, 520);
    resize(1020, 650);
    setStyleSheet(QStringLiteral(R"(
        QWidget { color:#e6edf7; background:#101827; font-size:14px; }
        QLabel#heading { font-size:27px; font-weight:700; }
        QLabel#secondary { color:#a8b7cc; }
        QLabel#error { color:#ffaaa4; }
        QPushButton { background:#1e2d44; border:1px solid #3c4e67; padding:7px 10px; border-radius:7px; }
        QPushButton:hover { background:#2a3e5c; }
        QPushButton:disabled { background:#141e2e; color:#77859a; border-color:#243248; }
        QPushButton#primary { background:#88b7ff; color:#101827; border-color:#88b7ff; font-weight:600; }
        QPushButton#primary:hover { background:#a7caff; }
        QLineEdit, QSpinBox, QTableWidget { background:#172237; border:1px solid #3c4e67; border-radius:7px; }
        QLineEdit, QSpinBox { padding:7px; selection-background-color:#365482; }
        QLineEdit:focus, QSpinBox:focus, QPushButton:focus { border-color:#88b7ff; }
        QHeaderView::section { background:#1e2d44; padding:9px; border:0; }
        QTableWidget { gridline-color:#29384f; selection-background-color:#365482; }
        QProgressBar { background:#172237; border:1px solid #3c4e67; text-align:center; }
        QProgressBar::chunk { background:#345e91; }
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
            if (manager.exists()) {
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
    auto *restore = button(authRow, card, QStringLiteral("Импортировать копию"), [this] { importVault(); });
    restore->setToolTip(QStringLiteral("Восстановление из зашифрованного файла"));
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
    auto *vault = new QVBoxLayout(vaultPage);
    vault->setContentsMargins(26, 22, 26, 22);
    vault->setSpacing(13);
    auto *top = new QHBoxLayout;
    vault->addLayout(top);
    top->addWidget(label(QStringLiteral("Хранилище"), vaultPage, "heading"));
    top->addStretch();
    button(top, vaultPage, QStringLiteral("Генератор"), [this] {
        GeneratorDialog dialog(this, QStringLiteral("Копировать пароль"));
        if (dialog.exec() == QDialog::Accepted && manager.unlocked()) {
            QString value = dialog.takePassword();
            copy(value);
            wipe(value);
        }
    });
    button(top, vaultPage, QStringLiteral("Настройки"), [this] { settings(); });
    button(top, vaultPage, QStringLiteral("Заблокировать"), [this] { lock(); });

    auto *searchRow = new QHBoxLayout;
    vault->addLayout(searchRow);
    search = new QLineEdit(vaultPage);
    search->setPlaceholderText(QStringLiteral("Поиск по сайту или логину"));
    search->setClearButtonEnabled(true);
    search->setMaxLength(2048);
    searchRow->addWidget(search, 1);
    button(searchRow, vaultPage, QStringLiteral("Добавить запись"),
           [this] { entryDialog(nullptr); }, true);

    table = new QTableWidget(0, 3, vaultPage);
    table->setHorizontalHeaderLabels({QStringLiteral("Сайт"), QStringLiteral("Логин"), QStringLiteral("Адрес")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    vault->addWidget(table, 1);

    auto *actions = new QHBoxLayout;
    vault->addLayout(actions);
    selectionActions = {
        button(actions, vaultPage, QStringLiteral("Просмотр"), [this] { entryDialog(selected(), true); }),
        button(actions, vaultPage, QStringLiteral("Копировать логин"), [this] {
            if (const auto *entry = selected())
                copy(entry->login);
        }),
        button(actions, vaultPage, QStringLiteral("Копировать пароль"), [this] {
            if (const auto *entry = selected())
                copy(entry->password);
        }),
        button(actions, vaultPage, QStringLiteral("Открыть сайт"), [this] {
            if (const auto *entry = selected()) {
                if (!QDesktopServices::openUrl(QUrl(entry->url, QUrl::StrictMode)))
                    showError({VaultError::Io, QStringLiteral("Не удалось открыть ссылку в браузере.")});
            }
        }),
        button(actions, vaultPage, QStringLiteral("Изменить"), [this] {
            if (const auto *entry = selected())
                entryDialog(entry);
        }),
        button(actions, vaultPage, QStringLiteral("Удалить"), [this] { removeEntry(); })
    };
    actions->addStretch();
    connect(table, &QTableWidget::itemSelectionChanged, this, [this] {
        const bool hasSelection = selected() != nullptr;
        for (auto *control : selectionActions)
            control->setEnabled(hasSelection);
    });
    connect(table, &QTableWidget::cellDoubleClicked, this, [this] { entryDialog(selected(), true); });
    connect(search, &QLineEdit::textChanged, this, [this] { refresh(); });

    auto *footer = new QHBoxLayout;
    footer->setSpacing(14);
    status = label({}, vaultPage);
    idleLabel = label({}, vaultPage);
    status->setWordWrap(false);
    idleLabel->setWordWrap(false);
    footer->addWidget(status);
    footer->addWidget(idleLabel);
    footer->addStretch();
    button(footer, vaultPage, QStringLiteral("Импорт"), [this] { importVault(); });
    button(footer, vaultPage, QStringLiteral("Экспорт"), [this] { exportVault(); });
    vault->addLayout(footer);
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
    const bool existing = manager.exists();
    authTitle->setText(existing ? QStringLiteral("Хранилище заблокировано") : QStringLiteral("Создайте хранилище"));
    authHint->setText(existing ? QStringLiteral("Введите мастер-пароль, чтобы открыть записи.")
                               : QStringLiteral("Установите мастер-пароль длиной не менее 12 символов."));
    authButton->setText(existing ? QStringLiteral("Разблокировать") : QStringLiteral("Создать"));
    repeat->setVisible(!existing);
    authError->clear();
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
    // Drop the table's shared strings before wiping the model.
    table->setRowCount(0);
    manager.lock();
    clearClipboard();
    for (auto *dialog : findChildren<QDialog *>()) {
        if (dialog->isVisible())
            dialog->reject();
    }
    clearText(search);
    refresh();
    showLocked();
}

void MainWindow::refresh(const QUuid &id)
{
    QString query = search->text().trimmed();
    const auto cleanup = qScopeGuard([&] { wipe(query); });
    table->setRowCount(0);
    int selectedRow = -1;
    for (const auto &entry : manager.data().entries) {
        if (!entry.site.contains(query, Qt::CaseInsensitive) && !entry.login.contains(query, Qt::CaseInsensitive))
            continue;
        const int row = table->rowCount();
        table->insertRow(row);
        auto *site = new QTableWidgetItem(entry.site);
        site->setData(Qt::UserRole, entry.id);
        table->setItem(row, 0, site);
        table->setItem(row, 1, new QTableWidgetItem(entry.login));
        table->setItem(row, 2, new QTableWidgetItem(entry.url));
        if (entry.id == id)
            selectedRow = row;
    }
    if (selectedRow >= 0)
        table->selectRow(selectedRow);
    for (auto *control : selectionActions)
        control->setEnabled(selectedRow >= 0);
    status->setText(QStringLiteral("Показано %1 из %2").arg(table->rowCount()).arg(manager.data().entries.size()));
}

const Entry *MainWindow::selected() const
{
    if (table->selectedItems().isEmpty())
        return nullptr;
    const auto *site = table->item(table->currentRow(), 0);
    if (!site)
        return nullptr;
    const auto id = site->data(Qt::UserRole).toUuid();
    for (const auto &entry : manager.data().entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
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
        status->setText(QStringLiteral("Показано %1 из %2").arg(table->rowCount()).arg(manager.data().entries.size()));
}

void MainWindow::showError(const Failure &error)
{
    QMessageBox dialog(QMessageBox::Warning, QStringLiteral("LocalVault"), error.message, QMessageBox::Ok, this);
    dialog.setTextFormat(Qt::PlainText);
    dialog.exec();
}

#include "MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

QPushButton *findButton(QWidget &owner, const QString &text)
{
    for (auto *button : owner.findChildren<QPushButton *>()) {
        if (button->isVisible() && button->text() == text)
            return button;
    }
    return nullptr;
}

void click(QWidget &owner, const QString &text)
{
    auto *button = findButton(owner, text);
    check(button && button->isEnabled(), "button unavailable");
    button->click();
}

QLineEdit *field(QWidget &owner, const QString &placeholder)
{
    for (auto *edit : owner.findChildren<QLineEdit *>()) {
        if (edit->isVisible() && edit->placeholderText() == placeholder)
            return edit;
    }
    throw std::runtime_error("field unavailable");
}

QLineEdit *formField(QFormLayout &form, int row)
{
    return qobject_cast<QLineEdit *>(form.itemAt(row, QFormLayout::FieldRole)->widget());
}

void checkUi(const QString &path)
{
    const auto password = QStringLiteral("master password for widgets");
    MainWindow window(path);
    window.show();
    QApplication::processEvents();

    auto *master = field(window, QStringLiteral("Мастер-пароль"));
    master->setText(password);
    field(window, QStringLiteral("Повторите мастер-пароль"))->setText(password);
    click(window, QStringLiteral("Создать"));
    auto *table = window.findChild<QTableWidget *>();
    check(table && table->isVisible() && table->rowCount() == 0, "setup did not open the empty vault");

    bool saved = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        auto *form = dialog->findChild<QFormLayout *>();
        auto *save = findButton(*dialog, QStringLiteral("Сохранить"));
        if (!form || !save || formField(*form, 3)->echoMode() != QLineEdit::Password) {
            dialog->reject();
            return;
        }
        formField(*form, 0)->setText(QStringLiteral("Example"));
        formField(*form, 1)->setText(QStringLiteral("https://example.com"));
        formField(*form, 2)->setText(QStringLiteral("widget-login"));
        formField(*form, 3)->setText(QStringLiteral("Synthetic-password-123"));
        save->click();
        saved = dialog->result() == QDialog::Accepted;
        if (!saved)
            dialog->reject();
    });
    click(window, QStringLiteral("Добавить запись"));
    check(saved && table->rowCount() == 1, "entry form did not save");

    const auto screenshot = qEnvironmentVariable("LOCALVAULT_TEST_SCREENSHOT");
    if (!screenshot.isEmpty())
        check(window.grab().save(screenshot), "could not save UI screenshot");

    click(window, QStringLiteral("Копировать пароль"));
    check(QApplication::clipboard()->text() == QStringLiteral("Synthetic-password-123"), "password copy");
    auto *search = field(window, QStringLiteral("Поиск по сайту или логину"));
    search->setText(QStringLiteral("missing"));
    check(table->rowCount() == 0, "search failed to filter");
    search->setText(QStringLiteral("WIDGET-LOGIN"));
    check(table->rowCount() == 1, "search did not match login");

    click(window, QStringLiteral("Заблокировать"));
    check(!table->isVisible() && table->rowCount() == 0, "lock did not clear the table");
    check(QApplication::clipboard()->text().isEmpty(), "lock did not clear the clipboard");
    master->setText(QStringLiteral("wrong"));
    click(window, QStringLiteral("Разблокировать"));
    check(!table->isVisible(), "wrong password opened the UI");
    master->setText(password);
    click(window, QStringLiteral("Разблокировать"));
    check(table->isVisible() && table->rowCount() == 1 && search->text().isEmpty(), "reopen failed");
}

void checkIdle(const QString &path)
{
    const auto password = QStringLiteral("idle test master password");
    {
        PasswordManager manager(path);
        manager.create(password);
        manager.setIdleMinutes(1);
    }
    MainWindow window(path);
    window.show();
    QApplication::processEvents();
    field(window, QStringLiteral("Мастер-пароль"))->setText(password);
    click(window, QStringLiteral("Разблокировать"));
    auto *table = window.findChild<QTableWidget *>();
    check(table && table->isVisible(), "idle test could not unlock");
    QEventLoop wait;
    QTimer::singleShot(63000, &wait, &QEventLoop::quit);
    QTimer::singleShot(0, &window, [&] {
        if (auto *generator = findButton(window, QStringLiteral("Генератор")))
            generator->click();
    });
    wait.exec();
    check(!table->isVisible() && QApplication::activeModalWidget() == nullptr,
          "idle timeout did not lock and close the generator");
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir directory;
    try {
        check(directory.isValid(), "temporary directory");
        CryptoManager::initialize();
        if (app.arguments().contains(QStringLiteral("--idle")))
            checkIdle(directory.filePath(QStringLiteral("idle.lvault")));
        else
            checkUi(directory.filePath(QStringLiteral("widgets.lvault")));
        std::cout << "Widget checks passed\n";
    } catch (const Failure &failure) {
        std::cerr << failure.message.toStdString() << '\n';
        return 1;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}

#include "MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

QPushButton *findButton(QWidget &owner, const QString &title)
{
    for (auto *button : owner.findChildren<QPushButton *>()) {
        if (button->isVisible() && button->accessibleName() == title)
            return button;
    }
    return nullptr;
}

void click(QWidget &owner, const QString &title)
{
    auto *control = findButton(owner, title);
    check(control && control->isEnabled() && !control->icon().isNull(), "SVG action unavailable");
    control->click();
}

QPushButton *textButton(QWidget &owner, const QString &title)
{
    for (auto *button : owner.findChildren<QPushButton *>()) {
        if (button->isVisible() && button->text() == title && button->icon().isNull())
            return button;
    }
    throw std::runtime_error("text button unavailable: " + title.toStdString());
}

void checkFooter(QDialog &dialog)
{
    auto *save = textButton(dialog, QStringLiteral("Сохранить"));
    auto *cancel = textButton(dialog, QStringLiteral("Отмена"));
    check(save->mapToGlobal(QPoint{}).y() == cancel->mapToGlobal(QPoint{}).y(), "save and cancel are not on the same row");
}

QLineEdit *field(QWidget &owner, const QString &placeholder)
{
    for (auto *edit : owner.findChildren<QLineEdit *>()) {
        if (edit->isVisible() && edit->placeholderText() == placeholder)
            return edit;
    }
    throw std::runtime_error("field unavailable: " + placeholder.toStdString());
}

QLineEdit *formField(QWidget &owner, const QString &name)
{
    for (auto *form : owner.findChildren<QFormLayout *>()) {
        for (int row = 0; row < form->rowCount(); ++row) {
            const auto *item = form->itemAt(row, QFormLayout::LabelRole);
            const auto *label = item ? qobject_cast<QLabel *>(item->widget()) : nullptr;
            if (label && label->text() == name) {
                auto *field = form->itemAt(row, QFormLayout::FieldRole);
                if (field && field->layout())
                    field = field->layout()->itemAt(0);
                if (auto *edit = field ? qobject_cast<QLineEdit *>(field->widget()) : nullptr) {
                    check(label->mapToGlobal(QPoint{}).y() < edit->mapToGlobal(QPoint{}).y(), "caption is not above its field");
                    return edit;
                }
            }
        }
    }
    throw std::runtime_error("form field unavailable");
}

template<class Function>
void interact(MainWindow &window, const QString &title, Function operation)
{
    std::exception_ptr error;
    bool observed = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        observed = true;
        try {
            operation(*dialog);
        } catch (...) {
            error = std::current_exception();
            dialog->reject();
        }
    });
    click(window, title);
    if (error)
        std::rethrow_exception(error);
    check(observed, "expected a modal form");
    QApplication::processEvents();
}

void addSite(MainWindow &window, const QString &name, const QString &url)
{
    interact(window, QStringLiteral("Добавить сайт"), [&](QDialog &dialog) {
        checkFooter(dialog);
        auto *nameField = formField(dialog, QStringLiteral("Название"));
        auto *urlField = formField(dialog, QStringLiteral("Адрес"));
        check(!nameField->placeholderText().isEmpty() && !urlField->placeholderText().isEmpty(), "site placeholders missing");
        const auto preview = qEnvironmentVariable("LOCALVAULT_SITE_PREVIEW");
        if (!preview.isEmpty())
            check(dialog.grab().save(preview), "could not save site screenshot");
        nameField->setText(name);
        urlField->setText(url);
        textButton(dialog, QStringLiteral("Сохранить"))->click();
        check(dialog.result() == QDialog::Accepted, "site form could not save");
    });
}

void addAccount(MainWindow &window, const QString &login, bool optionalFields)
{
    interact(window, QStringLiteral("Добавить аккаунт"), [&](QDialog &dialog) {
        checkFooter(dialog);
        auto *eye = findButton(dialog, QStringLiteral("Показать пароль"));
        auto *plus = findButton(dialog, QStringLiteral("Добавить электронную почту"));
        auto *password = formField(dialog, QStringLiteral("Пароль"));
        check(eye && eye->mapToGlobal(eye->rect().center()).x() > password->mapToGlobal(password->rect().topRight()).x()
              && qAbs(eye->mapToGlobal(eye->rect().center()).y()
                      - password->mapToGlobal(password->rect().center()).y()) <= 2, "eye is not beside the field");
        const auto headings = dialog.findChildren<QLabel *>();
        const auto caption = std::find_if(headings.begin(), headings.end(), [](const QLabel *label) {
            return label->text() == QStringLiteral("Электронные почты");
        });
        check(plus && plus->objectName() != QStringLiteral("primary") && caption != headings.end()
              && qAbs(plus->mapToGlobal(plus->rect().center()).y()
                      - (*caption)->mapToGlobal((*caption)->rect().center()).y()) <= 2, "plus is not in the email header");
        const auto preview = qEnvironmentVariable("LOCALVAULT_ACCOUNT_PREVIEW");
        if (!preview.isEmpty())
            check(dialog.grab().save(preview), "could not save account screenshot");
        formField(dialog, QStringLiteral("Логин"))->setText(login);
        check(!password->placeholderText().isEmpty(), "password placeholder missing");
        check(password->echoMode() == QLineEdit::Password, "password is visible by default");
        password->setText(QStringLiteral("Synthetic-password-123"));
        if (optionalFields) {
            formField(dialog, QStringLiteral("Имя пользователя"))->setText(QStringLiteral("Тестовый аккаунт"));
            formField(dialog, QStringLiteral("Телефон"))->setText(QStringLiteral("+1 202 555 0110"));
            click(dialog, QStringLiteral("Добавить электронную почту"));
            QApplication::processEvents();
            field(dialog, QStringLiteral("name@example.com"))->setText(QStringLiteral("first@example.com"));
            field(dialog, QStringLiteral("Например, рабочая"))->setText(QStringLiteral("Личная"));
            click(dialog, QStringLiteral("Добавить электронную почту"));
            for (auto *edit : dialog.findChildren<QLineEdit *>()) {
                if (edit->accessibleName() == QStringLiteral("Адрес электронной почты") && edit->text().isEmpty())
                    edit->setText(QStringLiteral("second@example.com"));
            }
        }
        textButton(dialog, QStringLiteral("Сохранить"))->click();
        check(dialog.result() == QDialog::Accepted, "account form could not save");
    });
}

int countCards(MainWindow &window)
{
    int count = 0;
    for (auto *card : window.findChildren<QWidget *>(QStringLiteral("accountCard"))) {
        if (card->isVisible())
            ++count;
    }
    return count;
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
    textButton(window, QStringLiteral("Импортировать копию"));
    textButton(window, QStringLiteral("Создать"))->click();
    auto *sites = window.findChild<QListWidget *>();
    check(sites && sites->isVisible() && sites->count() == 0, "setup did not open the empty vault");
    interact(window, QStringLiteral("Настройки"), [](QDialog &dialog) {
        checkFooter(dialog);
        textButton(dialog, QStringLiteral("Изменить мастер-пароль"));
        textButton(dialog, QStringLiteral("Отмена"))->click();
    });

    addSite(window, QStringLiteral("Zebra"), QStringLiteral("https://zebra.example"));
    addAccount(window, QStringLiteral("zebra-login"), false);
    addSite(window, QStringLiteral("Alpha"), QStringLiteral("https://alpha.example"));
    addAccount(window, QStringLiteral("alpha-first"), true);
    addAccount(window, QStringLiteral("alpha-second"), false);
    check(sites->count() == 2 && sites->item(0)->text() == QStringLiteral("Alpha")
          && sites->item(1)->text() == QStringLiteral("Zebra"), "sites not unique or sorted");
    check(countCards(window) == 2, "selected site did not show both cards");
    const auto screenshot = qEnvironmentVariable("LOCALVAULT_TEST_SCREENSHOT");
    if (!screenshot.isEmpty())
        check(window.grab().save(screenshot), "could not save UI screenshot");

    click(window, QStringLiteral("Копировать пароль"));
    check(QApplication::clipboard()->text() == QStringLiteral("Synthetic-password-123"), "password copy");
    interact(window, QStringLiteral("Просмотр аккаунта"), [&](QDialog &dialog) {
        check(!formField(dialog, QStringLiteral("Локальный ID"))->text().isEmpty(), "missing local ID");
        check(formField(dialog, QStringLiteral("Пароль"))->echoMode() == QLineEdit::Password, "password not masked");
        click(dialog, QStringLiteral("Показать пароль"));
        check(formField(dialog, QStringLiteral("Пароль"))->echoMode() == QLineEdit::Normal, "reveal failed");
        click(dialog, QStringLiteral("Закрыть"));
    });

    sites->setCurrentRow(1);
    QApplication::processEvents();
    check(countCards(window) == 1, "switching sites did not change account cards");
    auto *search = field(window, QStringLiteral("Поиск по названию"));
    search->setText(QStringLiteral("alpha-first"));
    check(sites->count() == 0 && countCards(window) == 0, "search matched account instead of site");
    search->setText(QStringLiteral("ALPH"));
    QApplication::processEvents();
    check(sites->count() == 1 && countCards(window) == 2, "search did not match site name");
    click(window, QStringLiteral("Заблокировать"));
    check(!sites->isVisible() && sites->count() == 0 && countCards(window) == 0, "lock did not clear the UI");
    check(QApplication::clipboard()->text().isEmpty(), "lock did not clear the clipboard");
    master->setText(QStringLiteral("wrong"));
    textButton(window, QStringLiteral("Разблокировать"))->click();
    check(!sites->isVisible(), "wrong password opened the UI");
    master->setText(password);
    textButton(window, QStringLiteral("Разблокировать"))->click();
    check(sites->isVisible() && sites->count() == 2 && search->text().isEmpty(), "reopen failed");

    click(window, QStringLiteral("Заблокировать"));
    PasswordManager verification(path);
    verification.open(password);
    bool found = false;
    for (const auto &entry : verification.data().entries) {
        if (entry.login != QStringLiteral("alpha-first"))
            continue;
        found = true;
        check(entry.createdAt.isValid() && entry.emails.size() == 2
            && entry.emails[0].description == QStringLiteral("Личная")
            && entry.emails[1].description.isEmpty(), "account options not saved");
    }
    check(found, "account missing after reopen");
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
    textButton(window, QStringLiteral("Разблокировать"))->click();
    auto *sites = window.findChild<QListWidget *>();
    check(sites && sites->isVisible(), "idle test could not unlock");
    QEventLoop wait;
    QTimer::singleShot(63000, &wait, &QEventLoop::quit);
    QTimer::singleShot(0, &window, [&] {
        if (auto *generator = findButton(window, QStringLiteral("Генератор")))
            generator->click();
    });
    wait.exec();
    check(!sites->isVisible() && QApplication::activeModalWidget() == nullptr, "idle lock failed");
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
        std::cout << "Widget checks passed.\n";
    } catch (const Failure &failure) {
        std::cerr << failure.message.toStdString() << '\n';
        return 1;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}

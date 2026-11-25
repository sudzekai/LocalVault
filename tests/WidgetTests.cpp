#include "MainWindow.h"
#include "IconPicker.h"
#include "TokenStore.h"

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QMouseEvent>
#include <QListView>
#include <QMenu>
#include <QPlainTextEdit>
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
    const auto fillForm = [&] {
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
    };
    QTimer::singleShot(0, &window, [&] {
        if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            if (title == QStringLiteral("Создать группу")) {
                bool hasSiteAction = false;
                for (auto *entry : menu->actions())
                    hasSiteAction |= entry->text() == QStringLiteral("Добавить сайт");
                if (!hasSiteAction) {
                    error = std::make_exception_ptr(std::runtime_error("empty-area menu is missing site creation"));
                    menu->close();
                    return;
                }
            }
            for (auto *action : menu->actions()) {
                if (action->text() == title) {
                    QTimer::singleShot(0, &window, fillForm);
                    menu->close();
                    action->trigger();
                    return;
                }
            }
            menu->close();
        } else {
            fillForm();
        }
    });
    if (findButton(window, title))
        click(window, title);
    else if (title == QStringLiteral("Добавить сайт")) {
        auto *tree = window.findChild<QTreeWidget *>();
        check(tree, "site navigation missing");
        auto *item = tree->currentItem();
        if (item && item->parent())
            item = item->parent();
        if (item && !item->data(0, Qt::UserRole + 1).toBool())
            item = nullptr;
        const auto point = item ? tree->visualItemRect(item).center()
            : QPoint(70, tree->viewport()->height() - 5);
        tree->customContextMenuRequested(point);
    } else if (title == QStringLiteral("Создать группу") || title == QStringLiteral("Изменить группу")) {
        auto *tree = window.findChild<QTreeWidget *>();
        const auto point = title == QStringLiteral("Изменить группу")
            ? tree->visualItemRect(tree->currentItem()).center() : QPoint(70, tree->viewport()->height() - 5);
        tree->customContextMenuRequested(point);
    } else {
        textButton(window, title)->click();
    }
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
    auto *sites = window.findChild<QTreeWidget *>();
    check(sites && sites->isVisible() && sites->topLevelItemCount() == 0, "setup did not open the empty vault");
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
    check(sites->topLevelItemCount() == 2 && sites->topLevelItem(0)->text(0) == QStringLiteral("Alpha")
          && sites->topLevelItem(1)->text(0) == QStringLiteral("Zebra"), "sites not unique or sorted");
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

    sites->setCurrentItem(sites->topLevelItem(1));
    QApplication::processEvents();
    check(countCards(window) == 1, "switching sites did not change account cards");
    auto *search = field(window, QStringLiteral("Поиск по названию"));
    search->setText(QStringLiteral("alpha-first"));
    check(sites->topLevelItemCount() == 0 && countCards(window) == 0, "search matched account instead of site");
    search->setText(QStringLiteral("ALPH"));
    QApplication::processEvents();
    check(sites->topLevelItemCount() == 1 && countCards(window) == 2, "search did not match site name");
    click(window, QStringLiteral("Заблокировать"));
    check(!sites->isVisible() && sites->topLevelItemCount() == 0 && countCards(window) == 0, "lock did not clear the UI");
    check(QApplication::clipboard()->text().isEmpty(), "lock did not clear the clipboard");
    master->setText(QStringLiteral("wrong"));
    textButton(window, QStringLiteral("Разблокировать"))->click();
    check(!sites->isVisible(), "wrong password opened the UI");
    master->setText(password);
    textButton(window, QStringLiteral("Разблокировать"))->click();
    check(sites->isVisible() && sites->topLevelItemCount() == 2 && search->text().isEmpty(), "reopen failed");

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
    auto *sites = window.findChild<QTreeWidget *>();
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

void checkSavedPath(const QString &defaultPath, const QString &externalPath)
{
    const auto masterPassword = QStringLiteral("test password for selected path");
    {
        PasswordManager external(externalPath);
        external.create(masterPassword);
    }
    {
        MainWindow window(defaultPath);
        window.show();
        QApplication::processEvents();
        interact(window, QStringLiteral("Настройки"), [&](QDialog &dialog) {
            auto *path = field(dialog, QStringLiteral("Путь к .lvault или https://сервер/vault"));
            path->setText(externalPath + QStringLiteral(".missing.lvault"));
            textButton(dialog, QStringLiteral("Сохранить"))->click();
            check(dialog.isVisible(), "invalid selection closed settings");
            path->setText(externalPath);
            textButton(dialog, QStringLiteral("Сохранить"))->click();
            check(dialog.result() == QDialog::Accepted, "valid location was not saved");
        });
        textButton(window, QStringLiteral("Разблокировать"));
    }
    {
        MainWindow window(defaultPath);
        window.show();
        QApplication::processEvents();
        field(window, QStringLiteral("Мастер-пароль"))->setText(masterPassword);
        textButton(window, QStringLiteral("Разблокировать"))->click();
        check(window.findChild<QTreeWidget *>()->isVisible(), "path was not loaded on restart");
        check(!QFile::exists(defaultPath), "external unlock created the default file");
    }
    check(QFile::rename(externalPath, externalPath + QStringLiteral(".offline")), "external test rename failed");
    {
        MainWindow window(defaultPath);
        window.show();
        QApplication::processEvents();
        textButton(window, QStringLiteral("Разблокировать"))->click();
        auto *error = window.findChild<QLabel *>(QStringLiteral("error"));
        check(error && error->isVisible() && error->text().contains(QStringLiteral("недоступен")),
              "missing selected file did not show an error");
        check(!QFile::exists(defaultPath) && !QFile::exists(externalPath), "missing path created a file");
        interact(window, QStringLiteral("Настройки"), [&](QDialog &dialog) {
            auto *path = field(dialog, QStringLiteral("Путь к .lvault или https://сервер/vault"));
            check(QDir::fromNativeSeparators(path->text()) == externalPath, "missing path was forgotten");
            textButton(dialog, QStringLiteral("Сбросить путь"))->click();
            check(dialog.result() == QDialog::Accepted, "reset didn't close settings");
        });
        textButton(window, QStringLiteral("Создать"));
    }
    MainWindow window(defaultPath);
    window.show();
    QApplication::processEvents();
    textButton(window, QStringLiteral("Создать"));
}

void checkNewFeatures(const QString &path)
{
    const auto password = QStringLiteral("groups widget master password");
    MainWindow window(path);
    window.show();
    QApplication::processEvents();
    field(window, QStringLiteral("Мастер-пароль"))->setText(password);
    field(window, QStringLiteral("Повторите мастер-пароль"))->setText(password);
    textButton(window, QStringLiteral("Создать"))->click();
    interact(window, QStringLiteral("Создать группу"), [&](QDialog &dialog) {
        formField(dialog, QStringLiteral("Название группы"))->setText(QStringLiteral("СОЦСЕТИ"));
        textButton(dialog, QStringLiteral("Сохранить"))->click();
    });
    auto *sites = window.findChild<QTreeWidget *>();
    check(sites && sites->topLevelItemCount() == 1
          && sites->topLevelItem(0)->text(0) == QStringLiteral("СОЦСЕТИ"), "group missing from site tree");
    addSite(window, QStringLiteral("Telegram"), QStringLiteral("https://telegram.org"));
    auto *group = sites->topLevelItem(0);
    check(group->text(0) == QStringLiteral("СОЦСЕТИ") && group->childCount() == 1
          && group->child(0)->text(0) == QStringLiteral("Telegram"), "site not created under current group");
    interact(window, QStringLiteral("Добавить аккаунт"), [&](QDialog &dialog) {
        formField(dialog, QStringLiteral("Логин"))->setText(QStringLiteral("oauth-account"));
        auto *passwordField = formField(dialog, QStringLiteral("Пароль"));
        check(passwordField->text().isEmpty(), "new password must be empty");
        for (auto *box : dialog.findChildren<QCheckBox *>()) {
            if (box->text() == QStringLiteral("Альтернативный способ входа"))
                box->setChecked(true);
        }
        auto *method = formField(dialog, QStringLiteral("Способ входа"));
        check(method->isEnabled(), "alternative method is not enabled");
        method->setText(QStringLiteral("OAuth через Google"));
        auto *codes = dialog.findChild<QPlainTextEdit *>();
        check(codes && codes->isHidden(), "recovery codes must initially be hidden");
        click(dialog, QStringLiteral("Показать коды восстановления"));
        check(codes->isVisible(), "recovery editor did not open");
        codes->setPlainText(QStringLiteral("TEST-CODE-ONE\nTEST-CODE-TWO"));
        textButton(dialog, QStringLiteral("Сохранить"))->click();
        check(dialog.result() == QDialog::Accepted, "passwordless account not saved");
    });
    auto *copyPassword = findButton(window, QStringLiteral("Копировать пароль"));
    check(copyPassword && !copyPassword->isEnabled(), "empty password copy is enabled");
    const auto treePreview = qEnvironmentVariable("LOCALVAULT_TREE_PREVIEW");
    if (!treePreview.isEmpty())
        check(window.grab().save(treePreview), "could not save group tree preview");
    interact(window, QStringLiteral("Просмотр аккаунта"), [&](QDialog &dialog) {
        auto *codes = dialog.findChild<QPlainTextEdit *>();
        check(codes && codes->isHidden() && codes->isReadOnly(), "saved codes are not masked/read-only");
        click(dialog, QStringLiteral("Показать коды восстановления"));
        check(codes->toPlainText() == QStringLiteral("TEST-CODE-ONE\nTEST-CODE-TWO"), "saved codes missing");
        click(dialog, QStringLiteral("Копировать коды восстановления"));
        check(QApplication::clipboard()->text() == codes->toPlainText(), "codes copy failed");
        click(dialog, QStringLiteral("Закрыть"));
    });
    group = sites->topLevelItem(0);
    check(group->isExpanded(), "new site's group must be expanded");
    const auto groupRect = sites->visualItemRect(group);
    const auto point = QPoint(groupRect.left() - sites->indentation() / 2, groupRect.center().y());
    const auto toggle = [&] {
        QMouseEvent clickEvent(QEvent::MouseButtonPress, QPointF(point),
            QPointF(sites->viewport()->mapToGlobal(point)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(sites->viewport(), &clickEvent);
        QApplication::processEvents();
    };
    toggle();
    check(!group->isExpanded() && countCards(window) == 0, "group row click did not collapse");
    auto *siteCard = window.findChild<QPushButton *>(QStringLiteral("siteCard"));
    check(siteCard && siteCard->isVisible()
          && siteCard->accessibleName() == QStringLiteral("Открыть сайт Telegram"), "group site card missing");
    const auto groupPreview = qEnvironmentVariable("LOCALVAULT_GROUP_PREVIEW");
    if (!groupPreview.isEmpty())
        check(window.grab().save(groupPreview), "could not save group card preview");
    siteCard->click();
    QApplication::processEvents();
    group = sites->topLevelItem(0);
    check(group->isExpanded() && sites->currentItem() == group->child(0)
          && countCards(window) == 1, "site card did not navigate to account cards");
    const auto nextRect = sites->visualItemRect(group);
    const auto nextPoint = QPoint(nextRect.left() - sites->indentation() / 2, nextRect.center().y());
    QMouseEvent groupClick(QEvent::MouseButtonPress, QPointF(nextPoint),
        QPointF(sites->viewport()->mapToGlobal(nextPoint)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(sites->viewport(), &groupClick);
    QApplication::processEvents();
    toggle();
    check(group->isExpanded(), "group row click did not expand");
    sites->setCurrentItem(group->child(0));
    QApplication::processEvents();
    check(countCards(window) == 1, "nested site did not open its accounts");
    auto *search = field(window, QStringLiteral("Поиск по названию"));
    search->setText(QStringLiteral("Tele"));
    group = sites->topLevelItem(0);
    check(group && group->childCount() == 1 && group->isExpanded(), "site search didn't expand matching group");
    search->clear();
    group = sites->topLevelItem(0);
    sites->setCurrentItem(group);
    interact(window, QStringLiteral("Изменить группу"), [&](QDialog &dialog) {
        check(formField(dialog, QStringLiteral("Название группы"))->text() == QStringLiteral("СОЦСЕТИ"),
              "context menu edits wrong group");
        textButton(dialog, QStringLiteral("Отмена"))->click();
    });
    sites->setCurrentItem(nullptr);
    addSite(window, QStringLiteral("Standalone"), QStringLiteral("https://standalone.example"));
    check(sites->topLevelItemCount() == 2 && !sites->currentItem()->parent(),
          "empty-area context menu did not create an ungrouped site");
    const auto assertNoDialogOnClick = [&](QWidget *target, const QPoint &point) {
        bool unexpected = false;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &window, [&] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                unexpected = true;
                dialog->reject();
            }
        });
        watchdog.start(0);
        QMouseEvent event(QEvent::MouseButtonPress, QPointF(point),
            QPointF(target->mapToGlobal(point)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
        watchdog.stop();
        check(!unexpected && !QApplication::activeModalWidget(), "left click opened a creation dialog");
    };
    const auto blank = QPoint(70, sites->viewport()->height() - 5);
    assertNoDialogOnClick(sites->viewport(), blank);
    auto *sidebar = window.findChild<QWidget *>(QStringLiteral("sidebar"));
    assertNoDialogOnClick(sidebar, QPoint(3, sidebar->height() - 3));
    QTimer::singleShot(0, &window, [&] {
        if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            const auto preview = qEnvironmentVariable("LOCALVAULT_MENU_PREVIEW");
            if (!preview.isEmpty())
                menu->grab().save(preview);
            menu->close();
        }
    });
    sites->customContextMenuRequested(blank);
    assertNoDialogOnClick(sites->viewport(), blank);
    click(window, QStringLiteral("Заблокировать"));
}

void checkIconPicker()
{
    const auto names = availableSiteIcons();
    check(names.size() >= 2000 && names.contains(QStringLiteral("telegram"))
          && names.contains(QStringLiteral("discord")), "full Bootstrap catalogue missing");
    IconPicker picker(nullptr);
    picker.show();
    QApplication::processEvents();
    bool chosen = false;
    QTimer::singleShot(0, &picker, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu)
            return;
        auto *search = menu->findChild<QLineEdit *>();
        auto *view = menu->findChild<QListView *>();
        if (!search || !view) { menu->close(); return; }
        search->setText(QStringLiteral("telegram"));
        if (view->model()->rowCount() == 1) {
            const auto index = view->model()->index(0, 0);
            chosen = index.data().toString() == QStringLiteral("telegram")
                && !qvariant_cast<QIcon>(index.data(Qt::DecorationRole)).isNull();
            view->clicked(index);
        } else {
            menu->close();
        }
    });
    picker.click();
    check(chosen && picker.selectedName() == QStringLiteral("telegram"), "icon search or selection failed");
}

void checkRemoteSourceUi(const QString &defaultPath)
{
    const auto url = qEnvironmentVariable("APPVAULT_TEST_URL");
    const auto token = qgetenv("APPVAULT_TEST_TOKEN");
    const auto password = QStringLiteral("synthetic migration master");
    {
        MainWindow window(defaultPath);
        window.show();
        QApplication::processEvents();
        interact(window, QStringLiteral("Настройки"), [&](QDialog &dialog) {
            field(dialog, QStringLiteral("Путь к .lvault или https://сервер/vault"))->setText(url);
            auto *secret = field(dialog, QStringLiteral("API-токен сервера"));
            check(secret->echoMode() == QLineEdit::Password, "API token is not masked");
            secret->setText(QString::fromUtf8(token));
            textButton(dialog, QStringLiteral("Сохранить"))->click();
            check(dialog.result() == QDialog::Accepted, "remote source settings were not accepted");
        });
        field(window, QStringLiteral("Мастер-пароль"))->setText(password);
        textButton(window, QStringLiteral("Разблокировать"))->click();
        check(window.findChild<QTreeWidget *>()->isVisible(), "remote UI did not unlock");
        addSite(window, QStringLiteral("UI remote fixture"), QStringLiteral("https://ui-remote.example"));
        const auto bytes = StorageManager(url, token).read();
        const auto session = AuthManager::unlock(bytes, password);
        const auto data = CryptoManager::decrypt(session, bytes);
        check(data.sites.size() == 2, "UI save was not immediately posted");
    }
    const auto settingsPath = QFileInfo(defaultPath).absoluteDir().filePath(QStringLiteral("vault-location.ini"));
    QFile settingsFile(settingsPath);
    check(settingsFile.open(QIODevice::ReadOnly), "source settings missing");
    const auto settings = settingsFile.readAll();
    settingsFile.close();
    check(!settings.contains(token), "API token was written to settings in plaintext");
    MainWindow restarted(defaultPath);
    restarted.show();
    QApplication::processEvents();
    check(field(restarted, QStringLiteral("API-токен сервера"))->text() == QString::fromUtf8(token),
          "API token was not restored after restart");
    field(restarted, QStringLiteral("Мастер-пароль"))->setText(password);
    textButton(restarted, QStringLiteral("Разблокировать"))->click();
    check(restarted.findChild<QTreeWidget *>()->isVisible(), "restored token did not unlock remote vault");
    check(!QFile::exists(defaultPath), "URL source created a local vault");
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
#ifdef Q_OS_WIN
        const QByteArray sampleToken("synthetic-api-token-never-used-on-server");
        const auto protectedToken = TokenStore::protect(sampleToken, QStringLiteral("test-source"));
        check(!protectedToken.contains(sampleToken), "protected token contains plaintext");
        check(TokenStore::restore(protectedToken, QStringLiteral("test-source")) == sampleToken, "token roundtrip");
        bool rejected = false;
        try { TokenStore::restore(protectedToken, QStringLiteral("different-source")); }
        catch (const Failure &) { rejected = true; }
        check(rejected, "token from another source was accepted");
#endif
        if (app.arguments().contains(QStringLiteral("--idle")))
            checkIdle(directory.filePath(QStringLiteral("idle.lvault")));
        else {
            checkUi(directory.filePath(QStringLiteral("widgets.lvault")));
            checkSavedPath(directory.filePath(QStringLiteral("default.lvault")),
                           directory.filePath(QStringLiteral("external.lvault")));
            checkNewFeatures(directory.filePath(QStringLiteral("groups.lvault")));
            checkIconPicker();
            if (!qEnvironmentVariable("APPVAULT_TEST_URL").isEmpty())
                checkRemoteSourceUi(directory.filePath(QStringLiteral("remote-unused.lvault")));
        }
        std::cout << "Widget checks passed.\n";
    } catch (const Failure &failure) {
        std::cerr << failure.message.toStdString() << '\n';
        return 1;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}

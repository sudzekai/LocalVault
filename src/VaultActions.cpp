#include "MainWindow.h"
#include "Dialogs.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QMessageBox>
#include <QScopeGuard>
#include <QSpinBox>

namespace {
void clearFields(std::initializer_list<QLineEdit *> fields)
{
    for (auto *field : fields)
        clearText(field);
}
}

void MainWindow::entryDialog(const Entry *entry, bool readOnly)
{
    if (readOnly && !entry)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(readOnly ? QStringLiteral("Данные аккаунта")
        : entry ? QStringLiteral("Изменение записи") : QStringLiteral("Новая запись"));
    dialog.setMinimumWidth(480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    const auto add = [&](const QString &name, QLineEdit *field) {
        field->setMaxLength(2048);
        field->setReadOnly(readOnly);
        form->addRow(name, field);
        return field;
    };
    if (readOnly) {
        auto *idField = add(QStringLiteral("ID"), new QLineEdit(&dialog));
        idField->setText(entry->id.toString(QUuid::WithoutBraces));
    }
    auto *site = add(QStringLiteral("Сайт"), new QLineEdit(&dialog));
    auto *url = add(QStringLiteral("Адрес"), new QLineEdit(&dialog));
    auto *login = add(QStringLiteral("Логин"), new QLineEdit(&dialog));
    auto *password = add(QStringLiteral("Пароль"), secretField(&dialog));
    const auto cleanup = qScopeGuard([&] { clearFields({site, url, login, password}); });
    url->setPlaceholderText(QStringLiteral("https://example.com"));
    const QUuid id = entry ? entry->id : QUuid::createUuid();
    if (entry) {
        site->setText(entry->site);
        url->setText(entry->url);
        login->setText(entry->login);
        password->setText(entry->password);
    }
    addReveal(layout, password);
    addStrength(layout, password);
    auto *actions = new QHBoxLayout;
    layout->addLayout(actions);
    if (readOnly) {
        button(actions, &dialog, QStringLiteral("Копировать логин"), [&] {
            QString value = login->text();
            copy(value);
            wipe(value);
        });
        button(actions, &dialog, QStringLiteral("Копировать пароль"), [&] {
            QString value = password->text();
            copy(value);
            wipe(value);
        });
        button(actions, &dialog, QStringLiteral("Закрыть"), [&] { dialog.accept(); });
        dialog.exec();
        return;
    }

    button(actions, &dialog, QStringLiteral("Сгенерировать пароль"), [&] {
        GeneratorDialog generator(&dialog, QStringLiteral("Использовать пароль"));
        if (generator.exec() == QDialog::Accepted && manager.unlocked()) {
            clearText(password);
            QString value = generator.takePassword();
            password->setText(value);
            wipe(value);
        }
    });
    auto *error = label({}, &dialog, "error");
    layout->addWidget(error);
    auto *buttons = dialogButtons(dialog, layout, QStringLiteral("Сохранить"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!manager.unlocked())
            return;
        Entry edited;
        edited.id = id;
        edited.site = site->text().trimmed();
        edited.url = url->text().trimmed();
        edited.login = login->text();
        edited.password = password->text();
        try {
            manager.put(std::move(edited));
            dialog.accept();
        } catch (const Failure &failure) {
            error->setText(failure.message);
        }
    });
    if (dialog.exec() == QDialog::Accepted)
        refresh(id);
}

void MainWindow::removeEntry()
{
    const auto *entry = selected();
    if (!entry)
        return;
    const auto id = entry->id;
    QMessageBox confirm(QMessageBox::Question, QStringLiteral("Удаление записи"),
        QStringLiteral("Удалить выбранную запись?"), QMessageBox::Ok | QMessageBox::Cancel, this);
    confirm.button(QMessageBox::Ok)->setText(QStringLiteral("Удалить"));
    confirm.button(QMessageBox::Cancel)->setText(QStringLiteral("Отмена"));
    confirm.setDefaultButton(QMessageBox::Cancel);
    if (confirm.exec() != QMessageBox::Ok || !manager.unlocked())
        return;
    try {
        manager.remove(id);
        refresh();
    } catch (const Failure &error) {
        showError(error);
    }
}

void MainWindow::settings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Настройки"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *minutes = new QSpinBox(&dialog);
    minutes->setRange(1, 120);
    minutes->setValue(manager.data().idleMinutes);
    minutes->setSuffix(QStringLiteral(" мин."));
    form->addRow(QStringLiteral("Блокировать при бездействии через"), minutes);
    layout->addLayout(form);
    constexpr int changePasswordResult = 2;
    button(layout, &dialog, QStringLiteral("Изменить мастер-пароль"), [&] {
        dialog.done(changePasswordResult);
    });
    auto *error = label({}, &dialog, "error");
    layout->addWidget(error);
    auto *buttons = dialogButtons(dialog, layout, QStringLiteral("Сохранить"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!manager.unlocked())
            return;
        try {
            manager.setIdleMinutes(minutes->value());
            idleLabel->setText(QStringLiteral("Автоблокировка через %1 мин.").arg(minutes->value()));
            activity.restart();
            dialog.accept();
        } catch (const Failure &failure) {
            error->setText(failure.message);
        }
    });
    if (dialog.exec() == changePasswordResult && manager.unlocked())
        changePassword();
}

void MainWindow::changePassword()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Смена мастер-пароля"));
    dialog.setMinimumWidth(470);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *current = secretField(&dialog, 1024);
    auto *next = secretField(&dialog, 1024);
    auto *confirm = secretField(&dialog, 1024);
    form->addRow(QStringLiteral("Текущий пароль"), current);
    form->addRow(QStringLiteral("Новый пароль"), next);
    form->addRow(QStringLiteral("Повторите пароль"), confirm);
    layout->addWidget(label(QStringLiteral("Ранее экспортированные копии открываются прежним мастер-паролем."), &dialog));
    auto *error = label({}, &dialog, "error");
    layout->addWidget(error);
    const auto cleanup = qScopeGuard([&] { clearFields({current, next, confirm}); });
    auto *buttons = dialogButtons(dialog, layout, QStringLiteral("Изменить пароль"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!manager.unlocked())
            return;
        QString oldValue = takeText(current), newValue = takeText(next), repeated = takeText(confirm);
        const auto wipeValues = qScopeGuard([&] { wipe(oldValue); wipe(newValue); wipe(repeated); });
        try {
            if (newValue != repeated)
                throw Failure{VaultError::Invalid, QStringLiteral("Новые пароли не совпадают.")};
            manager.changePassword(oldValue, newValue);
            clearClipboard();
            dialog.accept();
        } catch (const Failure &failure) {
            error->setText(failure.message);
        }
    });
    dialog.exec();
}

void MainWindow::importVault()
{
    const bool wasUnlocked = manager.unlocked();
    if (!wasUnlocked && manager.exists()) {
        authError->setText(QStringLiteral("Разблокируйте текущее хранилище перед импортом."));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Импорт хранилища"), {},
        QStringLiteral("Хранилище LocalVault (*.lvault);;Все файлы (*)"), nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty() || (wasUnlocked && !manager.unlocked()))
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Импорт хранилища"));
    dialog.setMinimumWidth(450);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label(wasUnlocked
        ? QStringLiteral("Импорт заменит текущие записи, настройки и мастер-пароль. Сохраните резервную копию, если данные ещё нужны.")
        : QStringLiteral("Введите мастер-пароль выбранной резервной копии."), &dialog));
    auto *password = secretField(&dialog, 1024);
    password->setPlaceholderText(QStringLiteral("Мастер-пароль копии"));
    layout->addWidget(password);
    auto *error = label({}, &dialog, "error");
    layout->addWidget(error);
    const auto cleanup = qScopeGuard([&] { clearText(password); });
    auto *buttons = dialogButtons(dialog, layout, wasUnlocked ? QStringLiteral("Заменить хранилище")
                                                              : QStringLiteral("Импортировать"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (wasUnlocked && !manager.unlocked())
            return;
        QString value = takeText(password);
        const auto wipeValue = qScopeGuard([&] { wipe(value); });
        try {
            manager.importFrom(path, value);
            dialog.accept();
        } catch (const Failure &failure) {
            error->setText(failure.message);
            password->setFocus();
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        clearClipboard();
        clearFields({search, master, repeat});
        showUnlocked();
    }
}

void MainWindow::exportVault()
{
    QFileDialog dialog(this, QStringLiteral("Экспорт зашифрованной копии"),
                       {}, QStringLiteral("Хранилище LocalVault (*.lvault)"));
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("lvault"));
    dialog.selectFile(QStringLiteral("backup.lvault"));
    if (dialog.exec() != QDialog::Accepted || !manager.unlocked())
        return;
    try {
        manager.exportTo(dialog.selectedFiles().constFirst());
        status->setText(QStringLiteral("Зашифрованная копия сохранена"));
    } catch (const Failure &failure) {
        showError(failure);
    }
}

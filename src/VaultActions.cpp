#include "MainWindow.h"
#include "Dialogs.h"
#include "IconPicker.h"
#include "TokenStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLocale>
#include <QScrollArea>
#include <QSignalBlocker>
#include <algorithm>
#include <QFileDialog>
#include <QFormLayout>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QSettings>
#include <QSpinBox>
#include <QTextDocument>

namespace {
class RecoveryCodesEditor : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) {
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
};

void clearFields(std::initializer_list<QLineEdit *> fields)
{
    for (auto *field : fields)
        clearText(field);
}

QFormLayout *stackedForm(QVBoxLayout *layout)
{
    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setVerticalSpacing(8);
    layout->addLayout(form);
    return form;
}

bool confirmRemoval(QWidget *parent, const QString &message)
{
    QMessageBox dialog(QMessageBox::Question, QStringLiteral("Подтверждение"), message,
                       QMessageBox::Ok | QMessageBox::Cancel, parent);
    dialog.setTextFormat(Qt::PlainText);
    setAction(dialog.button(QMessageBox::Ok), QStringLiteral("Удалить"), "trash");
    setAction(dialog.button(QMessageBox::Cancel), QStringLiteral("Отмена"), "x-lg");
    dialog.setDefaultButton(QMessageBox::Cancel);
    return dialog.exec() == QMessageBox::Ok;
}

class EmailFields {
public:
    EmailFields(QVBoxLayout *layout, QWidget *parent, bool readOnly)
        : parent(parent), readOnly(readOnly)
    {
        auto *header = new QHBoxLayout;
        auto *title = label(QStringLiteral("Электронные почты"), parent, "cardTitle");
        title->setWordWrap(false);
        header->addWidget(title);
        header->addStretch();
        layout->addLayout(header);
        if (!readOnly) {
            addButton = iconButton(header, parent, QStringLiteral("Добавить электронную почту"),
                                   "plus-lg", [this] { add({}, {}); });
        }
        rows = new QVBoxLayout;
        layout->addLayout(rows);
    }

    ~EmailFields()
    {
        for (const auto &row : fields)
            clearFields({row.address, row.description});
    }

    void add(const QString &address, const QString &description)
    {
        if (fields.size() >= Entry::maxEmails)
            return;
        auto *widget = new QWidget(parent);
        auto *layout = new QHBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *mail = new QLineEdit(address, widget);
        auto *note = new QLineEdit(description, widget);
        for (auto *field : {mail, note}) {
            field->setMaxLength(2048);
            field->setReadOnly(readOnly);
        }
        mail->setPlaceholderText(QStringLiteral("name@example.com"));
        mail->setAccessibleName(QStringLiteral("Адрес электронной почты"));
        note->setPlaceholderText(QStringLiteral("Например, рабочая"));
        note->setAccessibleName(QStringLiteral("Описание почты"));
        auto *mailColumn = new QVBoxLayout;
        mailColumn->setSpacing(5);
        auto *mailLabel = label(QStringLiteral("Адрес"), widget);
        mailLabel->setBuddy(mail);
        mailColumn->addWidget(mailLabel);
        mailColumn->addWidget(mail);
        layout->addLayout(mailColumn, 3);
        auto *noteColumn = new QVBoxLayout;
        noteColumn->setSpacing(5);
        auto *noteLabel = label(QStringLiteral("Описание (необязательно)"), widget);
        noteLabel->setBuddy(note);
        noteColumn->addWidget(noteLabel);
        noteColumn->addWidget(note);
        layout->addLayout(noteColumn, 2);
        fields.push_back({mail, note});
        if (!readOnly) {
            auto *remove = iconButton(layout, widget, QStringLiteral("Убрать адрес"), "dash-lg", [=] {
                clearFields({mail, note});
                const auto row = std::find_if(fields.begin(), fields.end(),
                                              [mail](const Row &item) { return item.address == mail; });
                fields.erase(row);
                rows->removeWidget(widget);
                widget->hide();
                widget->deleteLater();
                addButton->setEnabled(true);
            });
            layout->setAlignment(remove, Qt::AlignBottom);
            addButton->setEnabled(fields.size() < Entry::maxEmails);
        }
        rows->addWidget(widget);
        if (address.isEmpty())
            mail->setFocus();
    }

    std::vector<EmailAddress> collect() const
    {
        std::vector<EmailAddress> result;
        result.reserve(fields.size());
        for (const auto &row : fields) {
            EmailAddress email;
            email.address = row.address->text().trimmed();
            email.description = row.description->text().trimmed();
            if (!email.address.isEmpty() || !email.description.isEmpty())
                result.push_back(std::move(email));
        }
        return result;
    }

private:
    struct Row { QLineEdit *address; QLineEdit *description; };
    QWidget *parent;
    bool readOnly;
    QVBoxLayout *rows;
    QPushButton *addButton = nullptr;
    std::vector<Row> fields;
};
}

void MainWindow::groupDialog(bool edit)
{
    const auto id = edit ? selectedGroup : QUuid::createUuid();
    if (id.isNull())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(edit ? QStringLiteral("Изменить группу") : QStringLiteral("Новая группа"));
    dialog.setMinimumWidth(400);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 16);
    auto *form = stackedForm(layout);
    auto *name = new QLineEdit(&dialog);
    name->setMaxLength(2048);
    name->setPlaceholderText(QStringLiteral("Например, СОЦСЕТИ"));
    form->addRow(QStringLiteral("Название группы"), name);
    QString previousIcon;
    if (edit) {
        for (const auto &group : manager.data().groups) {
            if (group.id == id)
                previousIcon = group.iconName;
            if (group.id == id)
                name->setText(group.name);
        }
    }
    auto *icon = new IconPicker(&dialog, previousIcon);
    form->addRow(QStringLiteral("Иконка"), icon);
    const auto cleanup = qScopeGuard([&] { clearText(name); });
    auto *error = label({}, &dialog, "error");
    layout->addWidget(error);
    auto *controls = dialogButtons(dialog, layout, QStringLiteral("Сохранить"), true);
    connect(controls, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!manager.unlocked())
            return;
        Group group;
        group.id = id;
        group.name = name->text().simplified();
        group.iconName = icon->selectedName();
        try {
            manager.putGroup(std::move(group));
            dialog.accept();
        } catch (const Failure &failure) {
            error->setText(failure.message);
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        selectedGroup = id;
        selectedSite = QUuid{};
        expandedGroups.insert(id);
        const QSignalBlocker block(search);
        clearText(search);
        refresh();
    }
}

void MainWindow::removeGroup()
{
    const auto id = selectedGroup;
    if (id.isNull() || !confirmRemoval(this, QStringLiteral("Удалить группу? Её сайты и аккаунты останутся в общем списке."))
        || !manager.unlocked())
        return;
    try {
        manager.removeGroup(id);
        refresh();
    } catch (const Failure &failure) {
        showError(failure);
    }
}

void MainWindow::siteDialog(const Site *site)
{
    const auto id = site ? site->id : QUuid::createUuid();
    QDialog dialog(this);
    dialog.setWindowTitle(site ? QStringLiteral("Изменить сайт") : QStringLiteral("Новый сайт"));
    dialog.setMinimumWidth(460);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(12);
    auto *form = stackedForm(layout);
    auto *name = new QLineEdit(site ? site->name : QString{}, &dialog);
    auto *url = new QLineEdit(site ? site->url : QString{}, &dialog);
    name->setMaxLength(2048);
    url->setMaxLength(2048);
    name->setPlaceholderText(QStringLiteral("Например, GitHub"));
    url->setPlaceholderText(QStringLiteral("https://example.com"));
    form->addRow(QStringLiteral("Название"), name);
    form->addRow(QStringLiteral("Адрес"), url);
    auto *group = new QComboBox(&dialog);
    group->setAccessibleName(QStringLiteral("Группа сайта"));
    group->addItem(QStringLiteral("Без группы"), QUuid{});
    for (const auto &item : manager.data().groups)
        group->addItem(item.name, item.id);
    const auto previousGroup = site ? site->groupId : selectedGroup;
    group->setCurrentIndex(qMax(0, group->findData(previousGroup)));
    form->addRow(QStringLiteral("Группа"), group);
    auto *icon = new IconPicker(&dialog, site ? site->iconName : QString{});
    form->addRow(QStringLiteral("Иконка"), icon);
    const auto cleanup = qScopeGuard([&] { clearFields({name, url}); });
    auto *error = label({}, &dialog, "error");
    layout->addWidget(error);
    auto *controls = dialogButtons(dialog, layout, QStringLiteral("Сохранить"), true);
    connect(controls, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!manager.unlocked())
            return;
        Site edited;
        edited.id = id;
        edited.name = name->text().simplified();
        edited.url = url->text().trimmed();
        edited.groupId = group->currentData().toUuid();
        edited.iconName = icon->selectedName();
        try {
            manager.putSite(std::move(edited));
            dialog.accept();
        } catch (const Failure &failure) {
            error->setText(failure.message);
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        const QSignalBlocker block(search);
        clearText(search);
        refresh(id);
    }
}

void MainWindow::removeSite()
{
    if (selectedSite.isNull())
        return;
    const auto id = selectedSite;
    if (!confirmRemoval(this, QStringLiteral("Удалить выбранный сайт и все его аккаунты?")) || !manager.unlocked())
        return;
    try {
        manager.removeSite(id);
        refresh();
    } catch (const Failure &failure) {
        showError(failure);
    }
}

void MainWindow::entryDialog(const Entry *entry, bool readOnly)
{
    const auto siteId = entry ? entry->siteId : selectedSite;
    if (siteId.isNull() || (readOnly && !entry))
        return;
    const auto id = entry ? entry->id : QUuid::createUuid();
    const bool isNew = !entry;
    const QDateTime previousDate = entry ? entry->createdAt : QDateTime{};
    QDialog dialog(this);
    dialog.setWindowTitle(readOnly ? QStringLiteral("Данные аккаунта")
        : isNew ? QStringLiteral("Новый аккаунт") : QStringLiteral("Изменить аккаунт"));
    dialog.resize(610, 635);
    dialog.setMinimumSize(500, 390);
    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(20, 18, 20, 16);
    outer->setSpacing(12);
    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll, 1);
    auto *body = new QWidget(scroll);
    scroll->setWidget(body);
    auto *layout = new QVBoxLayout(body);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(12);
    auto *form = stackedForm(layout);
    const auto add = [&](const QString &name, const QString &hint, QLineEdit *field) {
        field->setMaxLength(2048);
        field->setReadOnly(readOnly);
        if (!readOnly)
            field->setPlaceholderText(hint);
        form->addRow(name, field);
        return field;
    };
    if (!isNew) {
        auto *idField = new QLineEdit(id.toString(QUuid::WithoutBraces), body);
        idField->setReadOnly(true);
        form->addRow(QStringLiteral("Локальный ID"), idField);
    }
    auto *login = add(QStringLiteral("Логин"), QStringLiteral("Введите логин"), new QLineEdit(body));
    auto *password = secretField(body);
    password->setReadOnly(readOnly);
    password->setAccessibleName(QStringLiteral("Пароль"));
    if (!readOnly)
        password->setPlaceholderText(QStringLiteral("Введите пароль"));
    auto *passwordRow = new QHBoxLayout;
    passwordRow->setSpacing(6);
    passwordRow->addWidget(password, 1);
    addReveal(passwordRow, password);
    form->addRow(QStringLiteral("Пароль"), passwordRow);
    const auto cleanup = qScopeGuard([&] { clearFields({login, password}); });
    if (entry) {
        login->setText(entry->login);
        password->setText(entry->password);
    }
    addStrength(layout, password);
    auto *signInForm = stackedForm(layout);
    auto *alternative = new QCheckBox(QStringLiteral("Альтернативный способ входа"), body);
    alternative->setChecked(entry && entry->alternativeSignIn);
    alternative->setEnabled(!readOnly);
    signInForm->addRow(alternative);
    auto *method = new QLineEdit(entry ? entry->signInMethod : QString{}, body);
    method->setMaxLength(2048);
    method->setPlaceholderText(QStringLiteral("Например, OAuth через Google или аутентификатор"));
    method->setReadOnly(readOnly);
    method->setEnabled(alternative->isChecked());
    signInForm->addRow(QStringLiteral("Способ входа"), method);
    connect(alternative, &QCheckBox::toggled, method, &QLineEdit::setEnabled);
    connect(alternative, &QCheckBox::toggled, password, [password](bool enabled) {
        password->setPlaceholderText(enabled ? QStringLiteral("Пароль (необязательно)") : QStringLiteral("Введите пароль"));
    });
    if (!readOnly && alternative->isChecked())
        password->setPlaceholderText(QStringLiteral("Пароль (необязательно)"));
    const auto clearMethod = qScopeGuard([&] { clearText(method); });
    if (!readOnly) {
        iconButton(passwordRow, body, QStringLiteral("Сгенерировать пароль"), "shuffle", [&] {
            GeneratorDialog generator(&dialog, QStringLiteral("Использовать пароль"));
            if (generator.exec() == QDialog::Accepted && manager.unlocked()) {
                clearText(password);
                QString value = generator.takePassword();
                password->setText(value);
                wipe(value);
            }
        });
    }

    layout->addWidget(label(QStringLiteral("Дополнительно"), body, "cardTitle"));
    form = stackedForm(layout);
    auto *userName = add(QStringLiteral("Имя пользователя"), QStringLiteral("Например, Алексей"), new QLineEdit(body));
    auto *phone = add(QStringLiteral("Телефон"), QStringLiteral("+7 900 000-00-00"), new QLineEdit(body));
    const auto clearAdditional = qScopeGuard([&] { clearFields({userName, phone}); });
    if (entry) {
        userName->setText(entry->userName);
        phone->setText(entry->phone);
    }
    QCheckBox *withDate = nullptr;
    QDateTimeEdit *date = nullptr;
    bool dateEdited = false;
    if (readOnly) {
        form->addRow(QStringLiteral("Дата создания"), label(entry->createdAt.isValid()
            ? QLocale(QLocale::Russian).toString(entry->createdAt.toLocalTime(), QLocale::ShortFormat)
            : QStringLiteral("Не указана"), body));
    } else {
        withDate = new QCheckBox(QStringLiteral("Указать дату создания"), body);
        withDate->setChecked(isNew || entry->createdAt.isValid());
        form->addRow(withDate);
        date = new QDateTimeEdit(body);
        date->setDisplayFormat(QStringLiteral("dd.MM.yyyy HH:mm:ss"));
        date->setCalendarPopup(true);
        date->setDateTimeRange(QDateTime(QDate(1, 1, 1), QTime(0, 0)),
                               QDateTime(QDate(9999, 12, 31), QTime(23, 59, 59)));
        date->setDateTime(entry && entry->createdAt.isValid()
                          ? entry->createdAt.toLocalTime() : QDateTime::currentDateTime());
        date->setEnabled(withDate->isChecked());
        connect(withDate, &QCheckBox::toggled, date, &QDateTimeEdit::setEnabled);
        connect(date, &QDateTimeEdit::dateTimeChanged, &dialog, [&] { dateEdited = true; });
        form->addRow(QStringLiteral("Дата и время"), date);
    }

    EmailFields emails(layout, body, readOnly);
    if (entry) {
        for (const auto &email : entry->emails)
            emails.add(email.address, email.description);
    }
    if (readOnly && entry->emails.empty())
        layout->addWidget(label(QStringLiteral("Адреса не указаны."), body));

    auto *codesHeader = new QHBoxLayout;
    auto *codesTitle = label(QStringLiteral("Запасные коды восстановления"), body, "cardTitle");
    codesTitle->setWordWrap(false);
    codesHeader->addWidget(codesTitle);
    codesHeader->addStretch();
    layout->addLayout(codesHeader);
    auto *codes = new RecoveryCodesEditor(body);
    codes->setAccessibleName(QStringLiteral("Коды восстановления"));
    codes->setPlaceholderText(QStringLiteral("Вставьте коды, каждый с новой строки"));
    codes->setUndoRedoEnabled(false);
    codes->setContextMenuPolicy(Qt::NoContextMenu);
    codes->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
    codes->setReadOnly(readOnly);
    codes->setFixedHeight(110);
    if (entry)
        codes->setPlainText(entry->recoveryCodes);
    codes->hide();
    layout->addWidget(codes);
    auto *masked = label(codes->document()->isEmpty() ? QStringLiteral("Коды не указаны")
                                                     : QStringLiteral("••••••••"), body);
    layout->addWidget(masked);
    auto *revealCodes = iconButton(codesHeader, body, QStringLiteral("Показать коды восстановления"), "eye", [] {});
    revealCodes->setCheckable(true);
    connect(revealCodes, &QPushButton::toggled, codes, [=](bool shown) {
        codes->setVisible(shown);
        masked->setVisible(!shown);
        masked->setText(codes->document()->isEmpty() ? QStringLiteral("Коды не указаны") : QStringLiteral("••••••••"));
        setAction(revealCodes, shown ? QStringLiteral("Скрыть коды восстановления")
                                    : QStringLiteral("Показать коды восстановления"), shown ? "eye-slash" : "eye");
        if (shown)
            codes->setFocus();
    });
    auto *copyCodes = iconButton(codesHeader, body, QStringLiteral("Копировать коды восстановления"), "copy", [&] {
        QString value = codes->toPlainText();
        copy(value);
        wipe(value);
    });
    copyCodes->setEnabled(!codes->document()->isEmpty());
    connect(codes, &QPlainTextEdit::textChanged, copyCodes, [=] {
        copyCodes->setEnabled(!codes->document()->isEmpty());
    });
    const auto clearCodes = qScopeGuard([&] {
        QString value = codes->toPlainText();
        codes->clear();
        wipe(value);
    });
    layout->addStretch();
    if (readOnly) {
        auto *actions = new QHBoxLayout;
        outer->addLayout(actions);
        iconButton(actions, &dialog, QStringLiteral("Копировать логин"), "copy", [&] {
            QString value = login->text();
            copy(value);
            wipe(value);
        });
        iconButton(actions, &dialog, QStringLiteral("Копировать пароль"), "key", [&] {
            QString value = password->text();
            copy(value);
            wipe(value);
        });
        actions->addStretch();
        iconButton(actions, &dialog, QStringLiteral("Закрыть"), "x-lg", [&] { dialog.accept(); });
        dialog.exec();
        return;
    }

    auto *error = label({}, &dialog, "error");
    outer->addWidget(error);
    auto *buttons = dialogButtons(dialog, outer, QStringLiteral("Сохранить"), true);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!manager.unlocked())
            return;
        Entry edited;
        edited.id = id;
        edited.siteId = siteId;
        edited.login = login->text();
        edited.password = password->text();
        edited.alternativeSignIn = alternative->isChecked();
        if (edited.alternativeSignIn)
            edited.signInMethod = method->text().trimmed();
        edited.userName = userName->text().trimmed();
        edited.phone = phone->text().trimmed();
        edited.emails = emails.collect();
        edited.recoveryCodes = codes->toPlainText();
        if (!withDate->isChecked())
            edited.createdAt = {};
        else if (dateEdited)
            edited.createdAt = date->dateTime().toUTC();
        else if (previousDate.isValid())
            edited.createdAt = previousDate;
        try {
            manager.put(std::move(edited));
            dialog.accept();
        } catch (const Failure &failure) {
            error->setText(failure.message);
        }
    });
    if (dialog.exec() == QDialog::Accepted)
        renderSite();
}

void MainWindow::removeEntry(const QUuid &id)
{
    if (!confirmRemoval(this, QStringLiteral("Удалить выбранный аккаунт?")) || !manager.unlocked())
        return;
    try {
        manager.remove(id);
        renderSite();
    } catch (const Failure &failure) {
        showError(failure);
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
    auto *actions = new QHBoxLayout;
    layout->addLayout(actions);
    button(actions, &dialog, QStringLiteral("Изменить мастер-пароль"), [&] {
        dialog.done(changePasswordResult);
    });
    actions->addStretch();
    auto *error = label({}, &dialog, "error");
    layout->addWidget(error);
    auto *buttons = dialogButtons(dialog, layout, QStringLiteral("Сохранить"), true);
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
    auto *buttons = dialogButtons(dialog, layout, QStringLiteral("Изменить пароль"), true);
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

void MainWindow::saveVaultLocation(const QString &customPath, QByteArray token)
{
    QSettings location(locationSettingsPath, QSettings::IniFormat);
    const auto previousPath = location.value(QStringLiteral("vaultPath")).toString();
    const auto previousToken = location.value(QStringLiteral("apiTokenProtected")).toByteArray();
    const auto protectedToken = StorageManager::isRemoteSource(customPath)
        ? TokenStore::protect(token, locationSettingsPath + '\n' + customPath) : QByteArray{};
    if (customPath.isEmpty())
        location.remove(QStringLiteral("vaultPath"));
    else
        location.setValue(QStringLiteral("vaultPath"), customPath);
    if (protectedToken.isEmpty())
        location.remove(QStringLiteral("apiTokenProtected"));
    else
        location.setValue(QStringLiteral("apiTokenProtected"), protectedToken);
    location.sync();
    if (location.status() != QSettings::NoError)
        throw Failure{VaultError::Io, QStringLiteral("Не удалось сохранить настройки пути.")};

    if (previousPath != customPath)
        TokenStore::remove(previousToken, locationSettingsPath + '\n' + previousPath);
    manager.selectPath(customPath.isEmpty() ? defaultVaultPath : customPath, !customPath.isEmpty(), std::move(token));
    locationError.clear();
}

void MainWindow::vaultLocationDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Источник хранилища"));
    dialog.setMinimumWidth(570);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);
    auto *form = stackedForm(layout);
    auto *path = new QLineEdit(manager.isRemote() ? manager.path() : QDir::toNativeSeparators(manager.path()), &dialog);
    path->setPlaceholderText(QStringLiteral("Путь к .lvault или https://сервер/vault"));
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(path, 1);
    button(row, &dialog, QStringLiteral("Обзор…"), [&] {
        const auto chosen = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Выберите хранилище"), path->text(),
            QStringLiteral("Хранилище LocalVault (*.lvault)"), nullptr, QFileDialog::DontUseNativeDialog);
        if (!chosen.isEmpty())
            path->setText(QDir::toNativeSeparators(chosen));
    });
    form->addRow(QStringLiteral("Источник"), row);
    auto *token = secretField(&dialog, 1024);
    token->setPlaceholderText(QStringLiteral("API-токен сервера"));
    token->setText(apiTokenField->text());
    form->addRow(QStringLiteral("API-токен (для URL)"), token);
    const auto clearToken = qScopeGuard([&] { clearText(token); });
    layout->addWidget(label(QStringLiteral("Для удалённого доступа используйте HTTPS. API-токен запоминается с защитой средствами ОС."), &dialog));
    layout->addWidget(label(QStringLiteral("По умолчанию: %1").arg(QDir::toNativeSeparators(defaultVaultPath)), &dialog));
    auto *error = label({}, &dialog, "error");
    layout->addWidget(error);

    const auto apply = [&](const QString &customPath) {
        try {
            if (!customPath.isEmpty()) {
                if (StorageManager::isRemoteSource(customPath)) {
                    QString secret = token->text();
                    const auto clearSecret = qScopeGuard([&] { wipe(secret); });
                    StorageManager::readHeader(StorageManager(customPath, secret.toUtf8()).read());
                } else {
                    const QFileInfo file(customPath);
                    if (!QDir::isAbsolutePath(customPath) || !file.isFile()
                        || file.suffix().compare(QStringLiteral("lvault"), Qt::CaseInsensitive) != 0)
                        throw Failure{VaultError::Invalid, QStringLiteral("Укажите полный путь к существующему файлу .lvault или URL сервера.")};
                    StorageManager::readHeader(StorageManager(customPath).read());
                }
            }
            QString secret = token->text();
            const auto clearSecret = qScopeGuard([&] { wipe(secret); });
            saveVaultLocation(customPath, StorageManager::isRemoteSource(customPath) ? secret.toUtf8() : QByteArray{});
            clearText(apiTokenField);
            if (StorageManager::isRemoteSource(customPath))
                apiTokenField->setText(secret);
            dialog.accept();
        } catch (const Failure &failure) {
            error->setText(failure.message);
        }
    };

    auto *actions = new QHBoxLayout;
    layout->addLayout(actions);
    button(actions, &dialog, QStringLiteral("Сбросить путь"), [&] { apply({}); });
    actions->addStretch();
    auto *controls = dialogButtons(dialog, actions, QStringLiteral("Сохранить"), true);
    connect(controls, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto entered = path->text().trimmed();
        if (entered.isEmpty()) {
            error->setText(QStringLiteral("Укажите путь к файлу или нажмите «Сбросить путь»."));
            return;
        }
        apply(StorageManager::isRemoteSource(entered) ? entered : QDir::cleanPath(QDir::fromNativeSeparators(entered)));
    });

    if (dialog.exec() == QDialog::Accepted) {
        clearFields({master, repeat});
        showLocked();
    }
}

void MainWindow::importVault()
{
    const bool wasUnlocked = manager.unlocked();
    if (!wasUnlocked) {
        try {
            if (!locationError.isEmpty())
                throw Failure{VaultError::Io, locationError};
            manager.checkAvailability();
            if (manager.usesCustomPath() || manager.exists())
                throw Failure{VaultError::Invalid, QStringLiteral("Разблокируйте текущее хранилище перед импортом.")};
        } catch (const Failure &error) {
            authError->setText(error.message);
            return;
        }
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
                                                              : QStringLiteral("Импортировать"), !wasUnlocked);
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
        const QSignalBlocker block(search);
        selectedSite = QUuid{};
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

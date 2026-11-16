#include "Dialogs.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QProgressBar>
#include <QSpinBox>

namespace {
class SecretField : public QLineEdit {
public:
    using QLineEdit::QLineEdit;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) {
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
};
}

QLabel *label(const QString &text, QWidget *parent, const char *style)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(QString::fromLatin1(style));
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}

QPushButton *button(QBoxLayout *layout, QWidget *parent, const QString &text,
                    const std::function<void()> &action, bool primary)
{
    auto *result = new QPushButton(text, parent);
    if (primary)
        result->setObjectName(QStringLiteral("primary"));
    layout->addWidget(result);
    QObject::connect(result, &QPushButton::clicked, parent, action);
    return result;
}

QDialogButtonBox *dialogButtons(QDialog &dialog, QVBoxLayout *layout, const QString &acceptText)
{
    auto *result = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *accept = result->addButton(acceptText, QDialogButtonBox::AcceptRole);
    accept->setObjectName(QStringLiteral("primary"));
    accept->setDefault(true);
    result->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    layout->addWidget(result);
    QObject::connect(result, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return result;
}

QLineEdit *secretField(QWidget *parent, int maxLength)
{
    auto *field = new SecretField(parent);
    field->setEchoMode(QLineEdit::Password);
    field->setMaxLength(maxLength);
    field->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData
                              | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    field->setContextMenuPolicy(Qt::NoContextMenu);
    return field;
}

QString takeText(QLineEdit *field)
{
    QString result = field->text();
    field->setText({});
    return result;
}

void clearText(QLineEdit *field)
{
    QString value = takeText(field);
    wipe(value);
}

void addStrength(QVBoxLayout *layout, QLineEdit *field)
{
    auto *bar = new QProgressBar(field->parentWidget());
    bar->setRange(0, 4);
    layout->addWidget(bar);
    const auto update = [bar](const QString &value) {
        static const QString strengths[] = {
            QStringLiteral("Оценка пароля"), QStringLiteral("Слабый"),
            QStringLiteral("Невысокая стойкость"), QStringLiteral("Хорошая стойкость"),
            QStringLiteral("Высокая стойкость")
        };
        const int score = PasswordGenerator::strength(value);
        bar->setValue(score);
        bar->setFormat(strengths[score]);
    };
    QObject::connect(field, &QLineEdit::textChanged, bar, update);
    update(field->text());
}

void addReveal(QVBoxLayout *layout, QLineEdit *field)
{
    auto *reveal = new QCheckBox(QStringLiteral("Показать пароль"), field->parentWidget());
    layout->addWidget(reveal);
    QObject::connect(reveal, &QCheckBox::toggled, field, [field](bool shown) {
        field->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
    });
}

GeneratorDialog::GeneratorDialog(QWidget *parent, const QString &acceptText) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Генератор паролей"));
    setMinimumWidth(430);
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *length = new QSpinBox(this);
    length->setRange(4, 256);
    length->setValue(24);
    form->addRow(QStringLiteral("Длина"), length);
    auto *lower = new QCheckBox(QStringLiteral("a–z"), this);
    auto *upper = new QCheckBox(QStringLiteral("A–Z"), this);
    auto *digits = new QCheckBox(QStringLiteral("0–9"), this);
    auto *symbols = new QCheckBox(QStringLiteral("Спецсимволы"), this);
    auto *options = new QHBoxLayout;
    for (auto *option : {lower, upper, digits, symbols}) {
        option->setChecked(true);
        options->addWidget(option);
    }
    layout->addLayout(options);
    result = secretField(this, 256);
    result->setReadOnly(true);
    layout->addWidget(result);
    addReveal(layout, result);
    addStrength(layout, result);
    auto *error = label({}, this, "error");
    layout->addWidget(error);
    const auto regenerate = [=] {
        clearText(result);
        try {
            QString generated = PasswordGenerator::generate({length->value(), lower->isChecked(),
                upper->isChecked(), digits->isChecked(), symbols->isChecked()});
            result->setText(generated);
            wipe(generated);
            error->clear();
        } catch (const Failure &failure) {
            error->setText(failure.message);
        }
    };
    button(layout, this, QStringLiteral("Сгенерировать"), regenerate);
    auto *buttons = dialogButtons(*this, layout, acceptText);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!result->text().isEmpty())
            accept();
    });
    connect(length, &QSpinBox::valueChanged, this, regenerate);
    for (auto *option : {lower, upper, digits, symbols})
        connect(option, &QCheckBox::toggled, this, regenerate);
    regenerate();
}

GeneratorDialog::~GeneratorDialog()
{
    clearText(result);
}

QString GeneratorDialog::takePassword()
{
    return takeText(result);
}

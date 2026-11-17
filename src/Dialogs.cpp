#include "Dialogs.h"

#include <QCheckBox>
#include <QFile>
#include <QFormLayout>
#include <QHash>
#include <QKeyEvent>
#include <QPainter>
#include <QProgressBar>
#include <QSpinBox>
#include <QSvgRenderer>

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

QIcon actionIcon(const char *name)
{
    static const bool initialized = [] { Q_INIT_RESOURCE(bootstrap_icons); return true; }();
    Q_UNUSED(initialized);
    static QHash<QByteArray, QIcon> cache;
    const QByteArray key(name);
    const auto found = cache.constFind(key);
    if (found != cache.cend())
        return found.value();
    QFile file(QStringLiteral(":/actions/%1.svg").arg(QString::fromLatin1(name)));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QByteArray svg = file.readAll();
    svg.replace("currentColor", "#ffffff");
    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return {};
    QIcon icon;
    for (int scale : {1, 2, 3}) {
        QPixmap image(18 * scale, 18 * scale);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.render(&painter);
        painter.end();
        image.setDevicePixelRatio(scale);
        icon.addPixmap(image);
        icon.addPixmap(image, QIcon::Selected);
    }
    cache.insert(key, icon);
    return icon;
}

void setAction(QAbstractButton *control, const QString &title, const char *icon)
{
    control->setText({});
    control->setToolTip(title);
    control->setAccessibleName(title);
    control->setIcon(actionIcon(icon));
    control->setIconSize(QSize(18, 18));
    control->setMinimumSize(36, 34);
    control->setCursor(Qt::PointingHandCursor);
}

QPushButton *iconButton(QBoxLayout *layout, QWidget *parent, const QString &title,
                        const char *icon, const std::function<void()> &action)
{
    auto *control = button(layout, parent, {}, action);
    setAction(control, title, icon);
    return control;
}

QDialogButtonBox *dialogButtons(QDialog &dialog, QBoxLayout *layout, const QString &acceptText, bool textButtons)
{
    auto *result = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *accept = result->addButton(acceptText, QDialogButtonBox::AcceptRole);
    accept->setObjectName(QStringLiteral("primary"));
    accept->setDefault(true);
    auto *cancel = result->button(QDialogButtonBox::Cancel);
    if (textButtons) {
        cancel->setText(QStringLiteral("Отмена"));
        cancel->setMinimumWidth(96);
        accept->setMinimumWidth(110);
    } else {
        setAction(accept, acceptText, "check-lg");
        setAction(cancel, QStringLiteral("Отмена"), "x-lg");
    }
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

void addReveal(QHBoxLayout *row, QLineEdit *field)
{
    auto *reveal = new QPushButton(field->parentWidget());
    reveal->setCheckable(true);
    setAction(reveal, QStringLiteral("Показать пароль"), "eye");
    row->addWidget(reveal);
    reveal->setFocusPolicy(Qt::StrongFocus);
    QObject::connect(reveal, &QPushButton::toggled, field, [field, reveal](bool shown) {
        field->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
        setAction(reveal, shown ? QStringLiteral("Скрыть пароль") : QStringLiteral("Показать пароль"),
                  shown ? "eye-slash" : "eye");
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
    auto *resultRow = new QHBoxLayout;
    layout->addLayout(resultRow);
    resultRow->addWidget(result, 1);
    addReveal(resultRow, result);
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
    auto *action = new QHBoxLayout;
    layout->addLayout(action);
    iconButton(action, this, QStringLiteral("Сгенерировать"), "shuffle", regenerate);
    action->addStretch();
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

#pragma once

#include "Vault.h"

#include <QBoxLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <functional>

QLabel *label(const QString &text, QWidget *parent, const char *style = "secondary");
QPushButton *button(QBoxLayout *layout, QWidget *parent, const QString &text,
                    const std::function<void()> &action, bool primary = false);
QIcon actionIcon(const char *name);
void setAction(QAbstractButton *control, const QString &title, const char *icon);
QPushButton *iconButton(QBoxLayout *layout, QWidget *parent, const QString &title,
                        const char *icon, const std::function<void()> &action);
QDialogButtonBox *dialogButtons(QDialog &dialog, QBoxLayout *layout, const QString &acceptText, bool textButtons = false);
QLineEdit *secretField(QWidget *parent, int maxLength = 2048);
QString takeText(QLineEdit *field);
void clearText(QLineEdit *field);
void addStrength(QVBoxLayout *layout, QLineEdit *field);
void addReveal(QHBoxLayout *row, QLineEdit *field);

class GeneratorDialog : public QDialog {
public:
    GeneratorDialog(QWidget *parent, const QString &acceptText);
    ~GeneratorDialog() override;
    QString takePassword();

private:
    QLineEdit *result;
};

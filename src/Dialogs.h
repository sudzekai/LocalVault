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
QDialogButtonBox *dialogButtons(QDialog &dialog, QVBoxLayout *layout, const QString &acceptText);
QLineEdit *secretField(QWidget *parent, int maxLength = 2048);
QString takeText(QLineEdit *field);
void clearText(QLineEdit *field);
void addStrength(QVBoxLayout *layout, QLineEdit *field);
void addReveal(QVBoxLayout *layout, QLineEdit *field);

class GeneratorDialog : public QDialog {
public:
    GeneratorDialog(QWidget *parent, const QString &acceptText);
    ~GeneratorDialog() override;
    QString takePassword();

private:
    QLineEdit *result;
};

#pragma once
#include "Vault.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(const QString &path);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void showLocked();
    void showUnlocked();
    void lock();
    void refresh(const QUuid &id = {});
    const Entry *selected() const;
    void entryDialog(const Entry *entry, bool readOnly = false);
    void removeEntry();
    void copy(const QString &value);
    void clearClipboard();
    void settings();
    void changePassword();
    void importVault();
    void exportVault();
    void showError(const Failure &error);

    PasswordManager manager;
    QElapsedTimer activity;
    QTimer idleTimer;
    QTimer clipboardTimer;
    QByteArray clipboardToken;
    QStackedWidget *pages;
    QWidget *lockedPage;
    QWidget *vaultPage;
    QLabel *authTitle;
    QLabel *authHint;
    QLabel *authError;
    QLineEdit *master;
    QLineEdit *repeat;
    QPushButton *authButton;
    QLineEdit *search;
    QTableWidget *table;
    QLabel *status;
    QLabel *idleLabel;
    QList<QPushButton *> selectionActions;
};

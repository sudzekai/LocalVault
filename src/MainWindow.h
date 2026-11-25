#pragma once
#include "Vault.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>
#include <QSet>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QScrollArea;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(const QString &path);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void vaultLocationDialog();
    void saveVaultLocation(const QString &customPath);
    void showLocked();
    void showUnlocked();
    void lock();
    void refresh(const QUuid &siteId = {});
    void renderSite();
    const Site *currentSite() const;
    const Entry *account(const QUuid &id) const;
    void siteDialog(const Site *site = nullptr);
    void groupDialog(bool edit = false);
    void removeGroup();
    void sidebarMenu(const QPoint &position, QTreeWidgetItem *item);
    void createSiteInGroup(const QUuid &groupId);
    void removeSite();
    void entryDialog(const Entry *entry, bool readOnly = false);
    void removeEntry(const QUuid &id);
    void updateAccountCount();
    void copy(const QString &value);
    void clearClipboard();
    void settings();
    void changePassword();
    void importVault();
    void exportVault();
    void showError(const Failure &error);

    QString defaultVaultPath;
    QString locationSettingsPath;
    QString locationError;
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
    QWidget *sidebar;
    QTreeWidget *sites;
    QUuid selectedGroup;
    QSet<QUuid> expandedGroups;
    QScrollArea *cards;
    QLabel *siteTitle;
    QLabel *siteAddress;
    QList<QPushButton *> siteActions;
    QUuid selectedSite;
    int accountCount = 0;
    QLabel *status;
    QLabel *idleLabel;
};

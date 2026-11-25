#pragma once

#include <QIcon>
#include <QPushButton>

QIcon siteIcon(const QString &name);
QStringList availableSiteIcons();

class IconPicker : public QPushButton {
public:
    explicit IconPicker(QWidget *parent, const QString &selected = {});
    const QString &selectedName() const { return name; }

private:
    void choose();
    void updatePreview();
    QString name;
};

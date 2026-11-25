#include "IconPicker.h"

#include <QAbstractListModel>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <QWidgetAction>

static void initializeIcons()
{
    static const bool initialized = [] { Q_INIT_RESOURCE(site_icons); return true; }();
    Q_UNUSED(initialized);
}

namespace {
class IconListModel : public QAbstractListModel {
public:
    explicit IconListModel(QObject *parent) : QAbstractListModel(parent), all(availableSiteIcons()), filtered(all) {}

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : filtered.size();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= filtered.size())
            return {};
        const auto &name = filtered.at(index.row());
        switch (role) {
        case Qt::DecorationRole:
            return siteIcon(name);
        case Qt::DisplayRole:
        case Qt::ToolTipRole:
        case Qt::AccessibleTextRole:
            return name;
        default:
            return {};
        }
    }

    void search(const QString &text)
    {
        beginResetModel();
        filtered.clear();
        for (const auto &name : all) {
            if (name.contains(text.trimmed(), Qt::CaseInsensitive))
                filtered.append(name);
        }
        endResetModel();
    }

private:
    QStringList all;
    QStringList filtered;
};
}

QStringList availableSiteIcons()
{
    initializeIcons();
    static const QStringList names = [] {
        auto files = QDir(QStringLiteral(":/site-icons")).entryList({QStringLiteral("*.svg")}, QDir::Files, QDir::Name);
        for (auto &file : files)
            file.chop(4);
        return files;
    }();
    return names;
}

QIcon siteIcon(const QString &name)
{
    initializeIcons();
    if (name.isEmpty())
        return {};
    static QHash<QString, QIcon> cache;
    const auto found = cache.constFind(name);
    if (found != cache.cend())
        return found.value();

    QFile file(QStringLiteral(":/site-icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    auto svg = file.readAll();
    svg.replace("currentColor", "#ffffff");
    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return {};
    QIcon icon;
    for (int scale : {1, 2, 3}) {
        QPixmap pixmap(24 * scale, 24 * scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter);
        painter.end();
        pixmap.setDevicePixelRatio(scale);
        icon.addPixmap(pixmap);
        icon.addPixmap(pixmap, QIcon::Selected);
    }
    cache.insert(name, icon);
    return icon;
}

IconPicker::IconPicker(QWidget *parent, const QString &selected) : QPushButton(parent), name(selected)
{
    setAccessibleName(QStringLiteral("Иконка"));
    setIconSize(QSize(24, 24));
    setMinimumHeight(36);
    setCursor(Qt::PointingHandCursor);
    updatePreview();
    connect(this, &QPushButton::clicked, this, [this] { choose(); });
}

void IconPicker::updatePreview()
{
    setText(name.isEmpty() ? QStringLiteral("Выбрать иконку…") : name);
    setIcon(siteIcon(name));
}

void IconPicker::choose()
{
    QMenu menu(this);
    menu.setWindowFlag(Qt::FramelessWindowHint);
    menu.setAttribute(Qt::WA_TranslucentBackground);
    auto *panel = new QWidget(&menu);
    panel->setMinimumSize(520, 420);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    auto *search = new QLineEdit(panel);
    search->setPlaceholderText(QStringLiteral("Поиск по названию Bootstrap"));
    search->setClearButtonEnabled(true);
    search->setAccessibleName(QStringLiteral("Поиск иконок"));
    layout->addWidget(search);
    auto *view = new QListView(panel);
    view->setAccessibleName(QStringLiteral("Bootstrap Icons"));
    view->setViewMode(QListView::IconMode);
    view->setMovement(QListView::Static);
    view->setResizeMode(QListView::Adjust);
    view->setWrapping(true);
    view->setGridSize(QSize(92, 68));
    view->setIconSize(QSize(24, 24));
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *model = new IconListModel(view);
    view->setModel(model);
    layout->addWidget(view, 1);
    auto *count = new QLabel(panel);
    layout->addWidget(count);
    const auto updateCount = [count, model] {
        count->setText(QStringLiteral("Иконок: %1").arg(model->rowCount()));
    };
    updateCount();
    connect(search, &QLineEdit::textChanged, panel, [model, updateCount](const QString &text) {
        model->search(text);
        updateCount();
    });
    auto *clear = new QPushButton(QStringLiteral("Без иконки"), panel);
    layout->addWidget(clear);
    connect(clear, &QPushButton::clicked, &menu, [&] {
        name.clear();
        menu.close();
    });
    connect(view, &QListView::clicked, &menu, [&](const QModelIndex &index) {
        name = index.data(Qt::DisplayRole).toString();
        menu.close();
    });
    auto *action = new QWidgetAction(&menu);
    action->setDefaultWidget(panel);
    menu.addAction(action);
    search->setFocus();
    menu.exec(mapToGlobal(QPoint(0, height())));
    updatePreview();
}

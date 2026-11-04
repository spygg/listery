#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class AppSettings;
class PluginHost;
class QCheckBox;
class QKeySequenceEdit;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QWidget;

/**
 * 插件管理对话框：左侧插件列表，右侧是当前插件的通用设置。
 *
 * 通用设置（启用、全局热键）写回 AppSettings 并同步给 PluginHost；
 * 插件自己的配置项由插件提供的 configure() 界面负责。
 * 保存后发出 pluginsChanged()，让 AppController 重新注册全局热键并刷新托盘菜单。
 */
class PluginManagerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PluginManagerDialog(AppSettings *settings, PluginHost *plugins,
                                 QWidget *parent = nullptr);

signals:
    void pluginsChanged();

private:
    struct Page
    {
        QString id;
        QCheckBox *enabled = nullptr;
        QKeySequenceEdit *hotkey = nullptr;
        QLabel *status = nullptr;
        QPushButton *configure = nullptr;
    };

    void buildUi();
    void buildPage(const QString &id);
    void refreshStatus(Page &page);
    bool saveValues();
    void openPluginConfig(const QString &id);

    AppSettings *m_settings = nullptr;
    PluginHost *m_plugins = nullptr;

    QListWidget *m_list = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_directoryLabel = nullptr;
    QVector<Page> m_pageWidgets;
    QVector<QString> m_ids;
};

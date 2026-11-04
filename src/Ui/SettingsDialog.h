#pragma once

#include <QDialog>
#include <QString>

class AppSettings;
class PluginHost;
class QCheckBox;
class QComboBox;
class QKeySequenceEdit;
class QLineEdit;
class QSpinBox;
class QVBoxLayout;

/** 设置面板：索引规则、热键间隔、结果数量与重建索引入口。插件管理在独立对话框里。 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(AppSettings *settings, PluginHost *plugins = nullptr,
                            QWidget *parent = nullptr);

signals:
    void rescanRequested();
    /** 插件管理对话框里改过启用状态 / 热键，需要重新注册全局热键。 */
    void pluginsChanged();

private:
    void buildUi();
    void buildPluginEntry(QVBoxLayout *mainLayout);
    void loadValues();
    void saveValues();
    void openPluginManager();

    AppSettings *m_settings = nullptr;
    PluginHost *m_plugins = nullptr;

    QSpinBox *m_hotkeyInterval = nullptr;
    QSpinBox *m_maxResults = nullptr;
    QSpinBox *m_rescanMinutes = nullptr;
    QCheckBox *m_autoScan = nullptr;
    QCheckBox *m_includeRemovable = nullptr;
    QCheckBox *m_smartSchedule = nullptr;
    QCheckBox *m_autoStart = nullptr;
    QCheckBox *m_hideOnFocusLost = nullptr;
    QLineEdit *m_extensions = nullptr;
    QLineEdit *m_skipDirectories = nullptr;
    QComboBox *m_language = nullptr;
#ifdef Q_OS_LINUX
    QCheckBox *m_useLocate = nullptr;
#endif
};

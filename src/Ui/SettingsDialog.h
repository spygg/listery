#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class AppSettings;
class PluginHost;
class QCheckBox;
class QKeySequenceEdit;
class QLineEdit;
class QSpinBox;
class QVBoxLayout;

/** 设置面板：索引规则、插件管理、热键间隔、结果数量与重建索引入口。 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(AppSettings *settings, PluginHost *plugins = nullptr,
                            QWidget *parent = nullptr);

signals:
    void rescanRequested();

private:
    void buildUi();
    void buildPluginGroup(QVBoxLayout *mainLayout);
    void loadValues();
    void saveValues();

    struct PluginWidgets
    {
        QString id;
        QCheckBox *enabled = nullptr;
        QKeySequenceEdit *hotkey = nullptr;
    };

    AppSettings *m_settings = nullptr;
    PluginHost *m_plugins = nullptr;
    QVector<PluginWidgets> m_pluginWidgets;

    QSpinBox *m_hotkeyInterval = nullptr;
    QSpinBox *m_maxResults = nullptr;
    QSpinBox *m_rescanMinutes = nullptr;
    QCheckBox *m_autoScan = nullptr;
    QCheckBox *m_includeRemovable = nullptr;
    QCheckBox *m_autoStart = nullptr;
    QCheckBox *m_hideOnFocusLost = nullptr;
    QCheckBox *m_smartSchedule = nullptr;
    QCheckBox *m_useLocate = nullptr;
    QLineEdit *m_extensions = nullptr;
    QLineEdit *m_skipDirectories = nullptr;
};

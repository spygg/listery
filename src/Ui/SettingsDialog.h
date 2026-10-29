#pragma once

#include <QDialog>

class AppSettings;
class QCheckBox;
class QLineEdit;
class QSpinBox;

/** 设置面板：索引规则、热键间隔、结果数量与重建索引入口。 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(AppSettings *settings, QWidget *parent = nullptr);

signals:
    void rescanRequested();

private:
    void buildUi();
    void loadValues();
    void saveValues();

    AppSettings *m_settings = nullptr;

    QSpinBox *m_hotkeyInterval = nullptr;
    QSpinBox *m_maxResults = nullptr;
    QSpinBox *m_rescanMinutes = nullptr;
    QCheckBox *m_autoScan = nullptr;
    QCheckBox *m_includeRemovable = nullptr;
    QCheckBox *m_autoStart = nullptr;
    QLineEdit *m_extensions = nullptr;
    QLineEdit *m_skipDirectories = nullptr;
};

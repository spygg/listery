#include "Ui/SettingsDialog.h"

#include "Core/AppSettings.h"
#include "Core/PluginHost.h"
#include "Core/QtCompat.h"
#include "Platform/HotkeyHook.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QStringList splitList(const QString &text)
{
    QStringList parts;
    static const QRegularExpression separators(QStringLiteral("[,，;；\\s]+"));
    for (const QString &part : text.split(separators)) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty())
            parts.append(trimmed);
    }
    return parts;
}
} // namespace

SettingsDialog::SettingsDialog(AppSettings *settings, PluginHost *plugins, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_plugins(plugins)
{
    setWindowTitle(QStringLiteral("Listery 设置"));
    listerySetWindowFlag(this, Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(520);

    buildUi();
    loadValues();
}

void SettingsDialog::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 18, 20, 16);
    mainLayout->setSpacing(14);

    auto *indexGroup = new QGroupBox(QStringLiteral("索引"), this);
    auto *indexForm = new QFormLayout(indexGroup);
    indexForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_extensions = new QLineEdit(indexGroup);
    m_extensions->setPlaceholderText(QStringLiteral("exe, com, bat, cmd, sh, ps1（lnk/url 快捷方式始终索引）"));
    indexForm->addRow(QStringLiteral("可执行后缀"), m_extensions);

    m_skipDirectories = new QLineEdit(indexGroup);
    m_skipDirectories->setPlaceholderText(QStringLiteral("WinSxS, node_modules, Temp"));
    indexForm->addRow(QStringLiteral("跳过目录"), m_skipDirectories);

    m_includeRemovable = new QCheckBox(QStringLiteral("索引可移动磁盘（U 盘、移动硬盘）"), indexGroup);
    indexForm->addRow(QString(), m_includeRemovable);

    m_autoScan = new QCheckBox(QStringLiteral("启动时自动更新索引（每天最多一次全盘扫描）"), indexGroup);
    indexForm->addRow(QString(), m_autoScan);

    m_smartSchedule = new QCheckBox(
        QStringLiteral("智能调度：可执行程序很少的分区自动降低扫描频率"), indexGroup);
    m_smartSchedule->setToolTip(QStringLiteral(
        "有程序的分区按下面的间隔刷新；只有零星几个的放宽到 2 倍；"
        "一个都没有的从 4 倍起步、每空一轮再翻倍，最多放宽到 14 天。"));
    indexForm->addRow(QString(), m_smartSchedule);

    m_useLocate = new QCheckBox(
        QStringLiteral("Linux 下优先用 locate 数据库建索引（秒级完成）"), indexGroup);
    m_useLocate->setToolTip(QStringLiteral(
        "读取 mlocate / plocate 的快照而不是遍历整个文件系统；"
        "locate 库由系统每天更新一次，因此新建的程序最多晚一天进入索引。"));
    indexForm->addRow(QString(), m_useLocate);

    m_rescanMinutes = new QSpinBox(indexGroup);
    m_rescanMinutes->setRange(0, 10080);
    m_rescanMinutes->setSuffix(QStringLiteral(" 分钟"));
    m_rescanMinutes->setSpecialValueText(QStringLiteral("不自动刷新"));
    indexForm->addRow(QStringLiteral("定时刷新间隔"), m_rescanMinutes);

    mainLayout->addWidget(indexGroup);

    auto *uiGroup = new QGroupBox(QStringLiteral("交互"), this);
    auto *uiForm = new QFormLayout(uiGroup);
    uiForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_hotkeyInterval = new QSpinBox(uiGroup);
    m_hotkeyInterval->setRange(150, 800);
    m_hotkeyInterval->setSingleStep(20);
    m_hotkeyInterval->setSuffix(QStringLiteral(" 毫秒"));
    uiForm->addRow(QStringLiteral("双击 Ctrl 间隔"), m_hotkeyInterval);

    m_maxResults = new QSpinBox(uiGroup);
    m_maxResults->setRange(5, 30);
    uiForm->addRow(QStringLiteral("最大候选数"), m_maxResults);

    m_hideOnFocusLost = new QCheckBox(QStringLiteral("窗口失去焦点时自动隐藏"), uiGroup);
    uiForm->addRow(QString(), m_hideOnFocusLost);

    m_autoStart = new QCheckBox(QStringLiteral("开机自启动（登录后静默运行，默认开启）"), uiGroup);
    uiForm->addRow(QString(), m_autoStart);

    mainLayout->addWidget(uiGroup);

    buildPluginGroup(mainLayout);

    auto *dataRow = new QHBoxLayout();
    auto *dataLabel = new QLabel(QStringLiteral("数据目录：%1").arg(m_settings->dataDir()), this);
    dataLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dataLabel->setWordWrap(true);
    dataRow->addWidget(dataLabel, 1);

    auto *openDataButton = new QPushButton(QStringLiteral("打开"), this);
    connect(openDataButton, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_settings->dataDir()));
    });
    dataRow->addWidget(openDataButton);
    mainLayout->addLayout(dataRow);

    auto *buttonBox = new QDialogButtonBox(this);
    auto *rescanButton = new QPushButton(QStringLiteral("立即重建索引"), buttonBox);
    rescanButton->setToolTip(QStringLiteral("重新遍历所有磁盘，耗时取决于磁盘规模"));
    buttonBox->addButton(rescanButton, QDialogButtonBox::ActionRole);
    auto *saveButton = new QPushButton(QStringLiteral("保存"), buttonBox);
    saveButton->setDefault(true);
    buttonBox->addButton(saveButton, QDialogButtonBox::AcceptRole);
    auto *cancelButton = new QPushButton(QStringLiteral("取消"), buttonBox);
    buttonBox->addButton(cancelButton, QDialogButtonBox::RejectRole);

    connect(saveButton, &QPushButton::clicked, this, [this]() {
        saveValues();
        accept();
    });
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(rescanButton, &QPushButton::clicked, this, [this]() {
        saveValues();
        emit rescanRequested();
        accept();
    });

    mainLayout->addWidget(buttonBox);
}

void SettingsDialog::buildPluginGroup(QVBoxLayout *mainLayout)
{
    auto *pluginGroup = new QGroupBox(QStringLiteral("插件"), this);
    auto *pluginLayout = new QVBoxLayout(pluginGroup);
    pluginLayout->setSpacing(8);

    const QString pluginDir = m_settings->pluginDir();
    auto *dirLabel = new QLabel(QStringLiteral("插件目录：%1").arg(pluginDir), pluginGroup);
    dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dirLabel->setWordWrap(true);
    pluginLayout->addWidget(dirLabel);

    const QVector<PluginInfo> plugins = m_plugins ? m_plugins->plugins() : QVector<PluginInfo>();
    if (plugins.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("未发现插件。把 .dll / .so 放进上面的目录，重启 Listery 即可加载。"),
                                 pluginGroup);
        empty->setWordWrap(true);
        empty->setStyleSheet(QStringLiteral("color: #8A919B;"));
        pluginLayout->addWidget(empty);
        mainLayout->addWidget(pluginGroup);
        return;
    }

    for (const PluginInfo &info : plugins) {
        auto *row = new QHBoxLayout();
        auto *enabled = new QCheckBox(info.name, pluginGroup);
        enabled->setChecked(info.enabled && info.loaded);
        enabled->setEnabled(info.loaded);
        enabled->setToolTip(info.loaded
                                ? info.filePath
                                : QStringLiteral("加载失败：%1").arg(info.error));
        row->addWidget(enabled, 1);

        auto *version = new QLabel(info.loaded ? info.version : QStringLiteral("加载失败"), pluginGroup);
        version->setStyleSheet(QStringLiteral("color: #8A919B;"));
        row->addWidget(version);

        auto *hotkey = new QKeySequenceEdit(QKeySequence::fromString(info.hotkey, QKeySequence::PortableText),
                                            pluginGroup);
        hotkey->setMaximumWidth(110);
        hotkey->setToolTip(QStringLiteral("留空表示不占用全局热键（仍可从托盘菜单进入）"));
        row->addWidget(hotkey);

        if (info.hasConfigUi && info.loaded) {
            auto *configure = new QPushButton(QStringLiteral("配置…"), pluginGroup);
            const QString pluginId = info.id;
            connect(configure, &QPushButton::clicked, this, [this, pluginId]() {
                if (m_plugins)
                    m_plugins->configure(pluginId, reinterpret_cast<void *>(winId()));
            });
            row->addWidget(configure);
        }

        pluginLayout->addLayout(row);

        if (!info.description.isEmpty()) {
            auto *description = new QLabel(info.description, pluginGroup);
            description->setWordWrap(true);
            description->setStyleSheet(QStringLiteral("color: #8A919B; margin-left: 22px;"));
            pluginLayout->addWidget(description);
        }
        if (!info.loaded && !info.error.isEmpty()) {
            auto *reason = new QLabel(info.error, pluginGroup);
            reason->setWordWrap(true);
            reason->setStyleSheet(QStringLiteral("color: #C0504D; margin-left: 22px;"));
            pluginLayout->addWidget(reason);
        }

        m_pluginWidgets.append(PluginWidgets{info.id, enabled, hotkey});
    }

    mainLayout->addWidget(pluginGroup);
}

void SettingsDialog::loadValues()
{
    m_hotkeyInterval->setValue(m_settings->hotkeyIntervalMs());
    m_maxResults->setValue(m_settings->maxResults());
    m_rescanMinutes->setValue(m_settings->rescanIntervalMinutes());
    m_autoScan->setChecked(m_settings->autoScanOnStart());
    m_includeRemovable->setChecked(m_settings->includeRemovableDrives());
    m_extensions->setText(m_settings->executableExtensions().join(QStringLiteral(", ")));
    m_skipDirectories->setText(m_settings->skipDirectoryPatterns().join(QStringLiteral(", ")));
    m_smartSchedule->setChecked(m_settings->smartSchedule());
    m_useLocate->setChecked(m_settings->useLocateIndex());
    m_autoStart->setChecked(m_settings->autoStart());
    m_hideOnFocusLost->setChecked(m_settings->hideWhenFocusLost());
}

void SettingsDialog::saveValues()
{
    QStringList extensions = splitList(m_extensions->text());
    if (extensions.isEmpty())
        extensions = QStringList() << QStringLiteral("exe");
    m_settings->setExecutableExtensions(extensions);

    m_settings->setSkipDirectoryPatterns(splitList(m_skipDirectories->text()));
    m_settings->setIncludeRemovableDrives(m_includeRemovable->isChecked());
    m_settings->setAutoStart(m_autoStart->isChecked());
    m_settings->setAutoScanOnStart(m_autoScan->isChecked());
    m_settings->setHideWhenFocusLost(m_hideOnFocusLost->isChecked());
    m_settings->setRescanIntervalMinutes(m_rescanMinutes->value());
    m_settings->setSmartSchedule(m_smartSchedule->isChecked());
    m_settings->setUseLocateIndex(m_useLocate->isChecked());
    m_settings->setHotkeyIntervalMs(m_hotkeyInterval->value());
    m_settings->setMaxResults(m_maxResults->value());

    // 插件：写回设置并同步给宿主（热键由 AppController 在对话框关闭后重新注册）
    for (const PluginWidgets &widget : m_pluginWidgets) {
        const bool enabled = widget.enabled->isChecked();
        m_settings->setPluginEnabled(widget.id, enabled);

        QString hotkey = widget.hotkey->keySequence().toString(QKeySequence::PortableText);
        const QString normalized = normalizeHotkey(hotkey);
        if (!hotkey.isEmpty() && normalized.isEmpty()) {
            // 用户按了无法识别的组合：保留插件默认值，避免注册出一个永远触发不了的热键
            hotkey = m_plugins ? m_plugins->info(widget.id).defaultHotkey : QString();
        } else {
            hotkey = normalized;
        }
        m_settings->setPluginHotkey(widget.id, hotkey);
        if (m_plugins) {
            m_plugins->setEnabled(widget.id, enabled);
            m_plugins->setHotkey(widget.id, hotkey.isEmpty()
                                                 ? m_plugins->info(widget.id).defaultHotkey
                                                 : hotkey);
        }
    }
}

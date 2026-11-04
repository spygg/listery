#include "Ui/SettingsDialog.h"

#include "Core/AppSettings.h"
#include "Core/Localization.h"
#include "Core/PluginHost.h"
#include "Core/QtCompat.h"
#include "Ui/PluginManagerDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
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
    setWindowTitle(tr("Listery 设置"));
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

    auto *indexGroup = new QGroupBox(tr("索引"), this);
    auto *indexForm = new QFormLayout(indexGroup);
    indexForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_extensions = new QLineEdit(indexGroup);
    m_extensions->setPlaceholderText(tr("exe, com, bat, cmd, sh, ps1（lnk/url 快捷方式始终索引）"));
    indexForm->addRow(tr("可执行后缀"), m_extensions);

    m_skipDirectories = new QLineEdit(indexGroup);
    m_skipDirectories->setPlaceholderText(QStringLiteral("WinSxS, node_modules, Temp"));
    indexForm->addRow(tr("跳过目录"), m_skipDirectories);

    m_includeRemovable = new QCheckBox(tr("索引可移动磁盘（U 盘、移动硬盘）"), indexGroup);
    indexForm->addRow(QString(), m_includeRemovable);

    m_autoScan = new QCheckBox(tr("启动时自动更新索引（每天最多一次全盘扫描）"), indexGroup);
    indexForm->addRow(QString(), m_autoScan);

    m_smartSchedule = new QCheckBox(
        tr("智能调度：可执行程序很少的分区自动降低扫描频率"), indexGroup);
    m_smartSchedule->setToolTip(tr(
        "有程序的分区按下面的间隔刷新；只有零星几个的放宽到 2 倍；"
        "一个都没有的从 4 倍起步、每空一轮再翻倍，最多放宽到 14 天。"));
    indexForm->addRow(QString(), m_smartSchedule);

#ifdef Q_OS_LINUX
    // locate 是 Linux 专属的快速通道（读取 mlocate/plocate 快照），Windows/macOS 根本没有，
    // 在这些平台上把这个选项藏掉，免得用户看到一条用不上的设置。
    m_useLocate = new QCheckBox(
        tr("优先用 locate 数据库建索引（秒级完成）"), indexGroup);
    m_useLocate->setToolTip(tr(
        "读取 mlocate / plocate 的快照而不是遍历整个文件系统；"
        "locate 库由系统每天更新一次，因此新建的程序最多晚一天进入索引。"));
    indexForm->addRow(QString(), m_useLocate);
#endif

    m_rescanMinutes = new QSpinBox(indexGroup);
    m_rescanMinutes->setRange(0, 10080);
    m_rescanMinutes->setSuffix(tr(" 分钟"));
    m_rescanMinutes->setSpecialValueText(tr("不自动刷新"));
    indexForm->addRow(tr("定时刷新间隔"), m_rescanMinutes);

    mainLayout->addWidget(indexGroup);

    auto *uiGroup = new QGroupBox(tr("交互"), this);
    auto *uiForm = new QFormLayout(uiGroup);
    uiForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_language = new QComboBox(uiGroup);
    for (const QString &code : Localization::availableLanguages())
        m_language->addItem(Localization::nativeName(code), code);
    uiForm->addRow(tr("界面语言"), m_language);

    m_hotkeyInterval = new QSpinBox(uiGroup);
    m_hotkeyInterval->setRange(150, 800);
    m_hotkeyInterval->setSingleStep(20);
    m_hotkeyInterval->setSuffix(tr(" 毫秒"));
    uiForm->addRow(tr("双击 Ctrl 间隔"), m_hotkeyInterval);

    m_maxResults = new QSpinBox(uiGroup);
    m_maxResults->setRange(5, 30);
    uiForm->addRow(tr("最大候选数"), m_maxResults);

    m_hideOnFocusLost = new QCheckBox(tr("窗口失去焦点时自动隐藏"), uiGroup);
    uiForm->addRow(QString(), m_hideOnFocusLost);

    m_autoStart = new QCheckBox(tr("开机自启动（登录后静默运行，默认开启）"), uiGroup);
    uiForm->addRow(QString(), m_autoStart);

    mainLayout->addWidget(uiGroup);

    buildPluginEntry(mainLayout);

    auto *dataRow = new QHBoxLayout();
    auto *dataLabel = new QLabel(tr("数据目录：%1").arg(m_settings->dataDir()), this);
    dataLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dataLabel->setWordWrap(true);
    dataRow->addWidget(dataLabel, 1);

    auto *openDataButton = new QPushButton(tr("打开"), this);
    connect(openDataButton, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_settings->dataDir()));
    });
    dataRow->addWidget(openDataButton);
    mainLayout->addLayout(dataRow);

    auto *buttonBox = new QDialogButtonBox(this);
    auto *rescanButton = new QPushButton(tr("立即重建索引"), buttonBox);
    rescanButton->setToolTip(tr("重新遍历所有磁盘，耗时取决于磁盘规模"));
    buttonBox->addButton(rescanButton, QDialogButtonBox::ActionRole);
    auto *saveButton = new QPushButton(tr("保存"), buttonBox);
    saveButton->setDefault(true);
    buttonBox->addButton(saveButton, QDialogButtonBox::AcceptRole);
    auto *cancelButton = new QPushButton(tr("取消"), buttonBox);
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

void SettingsDialog::buildPluginEntry(QVBoxLayout *mainLayout)
{
    auto *pluginGroup = new QGroupBox(tr("插件"), this);
    auto *pluginLayout = new QHBoxLayout(pluginGroup);
    pluginLayout->setSpacing(10);

    const int count = m_plugins ? m_plugins->plugins().size() : 0;
    auto *summary = new QLabel(
        count == 0 ? tr("未发现插件。把 .dll / .so / .dylib 放进插件目录后重启 Listery。")
                   : tr("已发现 %1 个插件，可在这里逐个启用、修改热键或打开插件设置。").arg(count),
        pluginGroup);
    summary->setWordWrap(true);
    pluginLayout->addWidget(summary, 1);

    auto *manage = new QPushButton(tr("插件管理…"), pluginGroup);
    manage->setEnabled(m_plugins != nullptr);
    connect(manage, &QPushButton::clicked, this, &SettingsDialog::openPluginManager);
    pluginLayout->addWidget(manage);

    mainLayout->addWidget(pluginGroup);
}

void SettingsDialog::openPluginManager()
{
    if (!m_plugins)
        return;

    PluginManagerDialog dialog(m_settings, m_plugins, this);
    listerySetWindowFlag(&dialog, Qt::WindowStaysOnTopHint, true);
    connect(&dialog, &PluginManagerDialog::pluginsChanged, this,
            &SettingsDialog::pluginsChanged);
    dialog.exec();
}

void SettingsDialog::loadValues()
{
    const int languageIndex = m_language->findData(m_settings->language());
    m_language->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);
    m_hotkeyInterval->setValue(m_settings->hotkeyIntervalMs());
    m_maxResults->setValue(m_settings->maxResults());
    m_rescanMinutes->setValue(m_settings->rescanIntervalMinutes());
    m_autoScan->setChecked(m_settings->autoScanOnStart());
    m_includeRemovable->setChecked(m_settings->includeRemovableDrives());
    m_extensions->setText(m_settings->executableExtensions().join(QStringLiteral(", ")));
    m_skipDirectories->setText(m_settings->skipDirectoryPatterns().join(QStringLiteral(", ")));
    m_smartSchedule->setChecked(m_settings->smartSchedule());
#ifdef Q_OS_LINUX
    if (m_useLocate)
        m_useLocate->setChecked(m_settings->useLocateIndex());
#endif
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
#ifdef Q_OS_LINUX
    if (m_useLocate)
        m_settings->setUseLocateIndex(m_useLocate->isChecked());
#endif
    m_settings->setHotkeyIntervalMs(m_hotkeyInterval->value());
    m_settings->setMaxResults(m_maxResults->value());
    m_settings->setLanguage(m_language->currentData().toString());
}

#include "Ui/SettingsDialog.h"

#include "Core/AppSettings.h"

#include <QCheckBox>
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

SettingsDialog::SettingsDialog(AppSettings *settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("Listery 设置"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
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
    m_extensions->setPlaceholderText(QStringLiteral("exe, com, bat, cmd, sh, ps1"));
    indexForm->addRow(QStringLiteral("可执行后缀"), m_extensions);

    m_skipDirectories = new QLineEdit(indexGroup);
    m_skipDirectories->setPlaceholderText(QStringLiteral("WinSxS, node_modules, Temp"));
    indexForm->addRow(QStringLiteral("跳过目录"), m_skipDirectories);

    m_includeRemovable = new QCheckBox(QStringLiteral("索引可移动磁盘（U 盘、移动硬盘）"), indexGroup);
    indexForm->addRow(QString(), m_includeRemovable);

    m_autoScan = new QCheckBox(QStringLiteral("启动时自动重建索引"), indexGroup);
    indexForm->addRow(QString(), m_autoScan);

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

    m_autoStart = new QCheckBox(QStringLiteral("开机自启动（登录后静默运行，默认开启）"), uiGroup);
    uiForm->addRow(QString(), m_autoStart);

    mainLayout->addWidget(uiGroup);

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

void SettingsDialog::loadValues()
{
    m_hotkeyInterval->setValue(m_settings->hotkeyIntervalMs());
    m_maxResults->setValue(m_settings->maxResults());
    m_rescanMinutes->setValue(m_settings->rescanIntervalMinutes());
    m_autoScan->setChecked(m_settings->autoScanOnStart());
    m_includeRemovable->setChecked(m_settings->includeRemovableDrives());
    m_extensions->setText(m_settings->executableExtensions().join(QStringLiteral(", ")));
    m_skipDirectories->setText(m_settings->skipDirectoryPatterns().join(QStringLiteral(", ")));
    m_autoStart->setChecked(m_settings->autoStart());
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
    m_settings->setRescanIntervalMinutes(m_rescanMinutes->value());
    m_settings->setHotkeyIntervalMs(m_hotkeyInterval->value());
    m_settings->setMaxResults(m_maxResults->value());
}

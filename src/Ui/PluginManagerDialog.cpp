#include "Ui/PluginManagerDialog.h"

#include "Core/AppSettings.h"
#include "Core/PluginHost.h"
#include "Core/QtCompat.h"
#include "Platform/HotkeyHook.h"

#include <QCheckBox>
#include <QColor>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QLabel *mutedLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: #8A919B;"));
    return label;
}

} // namespace

PluginManagerDialog::PluginManagerDialog(AppSettings *settings, PluginHost *plugins,
                                         QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_plugins(plugins)
{
    setWindowTitle(tr("插件管理"));
    listerySetWindowFlag(this, Qt::WindowContextHelpButtonHint, false);
    setMinimumSize(760, 520);

    buildUi();
}

void PluginManagerDialog::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(18, 16, 18, 14);
    mainLayout->setSpacing(12);

    auto *body = new QHBoxLayout();
    body->setSpacing(14);

    m_list = new QListWidget(this);
    m_list->setFixedWidth(200);
    m_list->setAlternatingRowColors(true);
    body->addWidget(m_list);

    m_pages = new QStackedWidget(this);
    body->addWidget(m_pages, 1);
    mainLayout->addLayout(body, 1);

    const QVector<PluginInfo> plugins = m_plugins ? m_plugins->plugins() : QVector<PluginInfo>();
    if (plugins.isEmpty()) {
        auto *empty = new QWidget(this);
        auto *emptyLayout = new QVBoxLayout(empty);
        auto *title = new QLabel(tr("未发现插件"), empty);
        emptyLayout->addWidget(title);
        emptyLayout->addWidget(mutedLabel(
            tr("把 .dll / .so / .dylib 放进插件目录，重启 Listery 后即可在这里管理。"), empty));
        emptyLayout->addStretch(1);
        m_pages->addWidget(empty);
    } else {
        for (const PluginInfo &info : plugins)
            buildPage(info.id);
        m_list->setCurrentRow(0);
    }

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < m_pages->count())
            m_pages->setCurrentIndex(row);
    });

    auto *dirRow = new QHBoxLayout();
    m_directoryLabel = mutedLabel(QString(), this);
    m_directoryLabel->setText(tr("插件目录：%1").arg(m_settings ? m_settings->pluginDir() : QString()));
    m_directoryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dirRow->addWidget(m_directoryLabel, 1);

    auto *openDir = new QPushButton(tr("打开目录"), this);
    connect(openDir, &QPushButton::clicked, this, [this]() {
        if (m_settings)
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_settings->pluginDir()));
    });
    dirRow->addWidget(openDir);
    mainLayout->addLayout(dirRow);

    auto *buttons = new QDialogButtonBox(this);
    auto *saveButton = new QPushButton(tr("保存"), buttons);
    saveButton->setDefault(true);
    buttons->addButton(saveButton, QDialogButtonBox::AcceptRole);
    auto *cancelButton = new QPushButton(tr("取消"), buttons);
    buttons->addButton(cancelButton, QDialogButtonBox::RejectRole);
    connect(saveButton, &QPushButton::clicked, this, [this]() {
        saveValues();
        accept();
    });
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    mainLayout->addWidget(buttons);
}

void PluginManagerDialog::buildPage(const QString &id)
{
    const PluginInfo info = m_plugins->info(id);

    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *title = new QLabel(info.name.isEmpty() ? id : info.name, page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    layout->addWidget(title);

    Page widgets;
    widgets.id = id;

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    widgets.enabled = new QCheckBox(tr("启用该插件"), page);
    widgets.enabled->setChecked(m_settings ? m_settings->pluginEnabled(id) : info.enabled);
    widgets.enabled->setEnabled(info.loaded);
    if (!info.loaded) {
        widgets.enabled->setToolTip(
            info.error.isEmpty() ? tr("插件未加载，无法启用") : tr("加载失败：%1").arg(info.error));
    }
    form->addRow(QString(), widgets.enabled);

    const QString currentHotkey = m_settings ? m_settings->pluginHotkey(id) : QString();
    widgets.hotkey = new QKeySequenceEdit(
        QKeySequence::fromString(currentHotkey.isEmpty() ? info.hotkey : currentHotkey,
                                 QKeySequence::PortableText),
        page);
    widgets.hotkey->setToolTip(tr("留空表示不占用全局热键（仍可从托盘菜单进入）"));
    widgets.hotkey->setMaximumWidth(180);
    form->addRow(tr("全局热键"), widgets.hotkey);

    layout->addLayout(form);

    auto *actions = new QHBoxLayout();
    widgets.configure = new QPushButton(tr("插件设置…"), page);
    widgets.configure->setEnabled(info.loaded && info.hasConfigUi);
    if (!info.hasConfigUi)
        widgets.configure->setToolTip(tr("该插件没有提供独立设置界面"));
    connect(widgets.configure, &QPushButton::clicked, this,
            [this, id]() { openPluginConfig(id); });
    actions->addWidget(widgets.configure);

    widgets.status = mutedLabel(QString(), page);
    actions->addWidget(widgets.status, 1);
    layout->addLayout(actions);

    if (!info.description.isEmpty())
        layout->addWidget(mutedLabel(info.description, page));

    layout->addWidget(mutedLabel(tr("版本：%1").arg(info.version.isEmpty() ? tr("未知")
                                                                         : info.version),
                                 page));
    layout->addWidget(mutedLabel(tr("文件：%1").arg(info.filePath), page));
    layout->addStretch(1);

    m_pages->addWidget(page);
    m_pageWidgets.append(widgets);
    m_ids.append(id);

    auto *item = new QListWidgetItem(info.loaded ? info.name : tr("%1（未加载）").arg(info.name),
                                     m_list);
    item->setToolTip(info.error.isEmpty() ? info.filePath : info.error);
    if (!info.loaded)
        item->setForeground(QColor(QStringLiteral("#C0504D")));

    refreshStatus(m_pageWidgets.last());

    // 勾选/取消时同步左侧列表的状态文字，保存前就能看到变化。
    connect(widgets.enabled, &QCheckBox::toggled, this, [this, id]() {
        const int index = m_ids.indexOf(id);
        if (index < 0 || index >= m_list->count())
            return;
        QString label = m_plugins ? m_plugins->info(id).name : id;
        if (index < m_pageWidgets.size() && !m_pageWidgets[index].enabled->isChecked())
            label = tr("%1（已停用）").arg(label);
        m_list->item(index)->setText(label);
    });
}

void PluginManagerDialog::refreshStatus(Page &page)
{
    if (!m_plugins || page.status == nullptr)
        return;
    const PluginInfo info = m_plugins->info(page.id);
    if (info.loaded) {
        page.status->setText(tr("已加载"));
        page.status->setStyleSheet(QStringLiteral("color: #6FA86F;"));
    } else {
        page.status->setText(info.error.isEmpty() ? tr("未加载") : info.error);
        page.status->setStyleSheet(QStringLiteral("color: #C0504D;"));
    }
}

void PluginManagerDialog::openPluginConfig(const QString &id)
{
    if (!m_plugins || !m_plugins->hasPlugin(id))
        return;
    m_plugins->configure(id, reinterpret_cast<void *>(winId()));
}

void PluginManagerDialog::saveValues()
{
    if (!m_plugins)
        return;

    bool changed = false;
    for (const Page &page : m_pageWidgets) {
        const PluginInfo info = m_plugins->info(page.id);

        const bool enabled = page.enabled->isChecked();
        if (enabled != (m_settings ? m_settings->pluginEnabled(page.id) : info.enabled)) {
            if (m_settings)
                m_settings->setPluginEnabled(page.id, enabled);
            m_plugins->setEnabled(page.id, enabled);
            changed = true;
        }

        QString hotkey = page.hotkey->keySequence().toString(QKeySequence::PortableText);
        const QString normalized = normalizeHotkey(hotkey);
        if (!hotkey.isEmpty() && normalized.isEmpty()) {
            // 无法识别的组合：退回插件默认值，避免注册一个永远触发不了的热键。
            hotkey = info.defaultHotkey;
        } else {
            hotkey = normalized;
        }
        const QString stored = m_settings ? m_settings->pluginHotkey(page.id) : QString();
        if (hotkey != stored) {
            if (m_settings)
                m_settings->setPluginHotkey(page.id, hotkey);
            changed = true;
        }
        const QString effective =
            hotkey.isEmpty() ? info.defaultHotkey : hotkey;
        if (effective != info.hotkey) {
            m_plugins->setHotkey(page.id, effective);
            changed = true;
        }
    }

    if (changed)
        emit pluginsChanged();
}

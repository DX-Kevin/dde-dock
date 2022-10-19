/*
 * Copyright (C) 2019 ~ 2019 Deepin Technology Co., Ltd.
 *
 * Author:     wangshaojun <wangshaojun_cm@deepin.com>
 *
 * Maintainer: wangshaojun <wangshaojun_cm@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dockitemmanager.h"
#include "appitem.h"
#include "launcheritem.h"
#include "pluginsitem.h"
#include "traypluginitem.h"
#include "utils.h"
#include "appmultiitem.h"
#include "quicksettingcontroller.h"

#include <QDebug>
#include <QGSettings>

#include <DApplication>

DockItemManager *DockItemManager::INSTANCE = nullptr;
const QGSettings *DockItemManager::m_appSettings = Utils::ModuleSettingsPtr("app");
const QGSettings *DockItemManager::m_activeSettings = Utils::ModuleSettingsPtr("activeapp");
const QGSettings *DockItemManager::m_dockedSettings = Utils::ModuleSettingsPtr("dockapp");

DockItemManager::DockItemManager(QObject *parent)
    : QObject(parent)
    , m_appInter(new DockInter(dockServiceName(), dockServicePath(), QDBusConnection::sessionBus(), this))
    , m_loadFinished(false)
{
    //固定区域：启动器
    m_itemList.append(new LauncherItem);

    // 应用区域
    for (auto entry : m_appInter->entries()) {
        AppItem *it = new AppItem(m_appInter, m_appSettings, m_activeSettings, m_dockedSettings, entry);
        manageItem(it);

        connect(it, &AppItem::requestActivateWindow, m_appInter, &DockInter::ActivateWindow, Qt::QueuedConnection);
        connect(it, &AppItem::requestPreviewWindow, m_appInter, &DockInter::PreviewWindow);
        connect(it, &AppItem::requestCancelPreview, m_appInter, &DockInter::CancelPreviewWindow);

#ifdef USE_AM
        connect(it, &AppItem::windowCountChanged, this, &DockItemManager::onAppWindowCountChanged);
#endif

        connect(this, &DockItemManager::requestUpdateDockItem, it, &AppItem::requestUpdateEntryGeometries);

        m_itemList.append(it);
#ifdef USE_AM
        updateMultiItems(it);
#endif
    }

    // 托盘区域和插件区域 由DockPluginsController获取

    // 应用信号
    connect(m_appInter, &DockInter::EntryAdded, this, &DockItemManager::appItemAdded);
    connect(m_appInter, &DockInter::EntryRemoved, this, static_cast<void (DockItemManager::*)(const QString &)>(&DockItemManager::appItemRemoved), Qt::QueuedConnection);
    connect(m_appInter, &DockInter::ServiceRestarted, this, &DockItemManager::reloadAppItems);
#ifdef USE_AM
    connect(m_appInter, &DockInter::ShowMultiWindowChanged, this, &DockItemManager::onShowMultiWindowChanged);
#endif

    DApplication *app = qobject_cast<DApplication *>(qApp);
    if (app) {
        connect(app, &DApplication::iconThemeChanged, this, &DockItemManager::refreshItemsIcon);
    }

    connect(qApp, &QApplication::aboutToQuit, this, &QObject::deleteLater);

    // 刷新图标
    QMetaObject::invokeMethod(this, "refreshItemsIcon", Qt::QueuedConnection);
}

DockItemManager *DockItemManager::instance(QObject *parent)
{
    if (!INSTANCE)
        INSTANCE = new DockItemManager(parent);

    return INSTANCE;
}

const QList<QPointer<DockItem>> DockItemManager::itemList() const
{
    return m_itemList;
}

const QList<PluginsItemInterface *> DockItemManager::pluginList() const
{
    return QuickSettingController::instance()->pluginsMap().keys();
}

bool DockItemManager::appIsOnDock(const QString &appDesktop) const
{
    return m_appInter->IsOnDock(appDesktop);
}

void DockItemManager::refreshItemsIcon()
{
    for (auto item : m_itemList) {
        if (item.isNull())
            continue;

        item->refreshIcon();
        item->update();
    }
}

/**
 * @brief 将插件的参数(Order, Visible, etc)写入gsettings
 * 自动化测试需要通过dbus(GetPluginSettings)获取这些参数
 */
void DockItemManager::updatePluginsItemOrderKey()
{
    int index = 0;
    for (auto item : m_itemList) {
        if (item.isNull() || item->itemType() != DockItem::Plugins)
            continue;
        static_cast<PluginsItem *>(item.data())->setItemSortKey(++index);
    }

    // 固定区域插件排序
    index = 0;
    for (auto item : m_itemList) {
        if (item.isNull() || item->itemType() != DockItem::FixedPlugin)
            continue;
        static_cast<PluginsItem *>(item.data())->setItemSortKey(++index);
    }
}

void DockItemManager::itemMoved(DockItem *const sourceItem, DockItem *const targetItem)
{
    Q_ASSERT(sourceItem != targetItem);

    const DockItem::ItemType moveType = sourceItem->itemType();
    const DockItem::ItemType replaceType = targetItem->itemType();

    // app move
    if (moveType == DockItem::App || moveType == DockItem::Placeholder)
        if (replaceType != DockItem::App)
            return;

    // plugins move
    if (moveType == DockItem::Plugins || moveType == DockItem::TrayPlugin)
        if (replaceType != DockItem::Plugins && replaceType != DockItem::TrayPlugin)
            return;

    const int moveIndex = m_itemList.indexOf(sourceItem);
    const int replaceIndex = m_itemList.indexOf(targetItem);

    m_itemList.removeAt(moveIndex);
    m_itemList.insert(replaceIndex, sourceItem);

    // update plugins sort key if order changed
    if (moveType == DockItem::Plugins || replaceType == DockItem::Plugins
            || moveType == DockItem::TrayPlugin || replaceType == DockItem::TrayPlugin
            || moveType == DockItem::FixedPlugin || replaceType == DockItem::FixedPlugin) {
        updatePluginsItemOrderKey();
    }

    // for app move, index 0 is launcher item, need to pass it.
    if (moveType == DockItem::App && replaceType == DockItem::App)
        m_appInter->MoveEntry(moveIndex - 1, replaceIndex - 1);
}

void DockItemManager::itemAdded(const QString &appDesktop, int idx)
{
    m_appInter->RequestDock(appDesktop, idx);
}

void DockItemManager::appItemAdded(const QDBusObjectPath &path, const int index)
{
    // 第一个是启动器
    int insertIndex = 1;

    // -1 for append to app list end
    if (index != -1) {
        insertIndex += index;
    } else {
        for (auto item : m_itemList)
            if (!item.isNull() && item->itemType() == DockItem::App)
                ++insertIndex;
    }

    AppItem *item = new AppItem(m_appInter, m_appSettings, m_activeSettings, m_dockedSettings, path);

    if (m_appIDist.contains(item->appId())) {
        delete item;
        return;
    }

    manageItem(item);

    connect(item, &AppItem::requestActivateWindow, m_appInter, &DockInter::ActivateWindow, Qt::QueuedConnection);
    connect(item, &AppItem::requestPreviewWindow, m_appInter, &DockInter::PreviewWindow);
    connect(item, &AppItem::requestCancelPreview, m_appInter, &DockInter::CancelPreviewWindow);
#ifdef USE_AM
    connect(item, &AppItem::windowCountChanged, this, &DockItemManager::onAppWindowCountChanged);
#endif
    connect(this, &DockItemManager::requestUpdateDockItem, item, &AppItem::requestUpdateEntryGeometries);

    m_itemList.insert(insertIndex, item);
    m_appIDist.append(item->appId());

    int itemIndex = insertIndex;
    if (index != -1)
        itemIndex = insertIndex - 1;

    // 插入dockItem
    emit itemInserted(itemIndex, item);
#ifdef USE_AM
    // 向后插入多开窗口
    updateMultiItems(item, true);
#endif
}

void DockItemManager::appItemRemoved(const QString &appId)
{
    for (int i(0); i != m_itemList.size(); ++i) {
        AppItem *app = static_cast<AppItem *>(m_itemList[i].data());
        if (!app) {
            continue;
        }

        if (m_itemList[i]->itemType() != DockItem::App)
            continue;

        if (!app->isValid() || app->appId() == appId) {
            appItemRemoved(app);
            break;
        }
    }

    m_appIDist.removeAll(appId);
}

void DockItemManager::appItemRemoved(AppItem *appItem)
{
    emit itemRemoved(appItem);
    m_itemList.removeOne(appItem);

    if (appItem->isDragging()) {
        QDrag::cancel();
    }
    appItem->deleteLater();
}

void DockItemManager::reloadAppItems()
{
    // remove old item
    for (auto item : m_itemList)
        if (item->itemType() == DockItem::App)
            appItemRemoved(static_cast<AppItem *>(item.data()));

    // append new item
    for (auto path : m_appInter->entries())
        appItemAdded(path, -1);
}

void DockItemManager::manageItem(DockItem *item)
{
    connect(item, &DockItem::requestRefreshWindowVisible, this, &DockItemManager::requestRefershWindowVisible, Qt::UniqueConnection);
    connect(item, &DockItem::requestWindowAutoHide, this, &DockItemManager::requestWindowAutoHide, Qt::UniqueConnection);
}

void DockItemManager::onPluginLoadFinished()
{
    updatePluginsItemOrderKey();
    m_loadFinished = true;
}

#ifdef USE_AM
void DockItemManager::onAppWindowCountChanged()
{
    AppItem *appItem = static_cast<AppItem *>(sender());
    updateMultiItems(appItem, true);
}

void DockItemManager::updateMultiItems(AppItem *appItem, bool emitSignal)
{
    // 如果系统设置不开启应用多窗口拆分，则无需之后的操作
    if (!m_appInter->showMultiWindow())
        return;

    // 如果开启了多窗口拆分，则同步窗口和多窗口应用的信息
    const WindowInfoMap &windowInfoMap = appItem->windowsMap();
    QList<AppMultiItem *> removeItems;
    // 同步当前已经存在的多开窗口的列表，删除不存在的多开窗口
    for (int i = 0; i < m_itemList.size(); i++) {
        QPointer<DockItem> dockItem = m_itemList[i];
        AppMultiItem *multiItem = qobject_cast<AppMultiItem *>(dockItem.data());
        if (!multiItem || multiItem->appItem() != appItem)
            continue;

        // 如果查找到的当前的应用的窗口不需要移除，则继续下一个循环
        if (!needRemoveMultiWindow(multiItem))
            continue;

        removeItems << multiItem;
    }
    // 从itemList中移除多开窗口
    for (AppMultiItem *dockItem : removeItems)
        m_itemList.removeOne(dockItem);
    if (emitSignal) {
        // 移除发送每个多开窗口的移除信号
        for (AppMultiItem *dockItem : removeItems)
            Q_EMIT itemRemoved(dockItem);
    }
    qDeleteAll(removeItems);

    // 遍历当前APP打开的所有窗口的列表，如果不存在多开窗口的应用，则新增，同时发送信号
    for (auto it = windowInfoMap.begin(); it != windowInfoMap.end(); it++) {
        if (multiWindowExist(it.key()))
            continue;

        const WindowInfo &windowInfo = it.value();
        // 如果不存在这个窗口对应的多开窗口，则新建一个窗口，同时发送窗口新增的信号
        AppMultiItem *multiItem = new AppMultiItem(appItem, it.key(), windowInfo);
        m_itemList << multiItem;
        if (emitSignal)
            Q_EMIT itemInserted(-1, multiItem);
    }
}

// 检查对应的窗口是否存在多开窗口
bool DockItemManager::multiWindowExist(quint32 winId) const
{
    for (QPointer<DockItem> dockItem : m_itemList) {
        AppMultiItem *multiItem = qobject_cast<AppMultiItem *>(dockItem.data());
        if (!multiItem)
            continue;

        if (multiItem->winId() == winId)
            return true;
    }

    return false;
}

// 检查当前多开窗口是否需要移除
// 如果当前多开窗口图标对应的窗口在这个窗口所属的APP中所有打开窗口中不存在，那么则认为该多窗口已经被关闭
bool DockItemManager::needRemoveMultiWindow(AppMultiItem *multiItem) const
{
    // 查找多分窗口对应的窗口在应用所有的打开的窗口中是否存在，只要它对应的窗口存在，就无需删除
    // 只要不存在，就需要删除
    AppItem *appItem = multiItem->appItem();
    const WindowInfoMap &windowInfoMap = appItem->windowsMap();
    for (auto it = windowInfoMap.begin(); it != windowInfoMap.end(); it++) {
        if (it.key() == multiItem->winId())
            return false;
    }

    return true;
}

void DockItemManager::onShowMultiWindowChanged()
{
    if (m_appInter->showMultiWindow()) {
        // 如果当前设置支持窗口多开，那么就依次对每个APPItem加载多开窗口
        for (int i = 0; i < m_itemList.size(); i++) {
            const QPointer<DockItem> &dockItem = m_itemList[i];
            if (dockItem->itemType() != DockItem::ItemType::App)
                continue;

            updateMultiItems(static_cast<AppItem *>(dockItem.data()), true);
        }
    } else {
        // 如果当前设置不支持窗口多开，则删除所有的多开窗口
        QList<DockItem *> multiWindows;
        for (const QPointer<DockItem> &dockItem : m_itemList) {
            if (dockItem->itemType() != DockItem::AppMultiWindow)
                continue;

            multiWindows << dockItem.data();
        }
        for (DockItem *multiItem : multiWindows) {
            m_itemList.removeOne(multiItem);
            Q_EMIT itemRemoved(multiItem);
            multiItem->deleteLater();
        }
    }
}
#endif

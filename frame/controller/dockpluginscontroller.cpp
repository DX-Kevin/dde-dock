/*
 * Copyright (C) 2011 ~ 2018 Deepin Technology Co., Ltd.
 *
 * Author:     sbw <sbw@sbw.so>
 *
 * Maintainer: sbw <sbw@sbw.so>
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

#include "dockpluginscontroller.h"
#include "pluginsiteminterface.h"
#include "dockitemcontroller.h"
#include "dockpluginloader.h"
#include "item/traypluginitem.h"

#include <QDebug>
#include <QDir>

DockPluginsController::DockPluginsController(
    DockItemController *itemControllerInter)
    : QObject(itemControllerInter)
    , m_dbusDaemonInterface(QDBusConnection::sessionBus().interface())
    , m_itemControllerInter(itemControllerInter)
    , m_dockDaemonInter(new DockDaemonInter("com.deepin.dde.daemon.Dock", "/com/deepin/dde/daemon/Dock", QDBusConnection::sessionBus(), this))
{
    qApp->installEventFilter(this);

    refreshPluginSettings(QDateTime::currentMSecsSinceEpoch() / 1000 / 1000);

    connect(m_dockDaemonInter, &DockDaemonInter::PluginSettingsUpdated, this, &DockPluginsController::refreshPluginSettings);
}

void DockPluginsController::itemAdded(PluginsItemInterface * const itemInter, const QString &itemKey)
{
    // check if same item added
    if (m_pluginList.contains(itemInter))
        if (m_pluginList[itemInter].contains(itemKey))
            return;

    PluginsItem *item = nullptr;
    if (itemInter->pluginName() == "tray") {
        item = new TrayPluginItem(itemInter, itemKey);
        connect(static_cast<TrayPluginItem *>(item), &TrayPluginItem::fashionTraySizeChanged,
                this, &DockPluginsController::fashionTraySizeChanged, Qt::UniqueConnection);
    } else {
        item = new PluginsItem(itemInter, itemKey);
    }

    item->setVisible(false);

    m_pluginList[itemInter][itemKey] = item;

    emit pluginItemInserted(item);
}

void DockPluginsController::itemUpdate(PluginsItemInterface * const itemInter, const QString &itemKey)
{
    PluginsItem *item = pluginItemAt(itemInter, itemKey);

    Q_ASSERT(item);

    item->update();

    emit pluginItemUpdated(item);
}

void DockPluginsController::itemRemoved(PluginsItemInterface * const itemInter, const QString &itemKey)
{
    PluginsItem *item = pluginItemAt(itemInter, itemKey);

    if (!item)
        return;

    item->detachPluginWidget();

    emit pluginItemRemoved(item);

    m_pluginList[itemInter].remove(itemKey);

    // do not delete the itemWidget object(specified in the plugin interface)
    item->centralWidget()->setParent(nullptr);

    // just delete our wrapper object(PluginsItem)
    item->deleteLater();
}

void DockPluginsController::requestWindowAutoHide(PluginsItemInterface * const itemInter, const QString &itemKey, const bool autoHide)
{
    PluginsItem *item = pluginItemAt(itemInter, itemKey);
    Q_ASSERT(item);

    Q_EMIT item->requestWindowAutoHide(autoHide);
}

void DockPluginsController::requestRefreshWindowVisible(PluginsItemInterface * const itemInter, const QString &itemKey)
{
    PluginsItem *item = pluginItemAt(itemInter, itemKey);
    Q_ASSERT(item);

    Q_EMIT item->requestRefreshWindowVisible();
}

void DockPluginsController::requestSetAppletVisible(PluginsItemInterface * const itemInter, const QString &itemKey, const bool visible)
{
    PluginsItem *item = pluginItemAt(itemInter, itemKey);
    Q_ASSERT(item);

    if (visible) {
        item->showPopupApplet(itemInter->itemPopupApplet(itemKey));
    } else {
        item->hidePopup();
    }
}

void DockPluginsController::startLoader()
{
    DockPluginLoader *loader = new DockPluginLoader(this);

    connect(loader, &DockPluginLoader::finished, loader, &DockPluginLoader::deleteLater, Qt::QueuedConnection);
    connect(loader, &DockPluginLoader::pluginFounded, this, &DockPluginsController::loadPlugin, Qt::QueuedConnection);

    QTimer::singleShot(1, loader, [=] { loader->start(QThread::LowestPriority); });
}

void DockPluginsController::displayModeChanged()
{
    const DisplayMode displayMode = qApp->property(PROP_DISPLAY_MODE).value<Dock::DisplayMode>();
    const auto inters = m_pluginList.keys();

    for (auto inter : inters)
        inter->displayModeChanged(displayMode);
}

void DockPluginsController::positionChanged()
{
    const Position position = qApp->property(PROP_POSITION).value<Dock::Position>();
    const auto inters = m_pluginList.keys();

    for (auto inter : inters)
        inter->positionChanged(position);
}

void DockPluginsController::loadPlugin(const QString &pluginFile)
{
    QPluginLoader *pluginLoader = new QPluginLoader(pluginFile);
    const auto meta = pluginLoader->metaData().value("MetaData").toObject();
    if (!meta.contains("api") || meta["api"].toString() != DOCK_PLUGIN_API_VERSION)
    {
        qWarning() << "plugin api version not matched! expect version:" << DOCK_PLUGIN_API_VERSION << pluginFile;
        return;
    }

    PluginsItemInterface *interface = qobject_cast<PluginsItemInterface *>(pluginLoader->instance());
    if (!interface)
    {
        qWarning() << "load plugin failed!!!" << pluginLoader->errorString() << pluginFile;
        pluginLoader->unload();
        pluginLoader->deleteLater();
        return;
    }

    m_pluginList.insert(interface, QMap<QString, PluginsItem *>());

    QString dbusService = meta.value("depends-daemon-dbus-service").toString();
    if (!dbusService.isEmpty() && !m_dbusDaemonInterface->isServiceRegistered(dbusService).value()) {
        qDebug() << dbusService << "daemon has not started, waiting for signal";
        connect(m_dbusDaemonInterface, &QDBusConnectionInterface::serviceOwnerChanged, this,
            [=](const QString &name, const QString &oldOwner, const QString &newOwner) {
                if (name == dbusService && !newOwner.isEmpty()) {
                    qDebug() << dbusService << "daemon started, init plugin and disconnect";
                    initPlugin(interface);
                    disconnect(m_dbusDaemonInterface);
                }
            }
        );
        return;
    }

    initPlugin(interface);
}

void DockPluginsController::initPlugin(PluginsItemInterface *interface) {
    qDebug() << "init plugin: " << interface->pluginName();
    interface->init(this);
    qDebug() << "init plugin finished: " << interface->pluginName();
}

void DockPluginsController::refreshPluginSettings(qlonglong ts)
{
    // TODO: handle nano seconds

    const QString &pluginSettings = m_dockDaemonInter->GetPluginSettings().value();
    if (pluginSettings.isEmpty()) {
        qDebug() << "Error! get plugin settings from dbus failed!";
        return;
    }

    const QJsonObject &settingsObject = QJsonDocument::fromJson(pluginSettings.toLocal8Bit()).object();
    if (settingsObject.isEmpty()) {
        qDebug() << "Error! parse plugin settings from json failed!";
        return;
    }

    m_pluginSettingsObject = settingsObject;

    // not notify plugins to refresh settings if this update is not emit by dock daemon
    if (sender() != m_dockDaemonInter) {
        return;
    }

    // TODO: notify all plugins to reload plugin settings
}

bool DockPluginsController::eventFilter(QObject *o, QEvent *e)
{
    if (o != qApp)
        return false;
    if (e->type() != QEvent::DynamicPropertyChange)
        return false;

    QDynamicPropertyChangeEvent * const dpce = static_cast<QDynamicPropertyChangeEvent *>(e);
    const QString propertyName = dpce->propertyName();

    if (propertyName == PROP_POSITION)
        positionChanged();
    else if (propertyName == PROP_DISPLAY_MODE)
        displayModeChanged();

    return false;
}

PluginsItem *DockPluginsController::pluginItemAt(PluginsItemInterface * const itemInter, const QString &itemKey) const
{
    if (!m_pluginList.contains(itemInter))
        return nullptr;

    return m_pluginList[itemInter][itemKey];
}

void DockPluginsController::saveValue(PluginsItemInterface *const itemInter, const QString &key, const QVariant &value) {
    QJsonObject valueObject = m_pluginSettingsObject.value(itemInter->pluginName()).toObject();
    valueObject.insert(key, value.toJsonValue());
    m_pluginSettingsObject.insert(itemInter->pluginName(), valueObject);

    m_dockDaemonInter->SetPluginSettings(
                QDateTime::currentMSecsSinceEpoch() / 1000 / 1000,
                QJsonDocument(m_pluginSettingsObject).toJson());
}

const QVariant DockPluginsController::getValue(PluginsItemInterface *const itemInter, const QString &key, const QVariant& fallback) {
    QVariant v = m_pluginSettingsObject.value(itemInter->pluginName()).toObject().value(key).toVariant();
    if (v.isNull() || !v.isValid()) {
        v = fallback;
    }
    return v;
}

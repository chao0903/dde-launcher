/*
 * Copyright (C) 2017 ~ 2018 Deepin Technology Co., Ltd.
 *
 * Author:     sbw <sbw@sbw.so>
 *
 * Maintainer: sbw <sbw@sbw.so>
 *             kirigaya <kirigaya@mkacg.com>
 *             Hualet <mr.asianwang@gmail.com>
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

#include "appsmanager.h"
#include "util.h"
#include "constants.h"
#include "calculate_util.h"

#include <QDebug>
#include <QX11Info>
#include <QSvgRenderer>
#include <QPainter>
#include <QDataStream>
#include <QIODevice>
#include <QIcon>
#include <QScopedPointer>
#include <QIconEngine>

#include <private/qguiapplication_p.h>
#include <qpa/qplatformtheme.h>
#include <DHiDPIHelper>
#include <DApplication>
#include <DGuiApplicationHelper>

DWIDGET_USE_NAMESPACE

QPointer<AppsManager> AppsManager::INSTANCE = nullptr;

QGSettings *AppsManager::m_launcherSettings = SettingsPtr("com.deepin.dde.launcher", "", nullptr);
QSet<QString> AppsManager::APP_AUTOSTART_CACHE;
QSettings AppsManager::APP_USER_SORTED_LIST("deepin", "dde-launcher-app-sorted-list", nullptr);
QSettings AppsManager::APP_USED_SORTED_LIST("deepin", "dde-launcher-app-used-sorted-list");
QSettings AppsManager::APP_CATEGORY_USED_SORTED_LIST("deepin","dde-launcher-app-category-used-sorted-list");
static constexpr int USER_SORT_UNIT_TIME = 3600; // 1 hours

int perfectIconSize(const int size)
{
    const int s = 8;
    const int l[s] = { 16, 18, 24, 32, 64, 96, 128, 256 };

    for (int i(0); i != s; ++i)
        if (size <= l[i])
            return l[i];

    return 256;
}

const QPixmap AppsManager::getThemeIcon(const ItemInfo &itemInfo, const int size)
{
    const QString &iconName = itemInfo.m_iconKey;
    const auto ratio = qApp->devicePixelRatio();
    const int s = perfectIconSize(size);
    bool findIcon = true;

    QPixmap pixmap;
    do {
        if (iconName.startsWith("data:image/")) {
            const QStringList strs = iconName.split("base64,");
            if (strs.size() == 2)
                pixmap.loadFromData(QByteArray::fromBase64(strs.at(1).toLatin1()));

            if (!pixmap.isNull())
                break;
        }

        if (QFile::exists(iconName)) {
            if (iconName.endsWith(".svg"))
                pixmap = loadSvg(iconName, s * ratio);
            else
                pixmap = DHiDPIHelper::loadNxPixmap(iconName);

            if (!pixmap.isNull())
                break;
        }

        QIcon icon = QIcon::fromTheme(iconName);

        if (icon.isNull()) {
            icon = QIcon::fromTheme("application-x-desktop");
            //手动更新缓存
            system("gtk-update-icon-cache /usr/share/icons/hicolor/");

            findIcon = false;
        }

        pixmap = icon.pixmap(QSize(s, s));
        if (!pixmap.isNull())
            break;
    } while (false);

    pixmap = pixmap.scaled(QSize(s, s) * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pixmap.setDevicePixelRatio(ratio);

    QPair<QString, int> tmpKey { itemInfo.m_iconKey, s};
    if (m_CacheData[tmpKey].isNull() && findIcon )
        m_CacheData[tmpKey] = pixmap;
    return pixmap;
}

/**
 * @brief AppsManager::getPageCount 获取应用分类下列表中的item分页后的总页数
 * @param category 应用分类类型
 * @return 返回应用分类下列表中的item分页后的总页数
 */
int AppsManager::getPageCount(const AppsListModel::AppCategory category)
{
    int nSize = appsInfoListSize(category);
    int pageCount = m_calUtil->appPageItemCount(category);
    int page = nSize / pageCount;
    page = nSize % pageCount ? page + 1 : page;
    return page;
}

AppsManager::AppsManager(QObject *parent) :
    QObject(parent),
    m_launcherInter(new DBusLauncher(this)),
    m_startManagerInter(new DBusStartManager(this)),
    m_dockInter(new DBusDock(this)),
    m_iconRefreshTimer(std::make_unique<QTimer>(new QTimer)),
    m_calUtil(CalculateUtil::instance()),
    m_searchTimer(new QTimer(this)),
    m_delayRefreshTimer(new QTimer(this)),
    m_RefreshCalendarIconTimer(new QTimer(this)),
    m_lastShowDate(0)
{
    if (QGSettings::isSchemaInstalled("com.deepin.dde.Launcher")) {
        m_filterSetting = new QGSettings("com.deepin.dde.Launcher", "/com/deepin/dde/Launcher/");
        connect(m_filterSetting, &QGSettings::changed, this, [ & ] (const QString & keyName) {
            if (keyName != "filter-keys" && keyName != "filterKeys") {
                return;
            }
            refreshAllList();
        });
    }

    m_iconRefreshTimer->setInterval(10 * 1000);
    m_iconRefreshTimer->setSingleShot(false);

    m_categoryTs
            << tr("Internet")
            << tr("Chat")
            << tr("Music")
            << tr("Video")
            << tr("Graphics")
            << tr("Games")
            << tr("Office")
            << tr("Reading")
            << tr("Development")
            << tr("System")
            << tr("Other");

    m_categoryIcon
            << QString(":/icons/skin/icons/internet_normal_22px.svg")
            << QString(":/icons/skin/icons/chat_normal_22px.svg")
            << QString(":/icons/skin/icons/music_normal_22px.svg")
            << QString(":/icons/skin/icons/multimedia_normal_22px.svg")
            << QString(":/icons/skin/icons/graphics_normal_22px.svg")
            << QString(":/icons/skin/icons/game_normal_22px.svg")
            << QString(":/icons/skin/icons/office_normal_22px.svg")
            << QString(":/icons/skin/icons/reading_normal_22px.svg")
            << QString(":/icons/skin/icons/development_normal_22px.svg")
            << QString(":/icons/skin/icons/system_normal_22px.svg")
            << QString(":/icons/skin/icons/others_normal_22px.svg");

    refreshAllList();
    refreshAppAutoStartCache();

    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(150);
    m_delayRefreshTimer->setSingleShot(true);
    m_delayRefreshTimer->setInterval(500);

    m_RefreshCalendarIconTimer->setInterval(1000);
    m_RefreshCalendarIconTimer->setSingleShot(false);

    connect(qApp, &DApplication::iconThemeChanged, this, &AppsManager::onIconThemeChanged, Qt::QueuedConnection);
    connect(m_launcherInter, &DBusLauncher::NewAppLaunched, this, &AppsManager::markLaunched);
    connect(m_launcherInter, &DBusLauncher::SearchDone, this, &AppsManager::searchDone);
    connect(m_launcherInter, &DBusLauncher::UninstallSuccess, this, &AppsManager::abandonStashedItem);
    connect(m_launcherInter, &DBusLauncher::UninstallFailed, [this](const QString & appKey) { restoreItem(appKey); emit dataChanged(AppsListModel::All); });
    connect(m_launcherInter, &DBusLauncher::ItemChanged, this, &AppsManager::handleItemChanged);
    connect(m_dockInter, &DBusDock::IconSizeChanged, this, &AppsManager::IconSizeChanged, Qt::QueuedConnection);
    connect(m_dockInter, &DBusDock::FrontendRectChanged, this, &AppsManager::dockGeometryChanged, Qt::QueuedConnection);
    connect(m_startManagerInter, &DBusStartManager::AutostartChanged, this, &AppsManager::refreshAppAutoStartCache);
    connect(m_delayRefreshTimer, &QTimer::timeout, this, &AppsManager::delayRefreshData);
    connect(m_searchTimer, &QTimer::timeout, this, &AppsManager::onSearchTimeOut);

    connect(DGuiApplicationHelper::instance(), &DGuiApplicationHelper::themeTypeChanged, this, [ = ] {
        refreshAppListIcon();
        generateCategoryMap();
    });

    connect(m_iconRefreshTimer.get(), &QTimer::timeout, this, &AppsManager::refreshNotFoundIcon);
    connect(m_RefreshCalendarIconTimer, &QTimer::timeout, this,  [=](){
               m_curDate = QDate::currentDate();
               if(m_lastShowDate != m_curDate.day()){
                    delayRefreshData();
                    m_lastShowDate = m_curDate.day();
               }
    });

    if (!m_RefreshCalendarIconTimer->isActive())
        m_RefreshCalendarIconTimer->start();
}

/**
 * @brief AppsManager::appendSearchResult 搜索到的数据写入搜索列表
 * @param appKey 应用名
 */
void AppsManager::appendSearchResult(const QString &appKey)
{
    for (const ItemInfo &info : m_allAppInfoList)
        if (info.m_key == appKey)
            return m_appSearchResultList.append(info);
}

void AppsManager::sortCategory(const AppsListModel::AppCategory category)
{
    switch (category) {
    case AppsListModel::Search:     sortByPresetOrder(m_appSearchResultList);      break;
//    case AppsListModel::All:        sortByName(m_appInfoList);              break;
    // disable sort other category
    default: Q_ASSERT(false) ;
    }
}

/**
 * @brief AppsManager::sortByPresetOrder app应用按照schemas文件中的预装应用列表顺序进行排序
 * @param processList 系统所有应用软件的信息
 */
void AppsManager::sortByPresetOrder(ItemInfoList &processList)
{
    const QString system_lang = QLocale::system().name();

    QString key = "appsOrder";
    for (const auto &item : system_lang.split('_')) {
        Q_ASSERT(!item.isEmpty());

        QString k = item.toLower();
        k[0] = k[0].toUpper();

        key.append(k);
    }

    QStringList preset;
    if (m_launcherSettings && m_launcherSettings->keys().contains(key))
        preset = m_launcherSettings->get(key).toStringList();
    if (m_launcherSettings && preset.isEmpty())
        preset = m_launcherSettings->get("apps-order").toStringList();

    std::sort(processList.begin(), processList.end(), [&preset](const ItemInfo & i1, const ItemInfo & i2) {
        int index1 = preset.indexOf(i1.m_key);
        int index2 = preset.indexOf(i2.m_key);
        if (index1 == index2) {
            // If both of them don't exist in the preset list,
            // fallback to comparing their name.
            return i1.m_name < i2.m_name;
        }

        // If one of them doesn't exist in the preset list,
        // the one exists go first.
        if (index1 == -1) {
            return false;
        }
        if (index2 == -1) {
            return true;
        }

        // If both of them exist, then obey the preset order.
        return index1 < index2;
    });
}

/**
 * @brief AppsManager::sortByInstallTimeOrder app应用按照应用安装的时间先后排序
 * @param processList 系统所有应用软件的信息
 */
void AppsManager::sortByInstallTimeOrder(ItemInfoList &processList)
{
    std::sort(processList.begin(), processList.end(), [&](const ItemInfo & i1, const ItemInfo & i2) {
        if (i1.m_installedTime == i2.m_installedTime && i1.m_installedTime != 0) {
            // If both of them don't exist in the preset list,
            // fallback to comparing their name.
            return i1.m_installedTime < i2.m_installedTime;
        }

        // If one of them doesn't exist in the preset list,
        // the one exists go first.
        if (i1.m_installedTime == 0) {
            return false;
        }

        if (i2.m_installedTime == 0) {
            return true;
        }

        // If both of them exist, then obey the preset order.
        return i1.m_installedTime < i2.m_installedTime;
    });
}

AppsManager *AppsManager::instance()
{
    if (INSTANCE.isNull())
        INSTANCE = new AppsManager;

    return INSTANCE;
}

void AppsManager::stashItem(const QModelIndex &index)
{
    const QString key = index.data(AppsListModel::AppKeyRole).toString();

    return stashItem(key);
}

void AppsManager::stashItem(const QString &appKey)
{
    for (int i(0); i != m_allAppInfoList.size(); ++i) {
        if (m_allAppInfoList[i].m_key == appKey) {
            m_stashList.append(m_allAppInfoList[i]);
            m_allAppInfoList.removeAt(i);

            generateCategoryMap();
            refreshUsedInfoList();
            refreshUserInfoList();
            refreshCategoryUsedInfoList();

            return;
        }
    }
}

/**
 * @brief AppsManager::abandonStashedItem 卸载应用更新列表
 * @param appKey 应用的key
 */
void AppsManager::abandonStashedItem(const QString &appKey)
{
    //qDebug() << "bana" << appKey;
    for (int i(0); i != m_stashList.size(); ++i) {
        if (m_stashList[i].m_key == appKey) {
            m_stashList.removeAt(i);
            break;
        }
    }
    //重新获取分类数据，类似wps一个appkey对应多个desktop文件的时候,有可能会导致漏掉
    refreshCategoryInfoList();

    emit dataChanged(AppsListModel::All);
}

void AppsManager::restoreItem(const QString &appKey, const int pos)
{
    for (int i(0); i != m_stashList.size(); ++i) {
        if (m_stashList[i].m_key == appKey) {
            // if pos is valid
            if (pos != -1) {
//                int itemIndex = m_pageIndex[AppsListModel::All] * m_calUtil->appPageItemCount() + pos;
                if (m_calUtil->displayMode() == ALL_APPS)
                    m_usedSortedList.insert(pos, m_stashList[i]);

                if (m_calUtil->displayMode() == GROUP_BY_CATEGORY)
                    m_appInfos[m_stashList[i].category()].insert(pos, m_stashList[i]);
            }
            m_allAppInfoList.append(m_stashList[i]);
            m_stashList.removeAt(i);

            generateCategoryMap();
            refreshCategoryUsedInfoList();

            return saveUsedSortedList();
        }
    }
}

int AppsManager::dockPosition() const
{
    return m_dockInter->position();
}

int AppsManager::dockWidth() const
{
    return dockGeometry().width();
}

QRect AppsManager::dockGeometry() const
{
    return QRect(m_dockInter->frontendRect());
}

bool AppsManager::isVaild()
{
    return m_launcherInter->isValid() && !m_allAppInfoList.isEmpty();
}

void AppsManager::refreshAllList()
{
    m_newInstalledAppsList = m_launcherInter->GetAllNewInstalledApps().value();

    refreshCategoryInfoList();
    refreshUsedInfoList();
    refreshCategoryUsedInfoList();
    refreshUserInfoList();
}

void AppsManager::saveUserSortedList()
{
    // save cache
    QByteArray writeBuf;
    QDataStream out(&writeBuf, QIODevice::WriteOnly);
    out << m_userSortedList;
    APP_USER_SORTED_LIST.setValue("list", writeBuf);
}

/**
 * @brief AppsManager::saveUsedSortedList 保存应用使用排序(时间上的先后)列表
 */
void AppsManager::saveUsedSortedList()
{
    QByteArray writeBuf;
    QDataStream out(&writeBuf, QIODevice::WriteOnly);
    out << m_usedSortedList;

    APP_USED_SORTED_LIST.setValue("list", writeBuf);
}

void AppsManager::searchApp(const QString &keywords)
{
    m_searchTimer->start();
    m_searchText = keywords;
}

void AppsManager::launchApp(const QModelIndex &index)
{
    const QString appDesktop = index.data(AppsListModel::AppDesktopRole).toString();
    QString appKey = index.data(AppsListModel::AppKeyRole).toString();
    markLaunched(appKey);

    for (ItemInfo &info : m_userSortedList) {
        if (info.m_key == appKey) {
            const int idx = m_userSortedList.indexOf(info);

            if (idx != -1) {
                m_userSortedList[idx].m_openCount++;

                if (m_userSortedList[idx].m_firstRunTime == 0) {
                    m_userSortedList[idx].m_firstRunTime = QDateTime::currentMSecsSinceEpoch() / 1000;
                }
            }

            break;
        }
    }

    refreshUserInfoList();

    if (!appDesktop.isEmpty())
        m_startManagerInter->LaunchWithTimestamp(appDesktop, QX11Info::getTimestamp());
}

void AppsManager::uninstallApp(const QString &appKey)
{
    // refersh auto start cache
    for (const ItemInfo &info : m_allAppInfoList) {
        if (info.m_key == appKey) {
            APP_AUTOSTART_CACHE.remove(info.m_desktop);
            break;
        }
    }

    // begin uninstall, remove icon first.
    stashItem(appKey);
    //卸载前，将应用从任务栏移除
    m_dockInter->RequestUndock(appKey);
    // request backend
    m_launcherInter->RequestUninstall(appKey, false);

    emit dataChanged(AppsListModel::All);

    // refersh search result
    m_searchTimer->start();
}

void AppsManager::markLaunched(QString appKey)
{
    if (appKey.isEmpty() || !m_newInstalledAppsList.contains(appKey))
        return;

    m_newInstalledAppsList.removeOne(appKey);
    m_launcherInter->MarkLaunched(appKey);

    refreshUserInfoList();

    emit newInstallListChanged();
}

/**
 * @brief AppsManager::delayRefreshData 延迟刷新安装的应用列表,保存用户操作后的应用排序
 */
void AppsManager::delayRefreshData()
{
    // refresh new installed apps
    m_newInstalledAppsList = m_launcherInter->GetAllNewInstalledApps().value();

    generateCategoryMap();
    saveUserSortedList();

    emit newInstallListChanged();

    emit dataChanged(AppsListModel::All);
}

/**
 * @brief 模糊匹配，反向查询key是否包含list任一个元素
 *
 * @param list 关键字列表
 * @param key 要模糊匹配的关键词
 * @return true 表示匹配成功
 * @return false 表示匹配失败
 */
bool AppsManager::fuzzyMatching(const QStringList& list, const QString& key)
{
    for (const QString& l : list) {
        if (key.indexOf(l, Qt::CaseInsensitive) != -1) {
            return true;
        }
    }
    return false;
}

const ItemInfo AppsManager::createOfCategory(qlonglong category)
{
    ItemInfo info;
    info.m_name = m_categoryTs[category];
    info.m_categoryId = category;
    info.m_iconKey = m_categoryIcon[category];
    return info;
}

const ItemInfoList AppsManager::appsInfoList(const AppsListModel::AppCategory &category) const
{
    switch (category) {
    case AppsListModel::Custom:    return m_userSortedList;
    case AppsListModel::All:       return m_usedSortedList;
    case AppsListModel::Search:     return m_appSearchResultList;
    case AppsListModel::Category:   return m_categoryList;
    default:;
    }

    return m_appInfos[category];
}

/**
 * @brief AppsManager::appsInfoListSize 获取自定义模式,自由模式,搜索模式,多应用分类模式,单个分类模式下app数量
 * @param category 场景模式
 * @return
 */
int AppsManager::appsInfoListSize(const AppsListModel::AppCategory &category) const
{
    switch (category) {
    case AppsListModel::Custom:    return m_userSortedList.size();
    case AppsListModel::All:       return m_usedSortedList.size();
    case AppsListModel::Search:     return m_appSearchResultList.size();
    case AppsListModel::Category:   return m_categoryList.size();
    default:;
    }

    return m_appInfos[category].size();
}

/**
 * @brief AppsManager::appsInfoListIndex 获取具体场景模式下app信息
 * @param category 场景模式
 * @param index app在列表中的索引
 * @return 返回单个应用信息
 */
const ItemInfo AppsManager::appsInfoListIndex(const AppsListModel::AppCategory &category, const int index) const
{
    switch (category) {
    case AppsListModel::Custom:
        Q_ASSERT(m_userSortedList.size() > index);
        return m_userSortedList[index];
    case AppsListModel::All:
        Q_ASSERT(m_usedSortedList.size() > index);
        return m_usedSortedList[index];
    case AppsListModel::Search:
        Q_ASSERT(m_appSearchResultList.size() > index);
        return m_appSearchResultList[index];
    case AppsListModel::Category:
        Q_ASSERT(m_categoryList.size() > index);
        return m_categoryList[index];
    default:;
    }
    Q_ASSERT(m_appInfos[category].size() > index);
    return m_appInfos[category][index];
}

bool AppsManager::appIsNewInstall(const QString &key)
{
    return m_newInstalledAppsList.contains(key);
}

bool AppsManager::appIsAutoStart(const QString &desktop)
{
    int index = desktop.lastIndexOf('/');
    return APP_AUTOSTART_CACHE.contains(index > 0 ? desktop.right(desktop.size() - index - 1) : desktop);
}

bool AppsManager::appIsOnDock(const QString &desktop)
{
    return m_dockInter->IsDocked(desktop);
}

bool AppsManager::appIsOnDesktop(const QString &desktop)
{
    return m_launcherInter->IsItemOnDesktop(desktop).value();
}

bool AppsManager::appIsProxy(const QString &desktop)
{
    return m_launcherInter->GetUseProxy(desktop).value();
}

bool AppsManager::appIsEnableScaling(const QString &desktop)
{
    return !m_launcherInter->GetDisableScaling(desktop);
}

/**
 * @brief AppsManager::appIcon 从缓存中获取app图片
 * @param info app信息
 * @param size app的长宽
 * @return 图片对象
 */
const QPixmap AppsManager::appIcon(const ItemInfo &info, const int size)
{
    const int s = perfectIconSize(size);
    QPair<QString, int> tmpKey { cacheKey(info, CacheType::ImageType) , s};

    if (m_CacheData.contains(tmpKey) && !m_CacheData[tmpKey].isNull()) {
        return m_CacheData[tmpKey].value<QPixmap>();
    } else {
        const QPixmap &pixmap = getThemeIcon(info, size);
        return pixmap;
    }
}

/**
 * @brief AppsManager::appName 从缓存获取app名称
 * @param info app信息
 * @param size app的长宽
 * @return app名称
 */
const QString AppsManager::appName(const ItemInfo &info, const int size)
{
    QPair<QString, int> tmpKey { cacheKey(info, CacheType::TextType) , size };

    if (m_CacheData.contains(tmpKey) && !m_CacheData[tmpKey].isNull()) {
        return m_CacheData[tmpKey].toString();
    } else {
        const QFontMetrics fm = qApp->fontMetrics();
        QString fm_string = fm.elidedText(info.m_name, Qt::ElideRight, size);
        m_CacheData[tmpKey] = fm_string;
        return fm_string;
    }
}

/**
 * @brief AppsManager::refreshCategoryInfoList 更新所有应用信息
 */
void AppsManager::refreshCategoryInfoList()
{
    QDBusPendingReply<ItemInfoList> reply = m_launcherInter->GetAllItemInfos();
    if (reply.isError()) {
        qWarning() << "data is empty, quit!!";
        qWarning() << reply.error();
        qApp->quit();
    }

    QStringList filters;
    if (m_filterSetting != nullptr) {
        filters = m_filterSetting->get("filter-keys").toStringList();
    }

    QByteArray readBuf = APP_USED_SORTED_LIST.value("list").toByteArray();
    QDataStream in(&readBuf, QIODevice::ReadOnly);
    in >> m_usedSortedList;
    for(const ItemInfo& used : m_usedSortedList) {
        bool bContains = fuzzyMatching(filters, used.m_key);
        if (bContains) {
            m_usedSortedList.removeOne(used);
        }
    }

    // 从配置文件中读取全屏分类下的应用
    int beginCategoryIndex = int(AppsListModel::AppCategory::Internet);
    int endCategoryIndex = int(AppsListModel::AppCategory::Others);
    for (; beginCategoryIndex < endCategoryIndex; beginCategoryIndex++) {
        ItemInfoList itemInfoList;
        QByteArray readCategoryBuf = APP_CATEGORY_USED_SORTED_LIST.value(QString("%1").arg(beginCategoryIndex)).toByteArray();
        QDataStream categoryIn(&readCategoryBuf, QIODevice::ReadOnly);
        categoryIn >> itemInfoList;
        m_appInfos.insert(AppsListModel::AppCategory(beginCategoryIndex), itemInfoList);
    }

    const ItemInfoList &datas = reply.value();
    m_allAppInfoList.clear();
    m_allAppInfoList.reserve(datas.size());
    for (const auto &it : datas) {
        bool bContains = fuzzyMatching(filters, it.m_key);
        if (!m_stashList.contains(it) && !bContains) {
            m_allAppInfoList.append(it);
        }
    }

    generateCategoryMap();
}

/**
 * @brief AppsManager::refreshUsedInfoList 更新使用过的应用列表
 */
void AppsManager::refreshUsedInfoList()
{
    // init data if used sorted list is empty.
    if (m_usedSortedList.isEmpty()) {
        // first reads the config file.
        QByteArray readBuffer = APP_USED_SORTED_LIST.value("list").toByteArray();
        QDataStream in(&readBuffer, QIODevice::ReadOnly);
        in >> m_usedSortedList;

        // if data cache file is empty.
        if (m_usedSortedList.isEmpty()) {
            m_usedSortedList = m_allAppInfoList;
        }

        // add new additions
        for (QList<ItemInfo>::ConstIterator it = m_allAppInfoList.constBegin(); it != m_allAppInfoList.constEnd(); ++it) {
            if (!m_usedSortedList.contains(*it)) {
                m_usedSortedList.append(*it);
            }
        }

        // check used list isvaild
        for (QList<ItemInfo>::iterator it = m_usedSortedList.begin(); it != m_usedSortedList.end();) {
            if (m_allAppInfoList.contains(*it)) {
                it++;
            } else {
                it = m_usedSortedList.erase(it);
            }
        }

        updateUsedListInfo();
    }

    saveUsedSortedList();
}

/**
 * @brief AppsManager::refreshCategoryUsedInfoList 保存全屏分类排序数据
 */
void AppsManager::refreshCategoryUsedInfoList()
{
    // 保存排序信息
    QHash<AppsListModel::AppCategory, ItemInfoList>::iterator categoryAppsIter = m_appInfos.begin();
    for (; categoryAppsIter != m_appInfos.end(); categoryAppsIter++) {
        int category = categoryAppsIter.key();

        QByteArray writeBuf;
        QDataStream out(&writeBuf, QIODevice::WriteOnly);
        out << categoryAppsIter.value();
        APP_CATEGORY_USED_SORTED_LIST.setValue(QString("%1").arg(category), writeBuf);
    }
}

/**
 * @brief AppsManager::refreshUserInfoList app安装时间排序
 */
void AppsManager::refreshUserInfoList()
{
    if (m_userSortedList.isEmpty()) {
        // first reads the config file.
        QByteArray readBuffer = APP_USER_SORTED_LIST.value("list").toByteArray();
        QDataStream in(&readBuffer, QIODevice::ReadOnly);
        in >> m_userSortedList;

        // if data cache file is empty.
        if (m_userSortedList.isEmpty()) {
            m_userSortedList = m_allAppInfoList;
        } else {
            // check used list isvaild
            for (QList<ItemInfo>::iterator it = m_userSortedList.begin(); it != m_userSortedList.end();) {
                int idx = m_allAppInfoList.indexOf(*it);
                if (idx >= 0) {

                    // 更换语言的时候更新语言
                    it->updateInfo(m_allAppInfoList.at(idx));

                    it++;
                } else {
                    it = m_userSortedList.erase(it);
                }
            }

            // add new additions
            for (QList<ItemInfo>::Iterator it = m_allAppInfoList.begin(); it != m_allAppInfoList.end(); ++it) {
                if (!m_userSortedList.contains(*it)) {
                    m_userSortedList.append(*it);
                }
            }
        }
    }

    // 从启动器小屏应用列表移除被限制使用的应用
    QStringList filters;
    if (m_filterSetting != nullptr)
        filters = m_filterSetting->get("filter-keys").toStringList();

    for (auto it = m_userSortedList.begin(); it != m_userSortedList.end(); it++) {
        if (fuzzyMatching(filters, it->m_key))
            m_userSortedList.erase(it);
    }

    const qint64 currentTime = QDateTime::currentMSecsSinceEpoch() / 1000;
    // If the first run time is less than the current time, I am not sure can maintain the correct results.
    std::stable_sort(m_userSortedList.begin(), m_userSortedList.end(), [ = ](const ItemInfo & a, const ItemInfo & b) {
        const bool ANewInsatll = m_newInstalledAppsList.contains(a.m_key);
        const bool BNewInsatll = m_newInstalledAppsList.contains(b.m_key);
        if (ANewInsatll || BNewInsatll) {
            if(ANewInsatll && BNewInsatll)
                return a.m_installedTime > b.m_installedTime;

            if(ANewInsatll) return true;
            if(BNewInsatll) return false;
        }

        const auto AFirstRunTime = a.m_firstRunTime;
        const auto BFirstRunTime = b.m_firstRunTime;

        // If it's past time, will be sorted by open count
        if (AFirstRunTime > currentTime || BFirstRunTime > currentTime) {
            return a.m_openCount > b.m_openCount;
        }

        int hours_diff_a = (currentTime - AFirstRunTime) / USER_SORT_UNIT_TIME + 1;
        int hours_diff_b = (currentTime - BFirstRunTime) / USER_SORT_UNIT_TIME + 1;

        // Average number of starts
        return (static_cast<double>(a.m_openCount) / hours_diff_a) > (static_cast<double>(b.m_openCount) / hours_diff_b);
    });

    saveUserSortedList();
}

/**
 * @brief AppsManager::updateUsedListInfo 更新应用信息
 */
void AppsManager::updateUsedListInfo()
{
    for (const ItemInfo &info : m_allAppInfoList) {
        const int idx = m_usedSortedList.indexOf(info);

        if (idx != -1) {
            const int openCount = m_usedSortedList[idx].m_openCount;
            m_usedSortedList[idx].updateInfo(info);
            m_usedSortedList[idx].m_openCount = openCount;
        }
    }
//    ReflashSortList();
}

/**
 * @brief AppsManager::generateCategoryMap 加入新装的app到列表,从列表中移除卸载的app
 */
void AppsManager::generateCategoryMap()
{
    m_categoryList.clear();
    sortByPresetOrder(m_allAppInfoList);

    ItemInfoList newInstallAppList;
    for (const ItemInfo &info : m_allAppInfoList) {

        const int userIdx = m_usedSortedList.indexOf(info);
        // append new installed app to user sorted list
        if (userIdx == -1) {
            m_usedSortedList.append(info);
        } else {
            const int openCount = m_usedSortedList[userIdx].m_openCount;
            m_usedSortedList[userIdx].updateInfo(info);
            m_usedSortedList[userIdx].m_openCount = openCount;
        }

        const AppsListModel::AppCategory category = info.category();
        if (!m_appInfos.contains(category))
            m_appInfos.insert(category, ItemInfoList());

        // 将已有应用保存到 m_appInfos， 新添加应用保存到 newInstallAppList
        if (!m_newInstalledAppsList.contains(info.m_key)) {
            // 检查应用是否已经存在
            const int idx = m_appInfos[category].indexOf(info);
            if (idx == -1) {
                m_appInfos[category].append(info);
            } else {
                // 更新一下已有应用信息
                qlonglong openCount = m_appInfos[category][idx].m_openCount;
                m_appInfos[category][idx].updateInfo(info);
                m_appInfos[category][idx].m_openCount = openCount;
            }
        } else {
            newInstallAppList.append(info);
        }
    }


    sortByInstallTimeOrder(newInstallAppList);
    if (!newInstallAppList.isEmpty()) {
        for (const ItemInfo &info : newInstallAppList) {
            if (!m_appInfos[info.category()].contains(info)) {
                m_appInfos[info.category()].append(info);
            } else {
                // 更新一下新应用信息
                const int idx = m_appInfos[info.category()].indexOf(info);
                qlonglong openCount = m_appInfos[info.category()][idx].m_openCount;
                m_appInfos[info.category()][idx].updateInfo(info);
                m_appInfos[info.category()][idx].m_openCount = openCount;
            }
        }
    }


    // 移除 m_appInfos 中已经不存在的应用
    QHash<AppsListModel::AppCategory, ItemInfoList>::iterator categoryAppsIter = m_appInfos.begin();
    for (; categoryAppsIter != m_appInfos.end(); categoryAppsIter++) {
        ItemInfoList &item = categoryAppsIter.value();
        for (auto it(item.begin()); it != item.end();) {
            int idx = m_allAppInfoList.indexOf(*it);

            // 在全屏自由排序模式下，m_allAppInfoList 中不存在的应用，可能缓存到 m_stashList 里面了，需要查一下
            // 检查是为了不更新 m_appInfos
            if (idx == -1 && (m_calUtil->displayMode() == ALL_APPS))
                idx = m_stashList.indexOf(*it);

            if (idx == -1)
                it = item.erase(it);
            else
                it++;
        }
    }

    // remove uninstalled app item
    for (auto it(m_usedSortedList.begin()); it != m_usedSortedList.end();) {
        int idx = m_allAppInfoList.indexOf(*it);

        // 如果是分类模式，那么再查一下 m_stashList，为了不要更新 m_usedSortedList
        if (idx == -1 && (m_calUtil->displayMode() == GROUP_BY_CATEGORY)) {
            idx = m_stashList.indexOf(*it);
        }
        if (idx == -1)
            it = m_usedSortedList.erase(it);
        else
            ++it;
    }

    // swt question?
    std::list<qlonglong> categoryID;
    for (const ItemInfo &it : m_allAppInfoList) {
        if (std::find(categoryID.begin(), categoryID.end(), it.m_categoryId) == categoryID.end()) {
            categoryID.push_back(it.m_categoryId);
        }
    }

    for (auto it = categoryID.begin(); it != categoryID.end(); ++it) {
        m_categoryList << createOfCategory(*it);
    }

    std::sort(m_categoryList.begin(),
              m_categoryList.end(),
    [ = ](const ItemInfo & info1, const ItemInfo & info2) {
        return info1.m_categoryId < info2.m_categoryId;
    });

    emit categoryListChanged();
}

int AppsManager::appNums(const AppsListModel::AppCategory &category) const
{
    return appsInfoListSize(category);
}

/**
 * @brief AppsManager::refreshAppAutoStartCache 刷新自启动应用集
 * @param type 操作类型
 * @param desktpFilePath 应用路径
 */
void AppsManager::refreshAppAutoStartCache(const QString &type, const QString &desktpFilePath)
{
    if (type.isEmpty()) {
        APP_AUTOSTART_CACHE.clear();
        const QStringList &desktop_list = m_startManagerInter->AutostartList().value();

        for (const QString &auto_start_desktop : desktop_list) {
            const QString desktop_file_name = auto_start_desktop.split("/").last();

            if (!desktop_file_name.isEmpty())
                APP_AUTOSTART_CACHE.insert(desktop_file_name);
        }
    } else {
        const QString desktop_file_name = desktpFilePath.split("/").last();

        if (desktop_file_name.isEmpty())
            return;

        if (type == "added") {
            APP_AUTOSTART_CACHE.insert(desktop_file_name);
        } else if (type == "deleted") {
            APP_AUTOSTART_CACHE.remove(desktop_file_name);
        }

        emit dataChanged(AppsListModel::All);
    }
}

/**
 * @brief AppsManager::onSearchTimeOut 搜索超时错误提示
 */
void AppsManager::onSearchTimeOut()
{
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(m_launcherInter->Search(m_searchText), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [ = ](QDBusPendingCallWatcher * w) {
        if (w->isError()) qDebug() << w->error();

        w->deleteLater();
    });
}

void AppsManager::refreshNotFoundIcon()
{
    QPlatformTheme *const platformTheme = QGuiApplicationPrivate::platformTheme();
    const qreal ratio = qApp->devicePixelRatio();

    for (auto it = m_notExistIconMap.begin(); it != m_notExistIconMap.end();) {
        // The number of retries is 6
        if (it->second > 6) {
            it = m_notExistIconMap.erase(it);
            continue;
        }

        const std::pair<ItemInfo, int> itemPair = it->first;
        // QIcon have cache, cannot use QIcon::fromTheme or QIcon::hasThemeIcon
        QScopedPointer<QIconEngine> engine(platformTheme->createIconEngine(itemPair.first.m_iconKey));
        if (!engine.isNull() && !engine->isNull()) {
            for (auto iconIt = m_CacheData.begin(); iconIt != m_CacheData.end(); ++iconIt) {
                if (iconIt.key().first == cacheKey(itemPair.first, CacheType::ImageType) && iconIt.key().second / ratio == itemPair.second) {
                    const QPair<QString, int> iconPair{ cacheKey(itemPair.first, CacheType::ImageType), iconIt.key().second };
                    const QPixmap &pixmap = getThemeIcon(itemPair.first, itemPair.second);
                    m_CacheData[iconPair] = pixmap;
                    emit itemDataChanged(itemPair.first);
                    it = m_notExistIconMap.erase(it);
                    break;
                }
            }
        }

        // Maybe map only have one data
        if (it != m_notExistIconMap.end()) {
            it->second += 1;
            ++it;
        }
    }

    if (m_notExistIconMap.size() == 0) {
        return m_iconRefreshTimer->stop();
    }
}

void AppsManager::onIconThemeChanged()
{
    m_CacheData.clear();

    emit dataChanged(AppsListModel::All);
}

/**
 * @brief AppsManager::searchDone 搜索完成
 * @param resultList 搜索后接口返回的结果列表
 */
void AppsManager::searchDone(const QStringList &resultList)
{
    m_appSearchResultList.clear();

    QStringList resultCopy = resultList;
    if (m_filterSetting != nullptr) {
        QStringList filters = m_filterSetting->get("filter-keys").toStringList();
        for (const QString& result : resultCopy) {
            bool bContains = fuzzyMatching(filters, result);
            if (bContains) {
                resultCopy.removeAll(result);
            }
        }
    }

    for (const QString &key : resultCopy)
        appendSearchResult(key);

    emit dataChanged(AppsListModel::Search);

    if (m_appSearchResultList.isEmpty())
        emit requestTips(tr("No search results"));
    else
        emit requestHideTips();
}

/**
 * @brief AppsManager::handleItemChanged
 * @param operation 操作类型
 * @param appInfo 操作的应用对象信息
 * @param categoryNumber 暂时没有用
 */
void AppsManager::handleItemChanged(const QString &operation, const ItemInfo &appInfo, qlonglong categoryNumber)
{
    Q_UNUSED(categoryNumber);
    if (operation == "created") {
        ItemInfo info = appInfo;

        QStringList filters;
        if (m_filterSetting != nullptr) {
            filters = m_filterSetting->get("filter-keys").toStringList();
        }
        if (fuzzyMatching(filters, appInfo.m_key)) {
            return;
        }

        m_allAppInfoList.append(info);
        m_usedSortedList.append(info);
        m_userSortedList.push_front(info);
        pushPixmap(appInfo);
    } else if (operation == "deleted") {

        m_allAppInfoList.removeOne(appInfo);
        m_usedSortedList.removeOne(appInfo);
        m_userSortedList.removeOne(appInfo);
    } else if (operation == "updated") {

        Q_ASSERT(m_allAppInfoList.contains(appInfo));

        // 更新所有应用列表
        int appIndex = m_allAppInfoList.indexOf(appInfo);
        if (appIndex != -1) {
            m_allAppInfoList[appIndex].updateInfo(appInfo);
        }

        // 更新按照最近使用顺序排序的列表
        int sortAppIndex = m_usedSortedList.indexOf(appInfo);
        if (sortAppIndex != -1) {
            m_usedSortedList[sortAppIndex].updateInfo(appInfo);
        }
    }
//    ReflashSortList();

    m_delayRefreshTimer->start();
}

void AppsManager::refreshAppListIcon()
{
    m_categoryIcon.clear();
    if (DGuiApplicationHelper::instance()->themeType() == DGuiApplicationHelper::DarkType) {
        m_categoryIcon
                << QString(":/icons/skin/icons/category_network_dark.svg")
                << QString(":/icons/skin/icons/category_chat_dark.svg")
                << QString(":/icons/skin/icons/category_music_dark.svg")
                << QString(":/icons/skin/icons/category_video_dark.svg")
                << QString(":/icons/skin/icons/category_graphic_dark.svg")
                << QString(":/icons/skin/icons/category_game_dark.svg")
                << QString(":/icons/skin/icons/category_office_dark.svg")
                << QString(":/icons/skin/icons/category_reading_dark.svg")
                << QString(":/icons/skin/icons/category_develop_dark.svg")
                << QString(":/icons/skin/icons/category_system_dark.svg")
                << QString(":/icons/skin/icons/category_others_dark.svg");
    } else {
        m_categoryIcon
                << QString(":/icons/skin/icons/category_network.svg")
                << QString(":/icons/skin/icons/category_chat.svg")
                << QString(":/icons/skin/icons/category_music.svg")
                << QString(":/icons/skin/icons/category_video.svg")
                << QString(":/icons/skin/icons/category_graphic.svg")
                << QString(":/icons/skin/icons/category_game.svg")
                << QString(":/icons/skin/icons/category_office.svg")
                << QString(":/icons/skin/icons/category_reading.svg")
                << QString(":/icons/skin/icons/category_develop.svg")
                << QString(":/icons/skin/icons/category_system.svg")
                << QString(":/icons/skin/icons/category_others.svg");
    }
}

void AppsManager::pushPixmap()
{
    for (auto itemInfo : m_allAppInfoList) {
        pushPixmap(itemInfo);
    }
}

void AppsManager::pushPixmap(const ItemInfo &itemInfo)
{
    const int s = 8;
    const int l[s] = { 16, 18, 24, 32, 64, 96, 128, 256 };

    for (int i = 0; i < s; i++) {
        QPair<QString, int> tmpKey { cacheKey(itemInfo, CacheType::ImageType), l[i]};
        if (m_CacheData[tmpKey].isNull()) {
            getThemeIcon(itemInfo, l[i]);
        }
    }
}

const QScreen *AppsManager::currentScreen()
{
    QRect dockRect = dockGeometry();

    const auto ratio = qApp->devicePixelRatio();
    for (auto *screen : qApp->screens()) {
        const QRect &sg = screen->geometry();
        const QRect &rg = QRect(sg.topLeft(), sg.size() * ratio);
        if (rg.contains(dockRect.topLeft())) {
            return  screen;
        }
    }

    return qApp->primaryScreen();
}

int AppsManager::getVisibleCategoryCount()
{
    int ret = 0;

    for (int i = AppsListModel::Internet; i <= AppsListModel::Others; i++) {
        AppsListModel::AppCategory appCategory = AppsListModel::AppCategory(i);

        if (appsInfoListSize(appCategory) > 0) {
            ret++;
        }
    }

    return ret;
}

QString AppsManager::cacheKey(const ItemInfo &info, CacheType type)
{
    QString key = info.m_name + info.m_iconKey + QString::number(type);
    return key;
}

/*
 * Copyright 2013 Giulio Camuffo <giuliocamuffo@gmail.com>
 *
 * This file is part of Orbital
 *
 * Orbital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Orbital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Orbital.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QDir>
#include <QStandardPaths>
#include <QDebug>
#include <QQmlComponent>
#include <QJsonDocument>
#include <QJsonObject>

#include "style.h"

QMap<QString, StyleInfo *> Style::s_styles;

Style::Style(QObject *p)
     : QObject(p)
     , m_panelBackground(nullptr)
     , m_panelBorder(nullptr)
     , m_taskBarBackground(nullptr)
     , m_taskBarItem(nullptr)
     , m_pagerBackground(nullptr)
     , m_pagerWorkspace(nullptr)
     , m_toolTipBackground(nullptr)
     , m_button(nullptr)
     , m_popup(nullptr)
     , m_popupLauncher(nullptr)
     , m_notificationBackground(nullptr)
{

}

Style *Style::loadStyle(const QString &name, QQmlEngine *engine)
{
    if (!s_styles.contains(name)) {
        qWarning() << "Could not find the style" << name;
        return nullptr;
    }

    StyleInfo *info = s_styles.value(name);

    QQmlComponent c(engine);
    c.loadUrl(info->m_qml);
    if (!c.isReady()) {
        qWarning() << "Could not load the style" << name;
        qWarning() << qPrintable(c.errorString());
        return nullptr;
    }

    QObject *obj = c.create();
    Style *style = qobject_cast<Style *>(obj);
    if (!style) {
        qWarning() << QStringLiteral("\'%1\' is not a style type.").arg(name);
        delete obj;
        return nullptr;
    }

    return style;
}

void Style::loadStylesList()
{
    const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::DataLocation);
    for (const QString &path: dirs) {
        QDir dir(QUrl(QStringLiteral("%1/../orbital/styles").arg(path)).toString(QUrl::NormalizePathSegments));
        const QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &subdir: subdirs) {
            dir.cd(subdir);
            if (dir.exists(QStringLiteral("style"))) {
                if (!s_styles.contains(subdir)) {
                    loadStyleInfo(subdir, dir.absolutePath());
                }
            }
            dir.cdUp();
        }
    }
}

void Style::cleanupStylesList()
{
    foreach (StyleInfo *info, s_styles) {
        delete info;
    }
}

void Style::loadStyleInfo(const QString &name, const QString &path)
{
    QString filePath(path + "/style");
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << QStringLiteral("Failed to load the style '%1'. Could not open %1 for reading.").arg(filePath);
        return;
    }

    StyleInfo *info = new StyleInfo;
    info->m_name = name;
    info->m_path = path;
    info->m_prettyName = name;

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning("Error parsing %s at offset %d: %s", qPrintable(filePath), error.offset, qPrintable(error.errorString()));
        delete info;
        return;
    }
    QJsonObject json = doc.object();

    if (json.contains(QStringLiteral("prettyName"))) {
        info->m_prettyName = json.value(QStringLiteral("prettyName")).toString();
    }
    if (json.contains(QStringLiteral("qmlFile"))) {
        info->m_qml = path + "/" + json.value(QStringLiteral("qmlFile")).toString();
    }

    if (info->m_qml.isEmpty()) {
        qWarning() << QStringLiteral("Failed to load the style '%1'. Missing 'qmlFile' field.").arg(path);
        delete info;
        return;
    }

    s_styles.insert(name, info);
}
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

#ifndef SHELLUI_H
#define SHELLUI_H

#include <QObject>
#include <QStringList>

class QXmlStreamReader;
class QXmlStreamWriter;
class QQuickItem;
class QQmlEngine;

class Element;
class Client;
class UiScreen;

class ShellUI : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString iconTheme READ iconTheme WRITE setIconTheme)
    Q_PROPERTY(bool configMode READ configMode WRITE setConfigMode NOTIFY configModeChanged)
public:
    ShellUI(Client *client, QQmlEngine *engine, const QString &configFile);
    ~ShellUI();

    UiScreen *loadScreen(int screen);
    QQmlEngine *qmlEngine() const { return m_engine; }

    QString iconTheme() const;
    void setIconTheme(const QString &theme);

    bool configMode() const { return m_configMode; }
    void setConfigMode(bool mode);

    Q_INVOKABLE void setOverrideCursorShape(Qt::CursorShape shape);
    Q_INVOKABLE void restoreOverrideCursorShape();

    Q_INVOKABLE Element *createElement(const QString &name);
    Q_INVOKABLE void toggleConfigMode();

public slots:
    void requestFocus(QQuickItem *item);
    void reloadConfig();
    void saveConfig();

signals:
    void configModeChanged();

private:
    void loadScreen(UiScreen *s);

    Client *m_client;
    QString m_configFile;
    QByteArray m_configData;
    bool m_configMode;
    int m_cursorShape;
    QQmlEngine *m_engine;
    QList<UiScreen *> m_screens;

    QStringList m_properties;
};

#endif

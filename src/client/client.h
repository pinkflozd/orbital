/*
 * Copyright 2013-2014 Giulio Camuffo <giuliocamuffo@gmail.com>
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

#ifndef CLIENT_H
#define CLIENT_H

#include <functional>

#include <QObject>
#include <QElapsedTimer>

class QQmlEngine;
class QQmlComponent;
class QWindow;
class QQuickWindow;
class QScreen;
class QProcess;

struct wl_display;
struct wl_registry;
struct wl_surface;
struct wl_registry_listener;
struct wl_output;
struct wl_subcompositor;
struct wl_subsurface;

struct desktop_shell;
struct desktop_shell_listener;
struct desktop_shell_binding;
struct desktop_shell_window;
struct desktop_shell_workspace;
struct desktop_shell_surface;
struct desktop_shell_panel;
struct notifications_manager;
struct notification_surface;
struct active_region;

class Window;
class ShellUI;
class Grab;
class Workspace;
class ElementInfo;
class StyleInfo;
class Element;
class CompositorSettings;
class UiScreen;

class Binding : public QObject
{
    Q_OBJECT
public:
    ~Binding();

signals:
    void triggered();

private:
    desktop_shell_binding *bind;
    friend class Client;
};

class Client : public QObject
{
    Q_OBJECT
    Q_PRIVATE_PROPERTY(Client::d_func(), QQmlListProperty<Window> windows READ windows NOTIFY windowsChanged)
    Q_PRIVATE_PROPERTY(Client::d_func(), QQmlListProperty<Workspace> workspaces READ workspaces NOTIFY workspacesChanged)
    Q_PRIVATE_PROPERTY(Client::d_func(), QQmlListProperty<ElementInfo> elementsInfo READ elementsInfo NOTIFY elementsInfoChanged)
    Q_PRIVATE_PROPERTY(Client::d_func(), QQmlListProperty<StyleInfo> stylesInfo READ stylesInfo NOTIFY stylesInfoChanged)
public:
    Client();
    ~Client();

    void quit();
    void lockSession();
    void unlockSession();
    bool isSessionLocked() const;

    QQuickWindow *findWindow(wl_surface *surface) const;
    desktop_shell_surface *setPopup(QWindow *p, QWindow *parent);

    Q_INVOKABLE Binding *addKeyBinding(uint32_t key, Qt::KeyboardModifiers modifiers);

    Q_INVOKABLE static Grab *createGrab();
    static QQuickWindow *createUiWindow();
    QQuickWindow *window(Element *ele);
    QQmlEngine *qmlEngine() const { return m_engine; }

    static Client *client() { return s_client; }
    static QLocale locale();

    void setBackground(QQuickWindow *window, QScreen *screen);
    desktop_shell_panel *setPanel(QQuickWindow *window, QScreen *screen, int location);
    notification_surface *pushNotification(QWindow *window, bool inactive);
    active_region *createActiveRegion(QQuickWindow *window, const QRect &rect);
    wl_subsurface *getSubsurface(QQuickWindow *window, QQuickWindow *parent);
    void addOverlay(QQuickWindow *window, QScreen *screen);
    void setInputRegion(QQuickWindow *w, const QRectF &region);
    QProcess *createTrustedClient(const QString &interface);
    void setLockScreen(QQuickWindow *window, QScreen *screen);

    static wl_output *nativeOutput(QScreen *screen);

    bool event(QEvent *e) override;

    void addAction(const QByteArray &name, const std::function<void ()> &action);
    std::function<void ()> *action(const QByteArray &name);

public slots:
    void minimizeWindows();
    void restoreWindows();
    void addWorkspace(int n);
    void removeWorkspace(int n);
    void selectWorkspace(UiScreen *screen, Workspace *ws);

signals:
    void windowsChanged();
    void windowAdded(Window *window);
    void windowRemoved(Window *window);
    void workspacesChanged();
    void elementsInfoChanged();
    void stylesInfoChanged();
    void locked();
    bool unlocked();

private slots:
    void create();
    void windowDestroyed(Window *w);
    void setGrabCursor();
    void sendOutputLoaded(uint32_t serial);
    void loadOutput(QScreen *screen, const QString &name, uint32_t serial);

private:
    void handleGlobal(wl_registry *registry, uint32_t id, const char *interface, uint32_t version);
    void handlePing(desktop_shell *shell, uint32_t serial);
    void handleLoad(desktop_shell *shell);
    void handleConfigure(desktop_shell *shell, uint32_t edges, wl_surface *surf, int32_t width, int32_t height);
    void handlePrepareLockSurface(desktop_shell *desktop_shell);
    void handleGrabCursor(desktop_shell *desktop_shell, uint32_t cursor);
    void handleWindowAdded(desktop_shell *desktop_shell, desktop_shell_window *window, uint32_t pid);
    void handleWorkspaceAdded(desktop_shell *desktop_shell, desktop_shell_workspace *ws);
    void handleDesktopRect(desktop_shell *desktop_shell, wl_output *output, int32_t x, int32_t y, int32_t width, int32_t height);
    void handleLocked(desktop_shell *desktop_shell);
    void handleUnlocked(desktop_shell *desktop_shell);
    void addUiWindow(QQuickWindow *w);
    void setGrabSurface();
    void screenAdded(QScreen *s);

    static const wl_registry_listener s_registryListener;
    static const desktop_shell_listener s_shellListener;

    wl_display *m_display;
    wl_registry *m_registry;
    int m_fd;
    desktop_shell *m_shell;
    notifications_manager *m_notifications;
    wl_subcompositor *m_subcompositor;
    CompositorSettings *m_settings;
    QQmlEngine *m_engine;
    QWindow *m_grabWindow;
    QList<Binding *> m_bindings;
    QList<QQuickWindow *> m_uiWindows;
    QElapsedTimer m_elapsedTimer;
    ShellUI *m_ui;

    QList<Window *> m_windows;
    QList<Workspace *> m_workspaces;
    QHash<QByteArray, std::function<void ()>> m_actions;

    uint32_t m_pendingGrabCursor;

    static Client *s_client;

    class ClientPrivate *const d_ptr;
    Q_DECLARE_PRIVATE(Client)
};

#endif

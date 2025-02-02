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

#include <unistd.h>

#include <QDebug>
#include <QFileInfo>
#include <QSettings>

#include "desktop-shell-window.h"
#include "../shell.h"
#include "../shellsurface.h"
#include "desktop-shell.h"
#include "../seat.h"
#include "../compositor.h"
#include "../shellview.h"
#include "../layer.h"
#include "../output.h"
#include "../pager.h"
#include "../focusscope.h"

#include "wayland-desktop-shell-server-protocol.h"

namespace Orbital {

DesktopShellWindow::DesktopShellWindow(DesktopShell *ds)
                  : Interface()
                  , m_desktopShell(ds)
                  , m_resource(nullptr)
                  , m_state(DESKTOP_SHELL_WINDOW_STATE_INACTIVE)
                  , m_sendState(true)
{
}

DesktopShellWindow::~DesktopShellWindow()
{
    destroy();
}

void DesktopShellWindow::added()
{
    connect(shsurf(), &ShellSurface::mapped, this, &DesktopShellWindow::mapped);
    connect(shsurf(), &ShellSurface::contentLost, this, &DesktopShellWindow::destroy);
    connect(shsurf(), &ShellSurface::titleChanged, this, &DesktopShellWindow::sendTitle);
    connect(shsurf()->surface(), &Surface::activated, this, &DesktopShellWindow::activated);
    connect(shsurf()->surface(), &Surface::deactivated, this, &DesktopShellWindow::deactivated);
    connect(shsurf(), &ShellSurface::minimized, this, &DesktopShellWindow::minimized);
    connect(shsurf(), &ShellSurface::restored, this, &DesktopShellWindow::restored);
}

ShellSurface *DesktopShellWindow::shsurf()
{
    return static_cast<ShellSurface *>(object());
}

void DesktopShellWindow::mapped()
{
    if (m_resource) {
        return;
    }

    create();
}

void DesktopShellWindow::surfaceTypeChanged()
{
//     ShellSurface::Type type = shsurf()->type();
//     if (type == ShellSurface::Type::TopLevel && !shsurf()->isTransient()) {
//         if (!m_resource) {
//             create();
//         }
//     } else {
//         destroy();
//     }
}

void DesktopShellWindow::activated(Seat *)
{
    m_state |= DESKTOP_SHELL_WINDOW_STATE_ACTIVE;
    sendState();
}

void DesktopShellWindow::deactivated(Seat *)
{
    m_state &= ~DESKTOP_SHELL_WINDOW_STATE_ACTIVE;
    sendState();
}

void DesktopShellWindow::minimized()
{
    m_state |= DESKTOP_SHELL_WINDOW_STATE_MINIMIZED;
    sendState();
}

void DesktopShellWindow::restored()
{
    m_state &= ~DESKTOP_SHELL_WINDOW_STATE_MINIMIZED;
    sendState();
}

void DesktopShellWindow::create()
{
    if (!m_desktopShell->resource()) {
        return;
    }

    if (!m_desktopShell->shell()->isSurfaceActive(shsurf())) {
        return;
    }

    static const struct desktop_shell_window_interface implementation = {
        wrapInterface(&DesktopShellWindow::setState),
        wrapInterface(&DesktopShellWindow::close),
        wrapInterface(&DesktopShellWindow::preview),
        wrapInterface(&DesktopShellWindow::endPreview),
    };

    m_resource = wl_resource_create(m_desktopShell->client(), &desktop_shell_window_interface, 1, 0);
    wl_resource_set_implementation(m_resource, &implementation, this, [](wl_resource *res) {
        DesktopShellWindow *win = static_cast<DesktopShellWindow *>(wl_resource_get_user_data(res));
        win->m_resource = nullptr;
    });

    QString title = shsurf()->title();
    if (title.isEmpty()) {
        QFileInfo exe(QStringLiteral("/proc/%1/exe").arg(shsurf()->pid()));
        title = QFileInfo(exe.symLinkTarget()).fileName();
    }
    QString icon;
    if (!shsurf()->appId().isEmpty()) {
        QString appId = shsurf()->appId().replace('-', '/').remove(QStringLiteral(".desktop"));
        static QString xdgDataDir = []() {
            QString s = qgetenv("XDG_DATA_DIRS");
            if (s.isEmpty()) {
                s = QStringLiteral("/usr/share");
            }
            return s;
        }();
        foreach (const QString &d, xdgDataDir.split(':')) {
            QString path = QStringLiteral("%1/applications/%2.desktop").arg(d, appId);
            if (QFile::exists(path)) {
                QSettings settings(path, QSettings::IniFormat);
                settings.beginGroup(QStringLiteral("Desktop Entry"));
                icon = settings.value(QStringLiteral("Icon")).toString();
                settings.endGroup();
                break;
            }
        }
    }

    desktop_shell_send_window_added(m_desktopShell->resource(), m_resource, shsurf()->pid());
    desktop_shell_window_send_title(m_resource, qPrintable(title));
    desktop_shell_window_send_icon(m_resource, qPrintable(icon));
    desktop_shell_window_send_state(m_resource, m_state);
}

void DesktopShellWindow::destroy()
{
    if (m_resource) {
        desktop_shell_window_send_removed(m_resource);
        wl_resource_set_implementation(m_resource, nullptr, nullptr, nullptr);
        m_resource = nullptr;
    }
}

void DesktopShellWindow::sendState()
{
    if (m_resource && m_sendState) {
        desktop_shell_window_send_state(m_resource, m_state);
    }
}

void DesktopShellWindow::sendTitle()
{
    if (m_resource) {
        desktop_shell_window_send_title(m_resource, qPrintable(shsurf()->title()));
    }
}

void DesktopShellWindow::setState(wl_client *client, wl_resource *resource, wl_resource *output, int32_t state)
{
    ShellSurface *s = shsurf();
    FocusScope *scope = m_desktopShell->shell()->appsFocusScope();

    m_sendState = false;
    if (m_state & DESKTOP_SHELL_WINDOW_STATE_MINIMIZED && !(state & DESKTOP_SHELL_WINDOW_STATE_MINIMIZED)) {
        scope->activate(s->surface());
        s->restore();
    } else if (state & DESKTOP_SHELL_WINDOW_STATE_MINIMIZED && !(m_state & DESKTOP_SHELL_WINDOW_STATE_MINIMIZED)) {
        s->minimize();
    }

    if (state & DESKTOP_SHELL_WINDOW_STATE_ACTIVE && !(state & DESKTOP_SHELL_WINDOW_STATE_MINIMIZED)) {
        s->workspace()->activate(Output::fromResource(output));
        scope->activate(s->surface());
        foreach (Output *o, m_desktopShell->compositor()->outputs()) {
            ShellView *view = s->viewForOutput(o);
            if (Layer *layer = view->layer()) {
                layer->raiseOnTop(view);
            }
        }
    }

    m_sendState = true;
    sendState();
}

void DesktopShellWindow::close(wl_client *client, wl_resource *resource)
{
    shsurf()->close();
}

void DesktopShellWindow::preview(wl_resource *output)
{
    shsurf()->preview(Output::fromResource(output));
}

void DesktopShellWindow::endPreview(wl_resource *output)
{
    shsurf()->endPreview(Output::fromResource(output));
}

}

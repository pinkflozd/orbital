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

#include <linux/input.h>

#include <QDebug>

#include <wayland-server.h>

#include "desktop-shell.h"
#include "../shell.h"
#include "../compositor.h"
#include "../utils.h"
#include "../output.h"
#include "../workspace.h"
#include "../view.h"
#include "../seat.h"
#include "../binding.h"
#include "../global.h"
#include "../layer.h"
#include "../shellsurface.h"
#include "../pager.h"
#include "../dummysurface.h"
#include "../focusscope.h"
#include "desktop-shell-workspace.h"
#include "desktop-shell-splash.h"
#include "desktop-shell-window.h"
#include "desktop-shell-notifications.h"
#include "desktop-shell-launcher.h"
#include "desktop-shell-settings.h"
#include "wayland-desktop-shell-server-protocol.h"

namespace Orbital {

DesktopShell::DesktopShell(Shell *shell)
            : Interface(shell)
            , Global(shell->compositor(), &desktop_shell_interface, 1)
            , m_shell(shell)
            , m_grabView(nullptr)
            , m_splash(new DesktopShellSplash(shell))
            , m_loadSerial(0)
            , m_loaded(false)
            , m_loadedOnce(false)
            , m_lockRequested(false)
{
    m_shell->addInterface(new DesktopShellNotifications(shell));
    m_shell->addInterface(new DesktopShellLauncher(shell));
    m_shell->addInterface(new DesktopShellSettings(shell));
    m_shell->addInterface(m_splash);

    m_client = shell->compositor()->launchProcess(QStringLiteral(LIBEXEC_PATH "/startorbital"));
    m_client->setAutoRestart(true);
    connect(m_client, &ChildProcess::givingUp, this, &DesktopShell::givingUp);

    shell->setGrabCursorSetter([this](Pointer *p, PointerCursor c) { setGrabCursor(p, c); });
    shell->setGrabCursorUnsetter([this](Pointer *p) { unsetGrabCursor(p); });
    connect(shell->compositor(), &Compositor::sessionActivated, this, &DesktopShell::session);

    KeyBinding *b = shell->compositor()->createKeyBinding(KEY_BACKSPACE, KeyboardModifiers::Super);
    connect(b, &KeyBinding::triggered, [this]() {
        m_client->restart();
    });
}

DesktopShell::~DesktopShell()
{
    delete m_grabView;
}

Compositor *DesktopShell::compositor() const
{
    return m_shell->compositor();
}

wl_client *DesktopShell::client() const
{
    return m_client->client();
}

void DesktopShell::bind(wl_client *client, uint32_t version, uint32_t id)
{
    wl_resource *resource = wl_resource_create(client, &desktop_shell_interface, version, id);
    if (client != m_client->client()) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "permission to bind desktop_shell denied");
        wl_resource_destroy(resource);
        return;
    }

    static const struct desktop_shell_interface implementation = {
        wrapInterface(&DesktopShell::setBackground),
        wrapInterface(&DesktopShell::setPanel),
        wrapInterface(&DesktopShell::setLockSurface),
        wrapInterface(&DesktopShell::setPopup),
        wrapInterface(&DesktopShell::lock),
        wrapInterface(&DesktopShell::unlock),
        wrapInterface(&DesktopShell::setGrabSurface),
        wrapInterface(&DesktopShell::addKeyBinding),
        wrapInterface(&DesktopShell::addOverlay),
        wrapInterface(&DesktopShell::minimizeWindows),
        wrapInterface(&DesktopShell::restoreWindows),
        wrapInterface(&DesktopShell::createGrab),
        wrapInterface(&DesktopShell::addWorkspace),
        wrapInterface(&DesktopShell::selectWorkspace),
        wrapInterface(&DesktopShell::quit),
        wrapInterface(&DesktopShell::pong),
        wrapInterface(&DesktopShell::outputLoaded),
        wrapInterface(&DesktopShell::createActiveRegion),
        wrapInterface(&DesktopShell::outputBound)
    };

    wl_resource_set_implementation(resource, &implementation, this, [](wl_resource *res) {
        static_cast<DesktopShell *>(wl_resource_get_user_data(res))->clientExited();
    });
    m_resource = resource;

    foreach (Workspace *ws, m_shell->workspaces()) {
        DesktopShellWorkspace *dws = ws->findInterface<DesktopShellWorkspace>();
        dws->init(m_client->client(), 0);
        desktop_shell_send_workspace_added(m_resource, dws->resource());
        dws->sendActivatedState();
        dws->sendPosition();
    }
    foreach (ShellSurface *shsurf, m_shell->surfaces()) {
        DesktopShellWindow *w = shsurf->findInterface<DesktopShellWindow>();
        if (w) {
            w->create();
        }
    }
    if (m_shell->isLocked() && m_lockRequested) {
        desktop_shell_send_locked(resource);
    }

    desktop_shell_send_load(resource);
}

void DesktopShell::clientExited()
{
    m_resource = nullptr;
    m_grabView = nullptr;
    m_loaded = false;
}

void DesktopShell::session(bool active)
{
    if (active && !m_lockRequested) {
        m_shell->unlock();
    }
}

void DesktopShell::givingUp()
{
    if (!m_loadedOnce) {
        m_shell->compositor()->quit();
    }
}

void DesktopShell::setGrabCursor(Pointer *p, PointerCursor c)
{
    m_grabCursor[p] = c;
    if (m_resource) {
        p->setFocus(m_grabView, 0, 0);
        desktop_shell_send_grab_cursor(m_resource, (uint32_t)c);
    }
}

void DesktopShell::unsetGrabCursor(Pointer *p)
{
    m_grabCursor.remove(p);
}

void DesktopShell::outputBound(uint32_t id, wl_resource *res)
{
    wl_resource *r = wl_resource_create(m_client->client(), &desktop_shell_output_feedback_interface, 1, id);
    Output *o = Output::fromResource(res);
    m_loadSerial = m_shell->compositor()->nextSerial();
    desktop_shell_output_feedback_send_load(r, qPrintable(o->name()), m_loadSerial);
    wl_resource_destroy(r);

    foreach (Workspace *ws, m_shell->workspaces()) {
        DesktopShellWorkspace *dws = ws->findInterface<DesktopShellWorkspace>();
        dws->sendActivatedState();
        dws->sendPosition();
    }
}

void DesktopShell::setBackground(wl_resource *outputResource, wl_resource *surfaceResource)
{
    Output *output = Output::fromResource(outputResource);
    Surface *surface = Surface::fromResource(surfaceResource);

    if (surface->setRole("desktop_shell_background_surface", m_resource, DESKTOP_SHELL_ERROR_ROLE)) {
        output->setBackground(surface);
        surface->setLabel(QStringLiteral("background"));
    }
}

void DesktopShell::setPanel(uint32_t id, wl_resource *outputResource, wl_resource *surfaceResource, uint32_t position)
{
    Output *output = Output::fromResource(outputResource);
    Surface *surface = Surface::fromResource(surfaceResource);

    if (!surface->setRole("desktop_shell_panel_surface", m_resource, DESKTOP_SHELL_ERROR_ROLE)) {
        return;
    }

    surface->setLabel(QStringLiteral("panel"));

    class Panel {
    public:
        Panel(wl_resource *res, Surface *s, Output *o, int p)
            : m_resource(res)
            , m_surface(s)
            , m_output(o)
//             , m_grab(nullptr)
        {
            struct desktop_shell_panel_interface implementation = {
                wrapInterface(&Panel::move),
                wrapInterface(&Panel::setPosition)
            };

            wl_resource_set_implementation(m_resource, &implementation, this, panelDestroyed);

            setPosition(p);
        }

        void move(wl_client *client, wl_resource *resource)
        {
//             m_grab = new PanelGrab;
//             m_grab->panel = this;
//             weston_seat *seat = container_of(m_shell->compositor()->seat_list.next, weston_seat, link);
//             m_grab->start(seat);
        }
        void setPosition(uint32_t pos)
        {
            m_pos = pos;
            m_output->setPanel(m_surface, m_pos);
        }

        static void panelDestroyed(wl_resource *res)
        {
            Panel *_this = static_cast<Panel *>(wl_resource_get_user_data(res));
//             delete _this->m_grab;
            delete _this;
        }

        wl_resource *m_resource;
        Surface *m_surface;
        Output *m_output;
        int m_pos;
//         PanelGrab *m_grab;
    };

    wl_resource *res = wl_resource_create(m_client->client(), &desktop_shell_panel_interface, 1, id);
    new Panel(res, surface, output, position);
}

void DesktopShell::setLockSurface(wl_resource *surfaceResource, wl_resource *outputResource)
{
    Surface *surface = Surface::fromResource(surfaceResource);

    if (!surface->setRole("desktop_shell_lock_surface", m_resource, DESKTOP_SHELL_ERROR_ROLE)) {
        return;
    }

    Output *output = Output::fromResource(outputResource);
    output->setLockSurface(surface);
    surface->setLabel(QStringLiteral("lock"));
    m_shell->lockFocusScope()->activate(surface);
}

void DesktopShell::setPopup(uint32_t id, wl_resource *parentResource, wl_resource *surfaceResource, int x, int y)
{
    Surface *parent = Surface::fromResource(parentResource);
    Surface *surface = Surface::fromResource(surfaceResource);

    if (!surface->setRole("desktop_shell_popup_surface", m_resource, DESKTOP_SHELL_ERROR_ROLE)) {
        return;
    }

    surface->setLabel(QStringLiteral("popup"));
    wl_resource *resource = wl_resource_create(m_client->client(), &desktop_shell_surface_interface, wl_resource_get_version(m_resource), id);

    class Popup : public Surface::RoleHandler
    {
    public:
        Popup(Surface *s, Surface *p, wl_resource *r, int x_, int y_)
            : surface(s)
            , parent(p)
            , resource(r)
            , x(x_)
            , y(y_)
        {
            s->setRoleHandler(this);
        }
        ~Popup() {
            qDeleteAll(surface->views());
            delete grab;
        }
        void configure(int, int) override
        {
            if (surface->width() == 0) {
                return;
            }

            if (surface->views().isEmpty()) {
                foreach (View *view, parent->views()) {
                    View *v = new View(surface);
                    v->setTransformParent(view);
                    view->layer()->addView(v);
                    v->setOutput(view->output());
                    configureView(v);

                    connect(view, &QObject::destroyed, v, &QObject::deleteLater);
                }
            } else {
                foreach (View *view, surface->views()) {
                    configureView(view);
                }
            }
        }
        void configureView(View *v)
        {
            int w = surface->width();
            int h = surface->height();
            Output *o = v->output();
            int x_ = x > 0 ? x : 0;
            if (x_ + w > o->width()) {
                x_ = o->width() - w;
            }
            int y_ = y > 0 ? y : 0;
            if (y_ + h > o->height()) {
                y_ = o->height() - h;
            }
            v->setPos(x_, y_);
        }
        void move(Seat *) override {}

        Surface *surface;
        Surface *parent;
        wl_resource *resource;
        PointerGrab *grab;
        int x, y;
    };

    class PopupGrab : public PointerGrab {
    public:
        void focus() override
        {
            if (pointer()->buttonCount() > 0) {
                return;
            }

            double sx, sy;
            View *v = pointer()->pickView(&sx, &sy);

            inside = popup->surface->views().contains(v);
            if (inside) {
                if (pointer()->focus() != v) {
                    pointer()->setFocus(v, sx, sy);
                }
            } else {
                pointer()->setFocus(nullptr);
            }
        }
        void motion(uint32_t time, double x, double y) override
        {
            pointer()->move(x, y);
            pointer()->sendMotion(time);
        }
        void button(uint32_t time, PointerButton button, Pointer::ButtonState state) override
        {
            pointer()->sendButton(time, button, state);

            // this time check is to ensure the window doesn't get shown and hidden very fast, mainly because
            // there is a bug in QQuickWindow, which hangs up the process.
            if (!inside && state == Pointer::ButtonState::Pressed && time - creationTime > 500) {
                desktop_shell_surface_send_popup_close(popup->resource);
                end();
            }
        }

        Popup *popup;
        bool inside;
        uint32_t creationTime;
    };

    Popup *popup = new Popup(surface, parent, resource, x, y);

    static const struct desktop_shell_surface_interface implementation = {
        [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }
    };
    wl_resource_set_implementation(resource, &implementation, popup, [](wl_resource *r) {
        delete static_cast<Popup *>(wl_resource_get_user_data(r));
    });

    Seat *seat = m_shell->compositor()->seats().first();
    PopupGrab *grab = new PopupGrab;
    popup->grab = grab;
    grab->popup = popup;
    grab->creationTime = seat->pointer()->grabTime();
    grab->start(seat);
}

void DesktopShell::lock()
{
    if (m_shell->isLocked()) {
        desktop_shell_send_locked(m_resource);
    } else {
        m_shell->lock([this]() {
            desktop_shell_send_locked(m_resource);
        });
    }
    m_lockRequested = true;
}

void DesktopShell::unlock()
{
    m_shell->unlock();
    m_lockRequested = false;
}

void DesktopShell::setGrabSurface(wl_resource *surfaceResource)
{
    Surface *surface = Surface::fromResource(surfaceResource);

    if (!surface->setRole("desktop_shell_grab_surface", m_resource, DESKTOP_SHELL_ERROR_ROLE)) {
        return;
    }

    if (m_grabView) {
        if (surface == m_grabView->surface()) {
            return;
        }
        delete m_grabView;
    }

    m_grabView = new View(surface);
}

void DesktopShell::addKeyBinding(uint32_t id, uint32_t key, uint32_t modifiers)
{
    class Binding : public QObject
    {
    public:
        ~Binding() {
            resource = nullptr;
            delete binding;
        }

        void triggered(Seat *seat, uint32_t time, uint32_t key)
        {
            desktop_shell_binding_send_triggered(resource);
        }

        wl_resource *resource;
        KeyBinding *binding;
    };

    Binding *b = new Binding;
    b->resource = wl_resource_create(m_client->client(), &desktop_shell_binding_interface, wl_resource_get_version(m_resource), id);
    wl_resource_set_implementation(b->resource, nullptr, b, [](wl_resource *r) {
        delete static_cast<Binding *>(wl_resource_get_user_data(r));
    });

    b->binding = m_shell->compositor()->createKeyBinding(key, (KeyboardModifiers)modifiers);
    connect(b->binding, &KeyBinding::triggered, b, &Binding::triggered);
}

void DesktopShell::addOverlay(wl_resource *outputResource, wl_resource *surfaceResource)
{
    Output *output = Output::fromResource(outputResource);
    Surface *surface = Surface::fromResource(surfaceResource);

    if (surface->setRole("desktop_shell_overlay_surface", m_resource, DESKTOP_SHELL_ERROR_ROLE)) {
        output->setOverlay(surface);
        surface->setLabel(QStringLiteral("overlay"));
    }
}

void DesktopShell::minimizeWindows()
{

}

void DesktopShell::restoreWindows()
{

}

void DesktopShell::createGrab(uint32_t id)
{
    class ClientGrab : public PointerGrab
    {
    public:
        void focus() override
        {
            double sx, sy;
            View *view = pointer()->pickView(&sx, &sy);
            if (view->surface()->client() != client) {
                return;
            }
            if (currentFocus != view) {
                currentFocus = view;
                desktop_shell_grab_send_focus(resource, view->surface()->surface()->resource,
                                              wl_fixed_from_double(sx), wl_fixed_from_double(sy));
            }
        }
        void motion(uint32_t time, double x, double y) override
        {
            pointer()->move(x, y);

            QPointF p(pointer()->x(), pointer()->y());
            if (currentFocus) {
                p = currentFocus->mapFromGlobal(p);
            }
            desktop_shell_grab_send_motion(resource, time, wl_fixed_from_double(p.x()), wl_fixed_from_double(p.y()));
        }
        void button(uint32_t time, PointerButton button, Pointer::ButtonState state) override
        {
            // Send the event to the application as normal if the mouse was pressed initially.
            // The application has to know the button was released, otherwise its internal state
            // will be inconsistent with the physical button state.
            // Eat the other events, as the app doesn't need to know them.
            // NOTE: this works only if there is only 1 button pressed initially. i can know how many button
            // are pressed but weston currently has no API to determine which ones they are.
            if (pressed && button == pointer()->grabButton()) {
                pointer()->sendButton(time, button, state);
                pressed = false;
            }
            desktop_shell_grab_send_button(resource, time, pointerButtonToRaw(button), (int)state);
        }
        void ended() override
        {
            if (resource) {
                desktop_shell_grab_send_ended(resource);
            }
        }

        void terminate()
        {
            resource = nullptr;
            end();
        }

        wl_resource *resource;
        View *currentFocus;
        wl_client *client;
        bool pressed;
    };

    static const struct desktop_shell_grab_interface desktop_shell_grab_implementation = {
        wrapInterface(&ClientGrab::terminate)
    };


    ClientGrab *grab = new ClientGrab;
    wl_resource *res = wl_resource_create(m_client->client(), &desktop_shell_grab_interface, wl_resource_get_version(m_resource), id);
    wl_resource_set_implementation(res, &desktop_shell_grab_implementation, grab, [](wl_resource *res) {
        delete static_cast<ClientGrab *>(wl_resource_get_user_data(res));
    });

    Seat *seat = m_shell->compositor()->seats().first();
    grab->resource = res;
    grab->pressed = seat->pointer()->buttonCount() > 0;

    double sx, sy;
    View *view = seat->pointer()->pickView(&sx, &sy);
    grab->currentFocus = view;
    grab->client = m_client->client();
    grab->start(seat);

    seat->pointer()->setFocus(view, sx, sy);
    desktop_shell_grab_send_focus(grab->resource, view->surface()->surface()->resource,
                                  wl_fixed_from_double(sx), wl_fixed_from_double(sy));
}

void DesktopShell::addWorkspace(uint32_t id)
{
    Workspace *ws = m_shell->createWorkspace();
    DesktopShellWorkspace *dws = ws->findInterface<DesktopShellWorkspace>();
    dws->init(m_client->client(), id);
    dws->sendActivatedState();
    dws->sendPosition();
}

void DesktopShell::selectWorkspace(wl_resource *outputResource, wl_resource *workspaceResource)
{
    Output *output = Output::fromResource(outputResource);
    DesktopShellWorkspace *dws = DesktopShellWorkspace::fromResource(workspaceResource);
    Workspace *ws = dws->workspace();
    qDebug()<<"select"<<output<<ws;
//     output->viewWorkspace(ws);

    m_shell->pager()->activate(ws, output);
}

void DesktopShell::quit()
{
    m_shell->compositor()->quit();
}

void DesktopShell::pong(uint32_t serial)
{
}

void DesktopShell::outputLoaded(uint32_t serial)
{
    if (serial > 0 && serial == m_loadSerial && !m_loaded) {
        m_splash->hide();
        m_loaded = true;
        m_loadedOnce = true;
        m_loadSerial = 0;

        for (auto i = m_grabCursor.begin(); i != m_grabCursor.end(); ++i) {
            setGrabCursor(i.key(), i.value());
        }
    }
}

void DesktopShell::createActiveRegion(uint32_t id, wl_resource *parentResource, int32_t x, int32_t y, int32_t width, int32_t height)
{
    wl_resource *res = wl_resource_create(m_client->client(), &active_region_interface, 1, id);

    class ActiveRegion : public DummySurface
    {
    public:
        class ActiveView : public View
        {
        public:
            ActiveView(Surface *s, View *p)
                : View(s), parent(p)
            {
                connect(p, &QObject::destroyed, this, &ActiveView::destroy);
            }
            View *pointerEnter(const Pointer *pointer) override
            {
                return parent;
            }
            void destroy()
            {
                delete this;
            }
            View *parent;
        };

        ActiveRegion(Compositor *c, wl_resource *resource, Surface *parent, int x, int y, int w, int h)
            : DummySurface(c, w, h)
            , m_resource(resource)
            , m_parent(parent)
        {
            foreach (View *view, parent->views()) {
                ActiveView *v = new ActiveView(this, view);
                v->setAlpha(0.);
                v->setTransformParent(view);
                v->setPos(x, y);
                view->layer()->addView(v);
            }

            static const struct active_region_interface implementation = {
                wrapInterface(&ActiveRegion::destroy),
                wrapInterface(&ActiveRegion::setGeometry)
            };
            wl_resource_set_implementation(resource, &implementation, this, [](wl_resource *r) {
                ActiveRegion *region = static_cast<ActiveRegion *>(wl_resource_get_user_data(r));
                delete region;
            });
        }
        ~ActiveRegion()
        {
        }
        void destroy()
        {
            wl_resource_destroy(m_resource);
        }
        void setGeometry(int32_t x, int32_t y, int32_t w, int32_t h)
        {
            setSize(w, h);
            foreach (View *v, views()) {
                v->setPos(x, y);
            }
        }
        Surface *activate() override
        {
            return m_parent;
        }

        wl_resource *m_resource;
        Surface *m_parent;
    };

    new ActiveRegion(m_shell->compositor(), res, Surface::fromResource(parentResource), x, y, width, height);
}

}

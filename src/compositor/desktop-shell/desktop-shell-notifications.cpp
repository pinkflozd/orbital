/*
 * Copyright 2014 Giulio Camuffo <giuliocamuffo@gmail.com>
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

#include <QDebug>

#include <wayland-server.h>

#include "../shell.h"
#include "../compositor.h"
#include "../utils.h"
#include "../output.h"
#include "../layer.h"
#include "../view.h"
#include "../animation.h"
#include "../surface.h"
#include "desktop-shell-notifications.h"
#include "wayland-desktop-shell-server-protocol.h"

namespace Orbital {

static const int ANIMATION_DURATION = 200;

class DesktopShellNotifications::NotificationSurface : public QObject, public Surface::RoleHandler
{
public:
    struct NSView : public View
    {
        NSView(Output *o, NotificationSurface *p, Surface *s)
            : View(s)
            , output(o)
            , parent(p)
        {
        }
        ~NSView()
        {
            parent->m_views.removeOne(this);
            if (parent->m_views.isEmpty()) {
                delete parent;
            }
        }
        View *pointerEnter(const Pointer *p) override
        {
            if (!parent->inactive) {
                return this;
            }

            alphaAnim.setStart(alpha());
            alphaAnim.setTarget(0.3);
            alphaAnim.run(output, ANIMATION_DURATION);
            return nullptr;
        }
        bool pointerLeave(const Pointer *p) override
        {
            if (!parent->inactive) {
                return true;
            }

            alphaAnim.setStart(alpha());
            alphaAnim.setTarget(1.);
            alphaAnim.run(output, ANIMATION_DURATION);
            return false;
        }
        void move(double v)
        {
            setPos(startPos * (1 - v) + endPos * v);
        }

        Output *output;
        NotificationSurface *parent;
        Animation alphaAnim;
        Animation moveAnim;
        QPointF startPos;
        QPointF endPos;
    };

    NotificationSurface(Compositor *c, Surface *s)
        : m_surface(s)
        , m_compositor(c)
        , initialMove(false)
    {
        foreach (Output *o, c->outputs()) {
            NSView *v = new NSView(o, this, s);
            m_views << v;
            v->setTransformParent(o->rootView());
            connect(&v->alphaAnim, &Animation::update, v, &View::setAlpha);
            connect(&v->moveAnim, &Animation::update, v, &NSView::move);
            c->layer(Compositor::Layer::Overlay)->addView(v);
        }

        s->setRoleHandler(this);
        connect(c, &Compositor::outputCreated, this, &NotificationSurface::outputCreated);
        connect(c, &Compositor::outputRemoved, this, &NotificationSurface::outputRemoved);
    }
    ~NotificationSurface()
    {
        manager->m_notifications.removeOne(this);
    }
    void moveTo(int x, int y)
    {
        foreach (NSView *view, m_views) {
            view->endPos = QPointF(x + view->output->width() - m_surface->width() - 20, y + 20);
            if (initialMove) {
                view->startPos = view->pos();
                view->moveAnim.setStart(0.);
                view->moveAnim.setTarget(1.0);
                view->moveAnim.run(view->output, ANIMATION_DURATION);
            } else {
                view->move(1.);
            }
        }
        initialMove = true;
    }
    void outputCreated(Output *o)
    {
        NSView *v = new NSView(o, this, m_surface);
        m_views << v;
        v->setTransformParent(o->rootView());
        connect(&v->alphaAnim, &Animation::update, v, &View::setAlpha);
        connect(&v->moveAnim, &Animation::update, v, &NSView::move);
        m_compositor->layer(Compositor::Layer::Overlay)->addView(v);
        manager->relayout();
    }
    void outputRemoved(Output *o)
    {
        foreach (NSView *v, m_views) {
            if (v->output == o) {
                delete v;
                break;
            }
        }
    }
    void configure(int x, int y) override
    {
        // All notifications (currently) come from the same client. If there are many
        // notifications so that one or more is pushed offscreen, its weston_surface::output_mask
        // will be 0, being outside the output. That means trying to repaint the surface or
        // the view will not really repaint it, so the frame callbacks for that surface will
        // not be sent, blocking the client.
        // To solve it manually redraw all the outputs, this way we're sure the surface gets
        // repainted and the frame callbacks sent.
        foreach (Output *o, m_compositor->outputs()) {
            o->repaint();
        }
        if (!manager->m_notifications.contains(this)) {
            manager->m_notifications.prepend(this);
            manager->relayout();
        }
    }
    void move(Seat *seat) override {}

    wl_resource *resource;
    Surface *m_surface;
    Compositor *m_compositor;
    QList<NSView *> m_views;
    bool inactive;
    bool initialMove;
    DesktopShellNotifications *manager;
};


DesktopShellNotifications::DesktopShellNotifications(Shell *shell)
                  : Interface(shell)
                  , Global(shell->compositor(), &notifications_manager_interface, 1)
                  , m_shell(shell)
{

}

DesktopShellNotifications::~DesktopShellNotifications()
{
    qDeleteAll(m_notifications);
}

void DesktopShellNotifications::bind(wl_client *client, uint32_t version, uint32_t id)
{
    wl_resource *resource = wl_resource_create(client, &notifications_manager_interface, version, id);

    static const struct notifications_manager_interface implementation = {
        wrapInterface(&DesktopShellNotifications::pushNotification)
    };

    wl_resource_set_implementation(resource, &implementation, this, nullptr);
}

void DesktopShellNotifications::pushNotification(wl_client *, wl_resource *res, uint32_t id, wl_resource *surfaceResource, int32_t flags)
{
    wl_resource *resource = wl_resource_create(wl_resource_get_client(surfaceResource), &notification_surface_interface, 1, id);
    Surface *surface = Surface::fromResource(surfaceResource);

    if (!surface->setRole("notification_surface", res, NOTIFICATIONS_MANAGER_ERROR_ROLE)) {
        return;
    }

    NotificationSurface *surf = new NotificationSurface(m_shell->compositor(), surface);
    surf->resource = resource;
    surf->inactive = flags;
    surf->manager = this;
}

void DesktopShellNotifications::relayout()
{
    int x = 0;
    int y = 0;
    int margin = 10;
    foreach (NotificationSurface *notification, m_notifications) {
        notification->moveTo(x, y);
        y += notification->m_surface->height() + margin;
    }
}

}

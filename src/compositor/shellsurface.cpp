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

#include <signal.h>
#include <unistd.h>

#include <QDebug>

#include <compositor.h>

#include "shellsurface.h"
#include "shell.h"
#include "shellview.h"
#include "workspace.h"
#include "output.h"
#include "compositor.h"
#include "seat.h"
#include "pager.h"
#include "layer.h"

namespace Orbital
{

QHash<QString, QPoint> ShellSurface::s_posCache;

ShellSurface::ShellSurface(Shell *shell, Surface *surface)
            : Object()
            , m_shell(shell)
            , m_surface(surface)
            , m_configureSender(nullptr)
            , m_workspace(nullptr)
            , m_previewView(nullptr)
            , m_resizeEdges(Edges::None)
            , m_forceMap(false)
            , m_currentGrab(nullptr)
            , m_type(Type::None)
            , m_nextType(Type::None)
            , m_popup({ 0, 0, nullptr })
            , m_toplevel({ false, false, nullptr })
            , m_transient({ 0, 0, false })
            , m_state({ QSize(), false, false })
{
    surface->setRoleHandler(this);

    foreach (Output *o, shell->compositor()->outputs()) {
        ShellView *view = new ShellView(this);
        view->setDesignedOutput(o);
        m_views.insert(o->id(), view);
        connect(o, &Output::availableGeometryChanged, this, &ShellSurface::availableGeometryChanged);
    }
    connect(shell->compositor(), &Compositor::outputCreated, this, &ShellSurface::outputCreated);
    connect(shell->compositor(), &Compositor::outputRemoved, this, &ShellSurface::outputRemoved);
    connect(surface, &QObject::destroyed, [this]() { m_views.clear(); delete m_currentGrab; delete this; });
    connect(shell->pager(), &Pager::workspaceActivated, this, &ShellSurface::workspaceActivated);

    wl_client_get_credentials(surface->client(), &m_pid, NULL, NULL);
}

ShellSurface::~ShellSurface()
{
    if (m_popup.seat) {
        m_popup.seat->ungrabPopup(this);
    }
    qDeleteAll(m_views);
}

ShellView *ShellSurface::viewForOutput(Output *o)
{
    return m_views.value(o->id());
}

void ShellSurface::setWorkspace(AbstractWorkspace *ws)
{
    m_workspace = ws;
    m_surface->setWorkspaceMask(ws->mask());
    m_forceMap = true;
    configure(0, 0);
}

Compositor *ShellSurface::compositor() const
{
    return m_shell->compositor();
}

AbstractWorkspace *ShellSurface::workspace() const
{
    return m_workspace;
}

void ShellSurface::setConfigureSender(ConfigureSender sender)
{
    m_configureSender = sender;
}

void ShellSurface::setToplevel()
{
    m_nextType = Type::Toplevel;
    m_toplevel.maximized = false;
    m_toplevel.fullscreen = false;
    disconnectParent();
}

void ShellSurface::setTransient(Surface *parent, int x, int y, bool inactive)
{
    m_parent = parent;
    m_transient.x = x;
    m_transient.y = y;
    m_transient.inactive = inactive;

    disconnectParent();
    m_parentConnections << connect(m_parent, &QObject::destroyed, this, &ShellSurface::parentSurfaceDestroyed);
    m_nextType = Type::Transient;
}

void ShellSurface::setPopup(Surface *parent, Seat *seat, int x, int y)
{
    m_parent = parent;
    m_popup.x = x;
    m_popup.y = y;
    m_popup.seat = seat;

    connectParent();
    m_nextType = Type::Popup;
}

void ShellSurface::connectParent()
{
    disconnectParent();
    m_parentConnections << connect(m_parent, &QObject::destroyed, this, &ShellSurface::parentSurfaceDestroyed);
    m_parentConnections << connect(m_surface, &Surface::activated, m_parent, &Surface::activated);
    m_parentConnections << connect(m_surface, &Surface::deactivated, m_parent, &Surface::deactivated);
}

void ShellSurface::disconnectParent()
{
    foreach (auto &c, m_parentConnections) {
        disconnect(c);
    }
    m_parentConnections.clear();
}

void ShellSurface::setMaximized()
{
    m_nextType = Type::Toplevel;
    m_toplevel.maximized = true;
    m_toplevel.fullscreen = false;
    m_toplevel.output = selectOutput();

    QRect rect = m_toplevel.output->availableGeometry();
    qDebug() << "Maximizing surface on output" << m_toplevel.output << "with rect" << rect;
    sendConfigure(rect.width(), rect.height());
}

void ShellSurface::setFullscreen()
{
    m_nextType = Type::Toplevel;
    m_toplevel.fullscreen = true;
    m_toplevel.maximized = false;

    Output *output = selectOutput();

    QRect rect = output->geometry();
    qDebug() << "Fullscrening surface on output" << output << "with rect" << rect;
    sendConfigure(rect.width(), rect.height());
}

void ShellSurface::setXWayland(int x, int y, bool inactive)
{
    // reuse the transient fields for XWayland
    m_parent = nullptr;
    m_transient.x = x;
    m_transient.y = y;
    m_transient.inactive = inactive;

    disconnectParent();
    m_nextType = Type::XWayland;
}

void ShellSurface::move(Seat *seat)
{
    if (isFullscreen()) {
        return;
    }

    class MoveGrab : public PointerGrab
    {
    public:
        void motion(uint32_t time, double x, double y) override
        {
            pointer()->move(x, y);

            Output *out = grabbedView->output();
            QRect surfaceGeometry = shsurf->geometry();

            int moveX = x + dx;
            int moveY = y + dy;

            QPointF p = QPointF(moveX, moveY);

            QPointF br = p + surfaceGeometry.bottomRight();
            if (shsurf->m_shell->snapPos(out, br)) {
                p = br - surfaceGeometry.bottomRight();
            }

            QPointF tl = p + surfaceGeometry.topLeft();
            if (shsurf->m_shell->snapPos(out, tl)) {
                p = tl - surfaceGeometry.topLeft();
            }

            shsurf->moveViews((int)p.x(), (int)p.y());
        }
        void button(uint32_t time, PointerButton button, Pointer::ButtonState state) override
        {
            if (pointer()->buttonCount() == 0 && state == Pointer::ButtonState::Released) {
                end();
            }
        }
        void ended() override
        {
            shsurf->m_currentGrab = nullptr;
            delete this;
        }

        ShellSurface *shsurf;
        View *grabbedView;
        double dx, dy;
    };

    MoveGrab *move = new MoveGrab;

    View *view = seat->pointer()->pickView()->mainView();
    move->dx = view->x() - seat->pointer()->x();
    move->dy = view->y() - seat->pointer()->y();
    move->shsurf = this;
    move->grabbedView = view;

    move->start(seat, PointerCursor::Move);
    m_currentGrab = move;
}

void ShellSurface::resize(Seat *seat, Edges edges)
{
    class ResizeGrab : public PointerGrab
    {
    public:
        void motion(uint32_t time, double x, double y) override
        {
            pointer()->move(x, y);

            QPointF from = view->mapFromGlobal(pointer()->grabPos());
            QPointF to = view->mapFromGlobal(QPointF(x, y));
            QPointF d = to - from;

            int32_t w = width;
            if (shsurf->m_resizeEdges & ShellSurface::Edges::Left) {
                w -= d.x();
            } else if (shsurf->m_resizeEdges & ShellSurface::Edges::Right) {
                w += d.x();
            }

            int32_t h = height;
            if (shsurf->m_resizeEdges & ShellSurface::Edges::Top) {
                h -= d.y();
            } else if (shsurf->m_resizeEdges & ShellSurface::Edges::Bottom) {
                h += d.y();
            }

            shsurf->sendConfigure(w, h);
        }
        void button(uint32_t time, PointerButton button, Pointer::ButtonState state) override
        {
            if (pointer()->buttonCount() == 0 && state == Pointer::ButtonState::Released) {
                end();
            }
        }
        void ended() override
        {
            shsurf->m_resizeEdges = ShellSurface::Edges::None;
            shsurf->m_currentGrab = nullptr;
            delete this;
        }

        ShellSurface *shsurf;
        View *view;
        int32_t width, height;
    };

    ResizeGrab *grab = new ResizeGrab;

    int e = (int)edges;
    if (e == 0 || e > 15 || (e & 3) == 3 || (e & 12) == 12) {
        return;
    }

    m_resizeEdges = edges;


    QRect rect = geometry();
    grab->width = m_width = rect.width();
    grab->height = m_height = rect.height();
    grab->shsurf = this;
    grab->view = seat->pointer()->pickView()->mainView();

    grab->start(seat, (PointerCursor)e);
    m_currentGrab = grab;
}

void ShellSurface::unmap()
{
    foreach (ShellView *v, m_views) {
        v->cleanupAndUnmap();
    }
    emit m_surface->unmapped();
}

void ShellSurface::sendPopupDone()
{
    m_nextType = Type::None;
    m_popup.seat = nullptr;
    emit popupDone();
}

void ShellSurface::minimize()
{
    unmap();
    emit minimized();
}

void ShellSurface::restore()
{
    m_forceMap = true;
    configure(0, 0);
    emit restored();
}

void ShellSurface::close()
{
    pid_t pid;
    wl_client_get_credentials(m_surface->client(), &pid, NULL, NULL);

    if (pid != getpid()) {
        kill(pid, SIGTERM);
    }
}

void ShellSurface::preview(Output *output)
{
    ShellView *v = viewForOutput(output);

    if (!m_previewView) {
        m_previewView = new ShellView(this);
    }

    m_previewView->setDesignedOutput(output);
    m_previewView->setPos(v->x(), v->y());

    m_shell->compositor()->layer(Compositor::Layer::Dashboard)->addView(m_previewView);
    m_previewView->setTransformParent(output->rootView());
    m_previewView->setAlpha(0.);
    m_previewView->animateAlphaTo(0.8);
}

void ShellSurface::endPreview(Output *output)
{
    if (m_previewView) {
        m_previewView->animateAlphaTo(0., [this]() { m_previewView->unmap(); });
    }
}

void ShellSurface::moveViews(double x, double y)
{
    s_posCache[cacheId()] = QPoint(x, y);
    foreach (ShellView *view, m_views) {
        view->move(QPointF(x, y));
    }
}

void ShellSurface::setTitle(const QString &t)
{
    if (m_title != t) {
        m_title = t;
        emit titleChanged();
        m_surface->setLabel(t);
    }
}

void ShellSurface::setAppId(const QString &id)
{
    if (m_appId != id) {
        m_appId = id;
        emit appIdChanged();
    }
}

void ShellSurface::setGeometry(int x, int y, int w, int h)
{
    m_nextGeometry = QRect(x, y, w, h);
}

void ShellSurface::setPid(pid_t pid)
{
    m_pid = pid;
}

bool ShellSurface::isFullscreen() const
{
    return m_type == Type::Toplevel && m_toplevel.fullscreen;
}

bool ShellSurface::isInactive() const
{
    return (m_type == Type::Transient || m_type == Type::XWayland) && m_transient.inactive;
}

QRect ShellSurface::geometry() const
{
    if (m_geometry.isValid()) {
        return m_geometry;
    }
    return surfaceTreeBoundingBox();
}

QString ShellSurface::title() const
{
    return m_title;
}

QString ShellSurface::appId() const
{
    return m_appId;
}

inline QString ShellSurface::cacheId() const
{
    return QStringLiteral("%1+%2").arg(m_appId, m_title);
}

Maybe<QPoint> ShellSurface::cachedPos() const
{
    QString id = cacheId();
    return s_posCache.contains(id) ? Maybe<QPoint>(s_posCache.value(id)) : Maybe<QPoint>();
}

void ShellSurface::parentSurfaceDestroyed()
{
    m_parent = nullptr;
    m_nextType = Type::None;
}

/*
 * Returns the bounding box of a surface and all its sub-surfaces,
 * in the surface coordinates system. */
QRect ShellSurface::surfaceTreeBoundingBox() const
{
    pixman_region32_t region;
    pixman_box32_t *box;
    weston_subsurface *subsurface;

    pixman_region32_init_rect(&region, 0, 0, m_surface->width(), m_surface->height());

    wl_list_for_each(subsurface, &surface()->surface()->subsurface_list, parent_link) {
        pixman_region32_union_rect(&region, &region,
                                   subsurface->position.x,
                                   subsurface->position.y,
                                   subsurface->surface->width,
                                   subsurface->surface->height);
    }

    box = pixman_region32_extents(&region);
    QRect rect(box->x1, box->y1, box->x2 - box->x1, box->y2 - box->y1);
    pixman_region32_fini(&region);

    return rect;
}

void ShellSurface::configure(int x, int y)
{
    if (m_surface->width() == 0) {
        if (m_popup.seat) {
            m_popup.seat->ungrabPopup(this);
        }

        m_type = Type::None;
        m_workspace = nullptr;
        emit contentLost();
        emit m_surface->unmapped();
        return;
    }

    Type oldType = m_type;
    updateState();
    bool typeChanged = m_type != oldType;

    if (m_type == Type::None) {
        return;
    }

    m_surface->setActivable(m_type != Type::Transient || !m_transient.inactive);

    bool wasMapped = m_surface->isMapped();
    m_shell->configure(this);
    if (!m_workspace) {
        return;
    }

    if (typeChanged) {
        qDeleteAll(m_extraViews);
        m_extraViews.clear();
    }

    if (m_type == Type::Toplevel) {
        int dy = 0;
        int dx = 0;
        QRect rect = geometry();
        if ((int)m_resizeEdges) {
            if (m_resizeEdges & Edges::Top) {
                dy = m_height - rect.height();
            }
            if (m_resizeEdges & Edges::Left) {
                dx = m_width - rect.width();
            }
            m_height = rect.height();
            m_width = rect.width();
        }

        bool map = m_state.maximized != m_toplevel.maximized || m_state.fullscreen != m_toplevel.fullscreen ||
                   m_state.size != rect.size() || m_forceMap;
        m_forceMap = false;
        m_state.size = rect.size();
        m_state.maximized = m_toplevel.maximized;
        m_state.fullscreen = m_toplevel.fullscreen;

        foreach (ShellView *view, m_views) {
            view->configureToplevel(map || !view->layer(), m_toplevel.maximized, m_toplevel.fullscreen, dx, dy);
        }
    } else if (m_type == Type::Popup && typeChanged) {
        ShellSurface *parent = ShellSurface::fromSurface(m_parent);
        if (!parent) {
            foreach (View *view, m_parent->views()) {
                ShellView *v = new ShellView(this);
                v->setDesignedOutput(view->output());
                v->configurePopup(view, m_popup.x, m_popup.y);
                m_extraViews << v;
            }
        } else {
            foreach (Output *o, m_shell->compositor()->outputs()) {
                ShellView *view = viewForOutput(o);
                ShellView *parentView = parent->viewForOutput(o);

                view->configurePopup(parentView, m_popup.x, m_popup.y);
            }
        }
        m_popup.seat->grabPopup(this);
    } else if (m_type == Type::Transient) {
        ShellSurface *parent = ShellSurface::fromSurface(m_parent);
        if (!parent) {
            View *parentView = View::fromView(container_of(m_parent->surface()->views.next, weston_view, surface_link));
            ShellView *view = viewForOutput(parentView->output());
            view->configureTransient(parentView, m_transient.x, m_transient.y);
        } else {
            foreach (Output *o, m_shell->compositor()->outputs()) {
                ShellView *view = viewForOutput(o);
                ShellView *parentView = parent->viewForOutput(o);

                view->configureTransient(parentView, m_transient.x, m_transient.y);
            }
        }
    } else if (m_type == Type::XWayland) {
        foreach (ShellView *view, m_views) {
            view->configureXWayland(m_transient.x, m_transient.y);
        }
    }
    m_surface->damage();

    if (!wasMapped && m_surface->isMapped()) {
        emit mapped();
    }
}

void ShellSurface::updateState()
{
    m_type = m_nextType;
    m_geometry = m_nextGeometry;
}

void ShellSurface::sendConfigure(int w, int h)
{
    if (m_configureSender) {
        m_configureSender(w, h);
    }
}

Output *ShellSurface::selectOutput()
{
    struct Out {
        Output *output;
        int vote;
    };
    QList<Out> candidates;
    foreach (Output *o, m_shell->compositor()->outputs()) {
        candidates.append({ o, m_shell->pager()->isWorkspaceActive(m_workspace, o) ? 10 : 0 });
    }

    Output *output = nullptr;
    if (candidates.isEmpty()) {
        return nullptr;
    } else if (candidates.size() == 1) {
        output = candidates.first().output;
    } else {
        QList<Seat *> seats = m_shell->compositor()->seats();
        for (int i = 0; i < candidates.count(); ++i) {
            Out &o = candidates[i];
            foreach (Seat *s, seats) {
                if (o.output->geometry().contains(s->pointer()->x(), s->pointer()->y())) {
                    o.vote++;
                }
            }
        }
        Out *out = nullptr;
        for (int i = 0; i < candidates.count(); ++i) {
            Out &o = candidates[i];
            if (!out || out->vote < o.vote) {
                out = &o;
            }
        }
        output = out->output;
    }
    return output;
}

void ShellSurface::outputCreated(Output *o)
{
    ShellView *view = new ShellView(this);
    view->setDesignedOutput(o);
    connect(o, &Output::availableGeometryChanged, this, &ShellSurface::availableGeometryChanged);

    if (View *v = *m_views.begin()) {
        view->setInitialPos(v->pos());
    }

    m_views.insert(o->id(), view);
    m_forceMap = true;
    configure(0, 0);
}

void ShellSurface::outputRemoved(Output *o)
{
    View *v = viewForOutput(o);
    m_views.remove(o->id());
    delete v;

    if (m_nextType == Type::Toplevel && m_toplevel.maximized && m_toplevel.output == o) {
        setMaximized();
    }
}

void ShellSurface::availableGeometryChanged()
{
    Output *o = static_cast<Output *>(sender());
    if (m_nextType == Type::Toplevel && m_toplevel.maximized && m_toplevel.output == o) {
        QRect rect = o->availableGeometry();
        sendConfigure(rect.width(), rect.height());
    }
}

void ShellSurface::workspaceActivated(Workspace *w, Output *o)
{
    if (w != workspace()) {
        return;
    }

    if (m_nextType != Type::Toplevel || !m_toplevel.maximized) {
        return;
    }

    if (m_toplevel.output->currentWorkspace() != w) {
        setMaximized();
    }
}

ShellSurface *ShellSurface::fromSurface(Surface *surface)
{
    if (ShellSurface *ss = dynamic_cast<ShellSurface *>(surface->roleHandler())) {
        return ss;
    }
    return nullptr;
}

}

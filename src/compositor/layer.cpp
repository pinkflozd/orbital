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

#include <QDebug>

#include <compositor.h>

#include "layer.h"
#include "view.h"

namespace Orbital {

struct Wrapper {
    weston_layer layer;
    Layer *parent;
};

Layer::Layer(weston_layer *l)
     : m_layer(new Wrapper)
     , m_parent(nullptr)
     , m_acceptInput(true)
{
    m_layer->parent = this;

    weston_layer_init(&m_layer->layer, nullptr);
    wl_list_init(&m_layer->layer.link);
    wl_list_insert(&l->link, &m_layer->layer.link);
}

Layer::Layer(Layer *parent)
     : m_layer(new Wrapper)
     , m_parent(parent)
     , m_acceptInput(true)
{
    m_layer->parent = this;

    weston_layer_init(&m_layer->layer, nullptr);
    wl_list_init(&m_layer->layer.link);

    parent->addChild(this);
}

Layer::~Layer()
{
    if (m_parent) {
        m_parent->m_children.removeOne(this);
    }
    foreach (Layer *c, m_children) {
        c->m_parent = nullptr;
        wl_list_remove(&c->m_layer->layer.link);
        wl_list_init(&c->m_layer->layer.link);
    }
    wl_list_remove(&m_layer->layer.link);
}

void Layer::addChild(Layer *l)
{
    int n = m_children.count();
    Layer *p = n ? m_children.at(n - 1) : this;

    wl_list_insert(&p->m_layer->layer.link, &l->m_layer->layer.link);
    m_children << l;
}

void Layer::addView(View *view)
{
    if (view->m_view->layer_link.link.prev) {
        weston_layer_entry_remove(&view->m_view->layer_link);
    }
    weston_layer_entry_insert(&m_layer->layer.view_list, &view->m_view->layer_link);
}

void Layer::raiseOnTop(View *view)
{
    weston_layer_entry_remove(&view->m_view->layer_link);
    weston_layer_entry_insert(&m_layer->layer.view_list, &view->m_view->layer_link);
    weston_view_damage_below(view->m_view);
}

void Layer::lower(View *view)
{
    weston_layer_entry *next;
    if (wl_list_empty(&view->m_view->layer_link.link)) {
        next = &m_layer->layer.view_list;
    } else {
        next = container_of(view->m_view->layer_link.link.next, weston_layer_entry, link);
    }
    weston_layer_entry_remove(&view->m_view->layer_link);

    weston_layer_entry_insert(next, &view->m_view->layer_link);
    weston_view_damage_below(view->m_view);
}

View *Layer::topView() const
{
    if (wl_list_empty(&m_layer->layer.view_list.link)) {
        return nullptr;
    }

    weston_view *v = container_of(m_layer->layer.view_list.link.next, weston_view, layer_link.link);
    return View::fromView(v);
}

void Layer::setMask(int x, int y, int w, int h)
{
    weston_layer_set_mask(&m_layer->layer, x, y, w, h);
}

void Layer::setAcceptInput(bool accept)
{
    m_acceptInput = accept;
}

Layer *Layer::fromLayer(weston_layer *l)
{
    return reinterpret_cast<Wrapper *>(l)->parent;
}

}

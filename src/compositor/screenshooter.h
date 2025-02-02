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

#ifndef SCREENSHOOTER_H
#define SCREENSHOOTER_H

#include <wayland-server.h>

#include "interface.h"

namespace Orbital {

class Shell;

class Screenshooter : public Interface, public RestrictedGlobal
{
public:
    Screenshooter(Shell *s);

private:
    void bind(wl_client *client, uint32_t version, uint32_t id) override;
    void shoot(wl_client *client, wl_resource *resource, uint32_t id, wl_resource *outputResource, wl_resource *bufferResource);
};

}

#endif

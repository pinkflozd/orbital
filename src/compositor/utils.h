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

#ifndef ORBITAL_UTILS_H
#define ORBITAL_UTILS_H

#include <compositor.h>

namespace Orbital {

template<class R, class T, class... Args>
struct InterfaceWrapper {
    template<R (T::*F)(wl_client *, wl_resource *, Args...)>
    static void forward(wl_client *client, wl_resource *resource, Args... args) {
        (static_cast<T *>(wl_resource_get_user_data(resource))->*F)(client, resource, args...);
    }
    template<R (T::*F)(Args...)>
    static void forward(wl_client *client, wl_resource *resource, Args... args) {
        (static_cast<T *>(wl_resource_get_user_data(resource))->*F)(args...);
    }
};

template<class R, class T, class... Args>
constexpr static auto createWrapper(R (T::*func)(wl_client *client, wl_resource *resource, Args...)) -> InterfaceWrapper<R, T, Args...> {
    return InterfaceWrapper<R, T, Args...>();
}

template<class R, class T, class... Args>
constexpr static auto createWrapper(R (T::*func)(Args...)) -> InterfaceWrapper<R, T, Args...> {
    return InterfaceWrapper<R, T, Args...>();
}


#define wrapInterface(method) createWrapper(method).forward<method>

template<class T>
class Maybe
{
public:
    inline Maybe() : m_isSet(false) {}
    inline Maybe(T v) : m_value(v), m_isSet(true) {}
    inline Maybe(const Maybe<T> &m) : m_value(m.m_value), m_isSet(m.m_isSet) {}

    inline bool isSet() const { return m_isSet; }
    inline operator bool() const { return m_isSet; }
    inline Maybe<T> &operator=(const Maybe<T> &m) { m_value = m.m_value; m_isSet = m.m_isSet; return *this; }

    inline void set(const T &v) { m_value = v; m_isSet = true; }
    inline void reset() { m_isSet = false; }
    inline const T &value() const { return m_value; }

private:
    T m_value;
    bool m_isSet;
};

}


#define DECLARE_OPERATORS_FOR_FLAGS(F) \
    inline int operator&(F a, F b) { \
        return (int)a & (int)b; \
    } \
    inline F operator|(F a, F b) { return (F)((int)a | (int)b); }

#endif

/*
 * Copyright 2015 Giulio Camuffo <giuliocamuffo@gmail.com>
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

#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>

#include <wayland-client.h>

#include "../client/utils.h"
#include "wayland-authorizer-helper-client-protocol.h"

class Helper {
public:
    Helper()
        : helper(nullptr)
    {
    }
    void global(wl_registry *registry, uint32_t id, const char *interface, uint32_t version)
    {
#define registry_bind(type, v) static_cast<type *>(wl_registry_bind(registry, id, &type ## _interface, qMin(version, v)))
        if (strcmp(interface, "orbital_authorizer_helper") == 0) {
            helper = registry_bind(orbital_authorizer_helper, 1u);
            static const orbital_authorizer_helper_listener listener = {
                wrapInterface(&Helper::authorizationRequested)
            };
            orbital_authorizer_helper_add_listener(helper, &listener, this);
        }
    }
    void globalRemove(wl_registry *registry, uint32_t id)
    {
    }

    void authorizationRequested(orbital_authorizer_helper *, orbital_authorizer_helper_result *result, const char *interface, int pid)
    {
        char path[256], buf[256];
        int ret = snprintf(path, sizeof(path), "/proc/%d/exe", pid);
        if ((size_t)ret >= sizeof(path)) {
            orbital_authorizer_helper_result_result(result, ORBITAL_AUTHORIZER_HELPER_RESULT_RESULT_VALUE_DENY);
            return;
        }

        ret = readlink(path, buf, sizeof(buf));
        if (ret == -1 || (size_t)ret == sizeof(buf)) {
            orbital_authorizer_helper_result_result(result, ORBITAL_AUTHORIZER_HELPER_RESULT_RESULT_VALUE_DENY);
            return;
        }
        buf[ret] = '\0';

        if (authorizeProcess(interface, buf)) {
            orbital_authorizer_helper_result_result(result, ORBITAL_AUTHORIZER_HELPER_RESULT_RESULT_VALUE_ALLOW);
        } else {
            orbital_authorizer_helper_result_result(result, ORBITAL_AUTHORIZER_HELPER_RESULT_RESULT_VALUE_DENY);
        }

    }

    bool authorizeProcess(const char *global, const char *executable)
    {
        QFile file(QStringLiteral("/etc/orbital/restricted_interfaces.conf"));
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning("Cannot open restricted_interfaces.conf");
            return false;
        }

        QJsonParseError error;
        QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError) {
            qWarning("Error parsing restricted_interfaces.conf at offset %d: %s", error.offset, qPrintable(error.errorString()));
            return false;
        }

        QJsonObject config = document.object();
        file.close();

        if (!config.contains(global)) {
            return false;
        }

        config = config.value(global).toObject();
        QJsonValue value = config.value(executable);
        if (value != QJsonValue::Undefined) {
            return value.toString(QStringLiteral("deny")) == QStringLiteral("allow");
        }
        return false;
    }

    wl_display *display;
    wl_registry *registry;
    orbital_authorizer_helper *helper;
};

int main(int argc, char **argv)
{
    Helper helper;

    helper.display = wl_display_connect(nullptr);

    helper.registry = wl_display_get_registry(helper.display);
    static const wl_registry_listener registryListener = {
        wrapInterface(&Helper::global),
        wrapInterface(&Helper::globalRemove)
    };
    wl_registry_add_listener(helper.registry, &registryListener, &helper);

    wl_display_roundtrip(helper.display);
    if (!helper.helper) {
        qWarning("No orbital_authorizer_helper interface.");
        exit(1);
    }

    int ret = 0;
    while (ret != -1)
        ret = wl_display_dispatch(helper.display);

    return 0;
}

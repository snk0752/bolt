/*
 * Copyright © 2017 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Christian J. Kellner <christian@kellner.me>
 */

#pragma once

#include "bolt-enums.h"
#include "bolt-gdbus.h"
#include "bolt-manager.h"

/* forward declaration */
struct udev_device;

G_BEGIN_DECLS

#define BOLT_TYPE_DEVICE bolt_device_get_type ()
G_DECLARE_FINAL_TYPE (BoltDevice, bolt_device, BOLT, DEVICE, BoltDBusDeviceSkeleton);

typedef void (*AuthCallback) (BoltDevice *dev,
                              gboolean    ok,
                              GError    **error,
                              gpointer    user_data);

BoltDevice *      bolt_device_new_for_udev (BoltManager        *manager,
                                            struct udev_device *udev,
                                            GError            **error);

void              bolt_device_set_manager (BoltDevice  *device,
                                           BoltManager *manager);

const char *      bolt_device_export (BoltDevice      *device,
                                      GDBusConnection *connection,
                                      GError         **error);

void              bolt_device_unexport (BoltDevice *device);

BoltStatus        bolt_device_connected (BoltDevice         *dev,
                                         struct udev_device *udev);

BoltStatus        bolt_device_disconnected (BoltDevice *dev);

gboolean          bolt_device_is_connected (BoltDevice *device);

gboolean          bolt_device_authorize (BoltDevice  *dev,
                                         AuthCallback callback,
                                         gpointer     user_data,
                                         GError     **error);

guint             bolt_device_get_key (BoltDevice *dev);

const char *      bolt_device_get_name (BoltDevice *dev);

const char *      bolt_device_get_object_path (BoltDevice *device);

BoltPolicy        bolt_device_get_policy (BoltDevice *dev);

const char *      bolt_device_get_uid (BoltDevice *dev);

BoltSecurity      bolt_device_get_security (BoltDevice *dev);

guint             bolt_device_get_store (BoltDevice *dev);

BoltStatus        bolt_device_get_status (BoltDevice *dev);

const char *      bolt_device_get_syspath (BoltDevice *dev);

const char *      bolt_device_get_vendor (BoltDevice *dev);

G_END_DECLS

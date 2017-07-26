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


#include <glib.h>
#include <glib/gprintf.h>

#include <gio/gio.h>
#include <gudev/gudev.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "device.h"
#include "manager.h"
#include "store.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DIR, closedir);

static gboolean
tb_close (int fd, GError **error)
{
  int r;

  r = close (fd);

  if (r == 0)
    return TRUE;

  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Could not close file: %s", g_strerror (errno));

  return FALSE;
}

static gboolean
tb_write_char (int fd, char data, GError **error)
{
  ssize_t n;

retry:
  n = write (fd, &data, 1);

  if (n == -1)
    {
      int errsv = errno;
      if (errsv == EINTR)
        goto retry;

      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (errsv),
                   "Could not write data: %s",
                   g_strerror (errno));
    }

  return n > 0;
}

static int
tb_openat (DIR *d, const char *path, int oflag, GError **error)
{
  int fd = -1;

  fd = openat (dirfd (d), path, oflag);

  if (fd < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (errno),
                   "Could not open file %s: %s",
                   path,
                   g_strerror (errno));
    }

  return fd;
}

static gboolean
tb_verify_uid (int fd, const char *uid, GError **error)
{
  gsize len = strlen (uid);
  char buffer[len];
  ssize_t n;

retry:
  n = read (fd, buffer, sizeof (buffer));
  if (n < 0)
    {
      if (errno == EINTR)
        goto retry;

      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno), "Could not read from file");
      return FALSE;
    }
  else if (len != n)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read full uid from file");
      return FALSE;
    }

  if (memcmp (buffer, uid, len))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "unique id verification failed [%s != %s]",
                   buffer,
                   uid);
      return FALSE;
    }

  return TRUE;
}

static ssize_t
tb_read_all (int fd, void *buffer, gsize nbyte, GError **error)
{
  guint8 *ptr   = buffer;
  ssize_t nread = 0;

  do
    {
      ssize_t n = read (fd, ptr, nbyte);

      if (n < 0)
        {
          int errsv = errno;

          if (errsv == EINTR)
            continue;

          g_set_error (error,
                       G_IO_ERROR,
                       g_io_error_from_errno (errsv),
                       "input error while reading: %s",
                       g_strerror (errsv));

          return n > 0 ? n : -errsv;

        }
      else if (n == 0)
        {
          return nread;
        }

      g_assert ((gsize) n <= nbyte);

      ptr += n;
      nread += n;
      nbyte -= n;

    }
  while (nbyte > 0);

  return nread;
}

static gboolean
copy_key (GFile *key, int to, GError **error)
{
  g_autofree char *path     = NULL;
  char buffer[TB_KEY_CHARS] = {
    0,
  };
  int from = -1;
  ssize_t n, k;

  path = g_file_get_path (key);

  from = open (path, O_RDONLY);

  if (from < 0)
    {
      gint code = g_io_error_from_errno (errno);
      g_set_error_literal (error, G_IO_ERROR, code, "Could not open key file");
      return FALSE;
    }

  /* NB: need to write the key in one go, no chuncked i/o */
  n = tb_read_all (from, buffer, sizeof (buffer), error);
  if (n < 0)
    {
      return FALSE;
    }
  else if (n != sizeof (buffer))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read entire key from disk");
      return FALSE;
    }

  do
    k = write (to, buffer, (size_t) n);
  while (k < 0 && errno == EINTR);

  if (k != n)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           g_io_error_from_errno (errno),
                           "io error while writing key data");
      return FALSE;
    }

  return TRUE;
}

static gboolean
tb_device_authorize (TbManager *mgr, TbDevice *dev, GError **error)
{
  g_autoptr(GFile) key = NULL;
  g_autoptr(DIR) d     = NULL;
  const char *sysfs;
  int security;
  gboolean ok;
  int fd = -1;

  g_return_val_if_fail (dev != NULL, FALSE);
  g_return_val_if_fail (dev->uid != NULL, FALSE);

  security = tb_manager_get_security_level (mgr);

  if (security < 1)
    /* nothing to do */
    return TRUE;

  sysfs = tb_device_get_sysfs_path (dev);
  g_assert (sysfs != NULL);

  d = opendir (sysfs);

  if (d == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno), "Could not open directory.");
      return FALSE;
    }

  /* openat is used here to be absolutely sure that the
   * directory that contains the right 'unique_id' is the
   * one we are authorizing */
  fd = tb_openat (d, "unique_id", O_RDONLY, error);
  if (fd < 0)
    return FALSE;

  ok = tb_verify_uid (fd, dev->uid, error);
  if (!ok)
    {
      close (fd);
      return FALSE;
    }

  close (fd);

  if (security == '2')
    {
      gboolean created = FALSE;
      int keyfd        = -1;

      key = tb_manager_ensure_key (mgr, dev, FALSE, &created, error);
      if (key == NULL)
        return FALSE;

      keyfd = tb_openat (d, "key", O_WRONLY, error);
      if (keyfd < 0)
        return FALSE;

      ok = copy_key (key, keyfd, error);
      if (!ok)
        {
          close (fd);
          return FALSE;
        }

      ok = tb_close (keyfd, error);
      if (!ok)
        return FALSE;

      if (created)
        security = '1';
    }

  fd = tb_openat (d, "authorized", O_WRONLY, error);

  if (fd < 0)
    return FALSE;

  ok = tb_write_char (fd, security, error);
  if (!ok)
    {
      close (fd);
      return FALSE;
    }

  return tb_close (fd, error);
}

static gboolean do_store = FALSE;
static gboolean do_auto  = FALSE;
static GOptionEntry authorize_opts[] =
{{"store", 's', 0, G_OPTION_ARG_NONE, &do_store, "Store device", NULL},
 {"auto", 'a', 0, G_OPTION_ARG_NONE, &do_auto, "Auto-authorize device (implies --store)", NULL},
 {NULL}};

static int
authorize_device (TbManager *mgr, int argc, char **argv)
{
  g_autoptr(GError) error          = NULL;
  g_autoptr(GOptionContext) optctx = NULL;
  g_autoptr(TbDevice) dev          = NULL;
  const char *uid;
  gboolean ok;

  optctx = g_option_context_new (NULL);

  g_option_context_set_summary (optctx, "Authorize a specific thunderbolt device");
  g_option_context_set_strict_posix (optctx, TRUE);
  g_option_context_add_main_entries (optctx, authorize_opts, NULL);

  if (!g_option_context_parse (optctx, &argc, &argv, &error))
    {
      g_printerr ("Failed to parse command line arguments.");
      return EXIT_FAILURE;
    }

  if (argc < 2)
    {
      g_printerr ("Need device id\n");
      return EXIT_FAILURE;
    }

  uid = argv[1];

  dev = tb_manager_lookup (mgr, uid);
  if (dev == NULL)
    {
      g_printerr ("Could not find device\n");
      return EXIT_FAILURE;
    }

  ok = tb_device_authorize (mgr, dev, &error);

  if (!ok)
    {
      g_printerr ("Could not authorize device: %s [%d]\n", error->message, error->code);
      return EXIT_FAILURE;
    }

  if (do_auto)
    {
      do_store = TRUE;
      g_object_set (dev, "policy", TB_POLICY_AUTO, NULL);
    }

  if (do_store)
    {
      ok = tb_manager_store (mgr, dev, &error);
      if (!ok)
        {
          g_fprintf (stderr, "Could not store device in database: %s\n", error->message);
          return EXIT_FAILURE;
        }
    }

  return EXIT_SUCCESS;
}

static int
auto_device (TbManager *mgr, int argc, char **argv)
{
  g_autoptr(GError) error          = NULL;
  g_autoptr(GOptionContext) optctx = NULL;
  g_autoptr(TbDevice) dev          = NULL;
  const char *uid;
  gboolean ok;

  optctx = g_option_context_new (NULL);

  g_option_context_set_summary (optctx, "Automatically authorize thunderbolt devices");
  g_option_context_set_strict_posix (optctx, TRUE);

  if (!g_option_context_parse (optctx, &argc, &argv, &error))
    {
      g_printerr ("Failed to parse command line arguments.");
      return EXIT_FAILURE;
    }

  if (argc < 2)
    {
      g_printerr ("Need device id\n");
      return EXIT_FAILURE;
    }

  uid = argv[1];

  dev = tb_manager_lookup (mgr, uid);
  if (dev == NULL)
    {
      g_printerr ("Could not find device\n");
      return EXIT_FAILURE;
    }

  if (!tb_device_in_store (dev))
    {
      g_print ("thunderbolt device %s not in store.", tb_device_get_uid (dev));
      return EXIT_SUCCESS;
    }
  else if (tb_device_get_policy (dev) != TB_POLICY_AUTO)
    {
      g_print ("thunderbolt device %s not setup for auto authorization.", tb_device_get_uid (dev));
      return EXIT_SUCCESS;
    }

  ok = tb_device_authorize (mgr, dev, &error);

  if (!ok)
    {
      g_printerr ("Could not authorize device: %s [%d]\n", error->message, error->code);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}

static char **
make_args (int argc, char **argv)
{
  char **args = g_new (char *, argc - 1);
  int i;

  args[0] = g_strdup (argv[0]);

  for (i              = 1; i < argc; i++)
    args[i - 1] = g_strdup (argv[i]);

  return args;
}

int
main (int argc, char **argv)
{
  g_autoptr(GError) error          = NULL;
  g_autoptr(TbManager) mgr         = NULL;
  g_autoptr(GOptionContext) optctx = NULL;
  const char *cmd;
  int res = EXIT_FAILURE;

  setlocale (LC_ALL, "");

  optctx = g_option_context_new (NULL);

  g_option_context_set_summary (optctx, "Authorize thunderbolt devices");
  g_option_context_set_strict_posix (optctx, TRUE);

  if (!g_option_context_parse (optctx, &argc, &argv, &error))
    {
      g_printerr ("Failed to parse command line arguments.");
      return EXIT_FAILURE;
    }

  if (argc < 2)
    {
      g_printerr ("Usage %s COMMAND\n", argv[0]);
      return EXIT_FAILURE;
    }

  if (getuid () != 0 || geteuid () != 0)
    {
      g_printerr ("Need root permissions to authenticate.\n");
      return EXIT_FAILURE;
    }

  cmd = argv[1];

  mgr = tb_manager_new (&error);

  if (g_str_equal (cmd, "authorize"))
    {
      g_auto(GStrv) args = make_args (argc, argv);
      res                = authorize_device (mgr, argc - 1, args);
    }
  else if (g_str_equal (cmd, "auto"))
    {
      g_auto(GStrv) args = make_args (argc, argv);
      res                = auto_device (mgr, argc - 1, args);
    }
  else
    {
      g_printerr ("Unknown command.\n");
    }

  return res;
}

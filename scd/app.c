/* app.c - Application selection.
 *	Copyright (C) 2003 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "scdaemon.h"
#include "app-common.h"
#include "apdu.h"
#include "iso7816.h"
#include "dynload.h"

static char *default_reader_port;

void
app_set_default_reader_port (const char *portstr)
{
  xfree (default_reader_port);
  default_reader_port = portstr? xstrdup (portstr): NULL;
}


/* The select the best fitting application and return a context.
   Returns NULL if no application was found or no card is present. */
APP
select_application (void)
{
  int slot;
  int rc;
  APP app;

  slot = apdu_open_reader (default_reader_port);
  if (slot == -1)
    {
      log_error ("card reader not available\n");
      return NULL;
    }

  app = xtrycalloc (1, sizeof *app);
  if (!app)
    {
      rc = out_of_core ();
      log_info ("error allocating context: %s\n", gpg_strerror (rc));
      /*apdu_close_reader (slot);*/
      return NULL;
    }

  app->slot = slot;
  rc = app_select_openpgp (app, &app->serialno, &app->serialnolen);
  if (rc)
    {
/*        apdu_close_reader (slot); */
      log_info ("selecting openpgp failed: %s\n", gpg_strerror (rc));
      xfree (app);
      return NULL;
    }

  app->initialized = 1;
  return app;
}



/* Retrieve the serial number and the time of the last update of the
   card.  The serial number is returned as a malloced string (hex
   encoded) in SERIAL and the time of update is returned in STAMP.  If
   no update time is available the returned value is 0.  Caller must
   free SERIAL unless the function returns an error. */
int 
app_get_serial_and_stamp (APP app, char **serial, time_t *stamp)
{
  unsigned char *buf, *p;
  int i;

  if (!app || !serial || !stamp)
    return gpg_error (GPG_ERR_INV_VALUE);

  *serial = NULL;
  *stamp = 0; /* not available */

  buf = xtrymalloc (app->serialnolen * 2 + 1);
  if (!buf)
    return gpg_error_from_errno (errno);
  for (p=buf, i=0; i < app->serialnolen; p +=2, i++)
    sprintf (p, "%02X", app->serialno[i]);
  *p = 0;
  *serial = buf;
  return 0;
}


/* Write out the application specifig status lines for the LEARN
   command. */
int
app_write_learn_status (APP app, CTRL ctrl)
{
  if (!app)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.learn_status)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  return app->fnc.learn_status (app, ctrl);
}


/* Perform a GETATTR operation.  */
int 
app_getattr (APP app, CTRL ctrl, const char *name)
{
  if (!app || !name || !*name)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.getattr)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  return app->fnc.getattr (app, ctrl, name);
}

/* Perform a SETATTR operation.  */
int 
app_setattr (APP app, const char *name,
             int (*pincb)(void*, const char *, char **),
             void *pincb_arg,
             const unsigned char *value, size_t valuelen)
{
  if (!app || !name || !*name || !value)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.setattr)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  return app->fnc.setattr (app, name, pincb, pincb_arg, value, valuelen);
}

/* Create the signature and return the allocated result in OUTDATA.
   If a PIN is required the PINCB will be used to ask for the PIN; it
   should return the PIN in an allocated buffer and put it into PIN.  */
int 
app_sign (APP app, const char *keyidstr, int hashalgo,
          int (pincb)(void*, const char *, char **),
          void *pincb_arg,
          const void *indata, size_t indatalen,
          unsigned char **outdata, size_t *outdatalen )
{
  int rc;

  if (!app || !indata || !indatalen || !outdata || !outdatalen || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.sign)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.sign (app, keyidstr, hashalgo,
                      pincb, pincb_arg,
                      indata, indatalen,
                      outdata, outdatalen);
  if (opt.verbose)
    log_info ("operation sign result: %s\n", gpg_strerror (rc));
  return rc;
}

/* Create the signature using the INTERNAL AUTHENTICATE command and
   return the allocated result in OUTDATA.  If a PIN is required the
   PINCB will be used to ask for the PIN; it should return the PIN in
   an allocated buffer and put it into PIN.  */
int 
app_auth (APP app, const char *keyidstr,
          int (pincb)(void*, const char *, char **),
          void *pincb_arg,
          const void *indata, size_t indatalen,
          unsigned char **outdata, size_t *outdatalen )
{
  int rc;

  if (!app || !indata || !indatalen || !outdata || !outdatalen || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.auth)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.auth (app, keyidstr,
                      pincb, pincb_arg,
                      indata, indatalen,
                      outdata, outdatalen);
  if (opt.verbose)
    log_info ("operation auth result: %s\n", gpg_strerror (rc));
  return rc;
}


/* Decrypt the data in INDATA and return the allocated result in OUTDATA.
   If a PIN is required the PINCB will be used to ask for the PIN; it
   should return the PIN in an allocated buffer and put it into PIN.  */
int 
app_decipher (APP app, const char *keyidstr,
              int (pincb)(void*, const char *, char **),
              void *pincb_arg,
              const void *indata, size_t indatalen,
              unsigned char **outdata, size_t *outdatalen )
{
  int rc;

  if (!app || !indata || !indatalen || !outdata || !outdatalen || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.decipher)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.decipher (app, keyidstr,
                          pincb, pincb_arg,
                          indata, indatalen,
                          outdata, outdatalen);
  if (opt.verbose)
    log_info ("operation decipher result: %s\n", gpg_strerror (rc));
  return rc;
}


/* Perform a SETATTR operation.  */
int 
app_genkey (APP app, CTRL ctrl, const char *keynostr, unsigned int flags,
            int (*pincb)(void*, const char *, char **),
            void *pincb_arg)
{
  int rc;

  if (!app || !keynostr || !*keynostr || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.genkey)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.genkey (app, ctrl, keynostr, flags, pincb, pincb_arg);
  if (opt.verbose)
    log_info ("operation genkey result: %s\n", gpg_strerror (rc));
  return rc;
}


/* Perform a GET CHALLENGE operation.  This fucntion is special as it
   directly accesses the card without any application specific
   wrapper. */
int
app_get_challenge (APP app, size_t nbytes, unsigned char *buffer)
{
  if (!app || !nbytes || !buffer)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  return iso7816_get_challenge (app->slot, nbytes, buffer);
}



/* Perform a CHANGE REFERENCE DATA or RESET RETRY COUNTER operation.  */
int 
app_change_pin (APP app, CTRL ctrl, const char *chvnostr, int reset_mode,
                int (*pincb)(void*, const char *, char **),
                void *pincb_arg)
{
  int rc;

  if (!app || !chvnostr || !*chvnostr || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.change_pin)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.change_pin (app, ctrl, chvnostr, reset_mode, pincb, pincb_arg);
  if (opt.verbose)
    log_info ("operation change_pin result: %s\n", gpg_strerror (rc));
  return rc;
}


/* Perform a VERIFY operation without doing anything lese.  This may
   be used to initialze a the PION cache for long lasting other
   operations.  Its use is highly application dependent. */
int 
app_check_pin (APP app, const char *keyidstr,
               int (*pincb)(void*, const char *, char **),
               void *pincb_arg)
{
  int rc;

  if (!app || !keyidstr || !*keyidstr || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.check_pin)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.check_pin (app, keyidstr, pincb, pincb_arg);
  if (opt.verbose)
    log_info ("operation check_pin result: %s\n", gpg_strerror (rc));
  return rc;
}







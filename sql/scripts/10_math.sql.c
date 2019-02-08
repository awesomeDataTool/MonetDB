/*
* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0.  If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/.
*
* Copyright 1997 - July 2008 CWI, August 2008 - 2019 MonetDB B.V.
*/

// This file was generated automatically through bootstrap.py.
// Do not edit this file directly.

#include "monetdb_config.h"

#include "10_math.sql.h"
#include "gdk.h"
#include "mal_client.h"
#include "sql_catalog.h"

#include <assert.h>

extern str SQLstatementIntern(Client c, str *expr, str nme, bit execute, bit output, res_table **result);

str
sql_install_10_math(Client c, char *buf, size_t bufsize)
{
	size_t pos = 0;

	pos += snprintf(buf, bufsize, " CREATE FUNCTION degrees(r double) RETURNS double RETURN r*180/pi(); CREATE FUNCTION radians(d double) RETURNS double RETURN d*pi()/180; grant execute on function degrees to public; grant execute on function radians to public;");
	if (pos >= bufsize)
		throw(SQL, "createdb.10_math", SQLSTATE(42000) "SQL script to install is too large");
	printf("# loading sql script: 10_math.sql\n");
	return SQLstatementIntern(c, &buf, "createdb.10_math", 1, 0, NULL);
}

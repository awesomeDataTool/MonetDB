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

#include "14_inet.sql.h"
#include "gdk.h"
#include "mal_client.h"
#include "sql_catalog.h"

#include <assert.h>

extern str SQLstatementIntern(Client c, str *expr, str nme, bit execute, bit output, res_table **result);

str
sql_install_14_inet(Client c, char *buf, size_t bufsize)
{
	size_t pos = 0;

	pos += snprintf(buf, bufsize, " CREATE TYPE inet EXTERNAL NAME inet; CREATE FUNCTION \"broadcast\" (p inet) RETURNS inet EXTERNAL NAME inet.\"broadcast\"; GRANT EXECUTE ON FUNCTION \"broadcast\"(inet) TO PUBLIC; CREATE FUNCTION \"host\" (p inet) RETURNS clob EXTERNAL NAME inet.\"host\"; GRANT EXECUTE ON FUNCTION \"host\"(inet) TO PUBLIC; CREATE FUNCTION \"masklen\" (p inet) RETURNS int EXTERNAL NAME inet.\"masklen\"; GRANT EXECUTE ON FUNCTION \"masklen\"(inet) TO PUBLIC; CREATE FUNCTION \"setmasklen\" (p inet, mask int) RETURNS inet EXTERNAL NAME inet.\"setmasklen\"; GRANT EXECUTE ON FUNCTION \"setmasklen\"(inet, int) TO PUBLIC; CREATE FUNCTION \"netmask\" (p inet) RETURNS inet EXTERNAL NAME inet.\"netmask\"; GRANT EXECUTE ON FUNCTION \"netmask\"(inet) TO PUBLIC; CREATE FUNCTION \"hostmask\" (p inet) RETURNS inet EXTERNAL NAME inet.\"hostmask\"; GRANT EXECUTE ON FUNCTION \"hostmask\"(inet) TO PUBLIC; CREATE FUNCTION \"network\" (p inet) RETURNS inet EXTERNAL NAME inet.\"network\"; GRANT EXECUTE ON FUNCTION \"network\"(inet) TO PUBLIC; CREATE FUNCTION \"text\" (p inet) RETURNS clob EXTERNAL NAME inet.\"text\"; GRANT EXECUTE ON FUNCTION \"text\"(inet) TO PUBLIC; CREATE FUNCTION \"abbrev\" (p inet) RETURNS clob EXTERNAL NAME inet.\"abbrev\"; GRANT EXECUTE ON FUNCTION \"abbrev\"(inet) TO PUBLIC; CREATE FUNCTION \"left_shift\"(i1 inet, i2 inet) RETURNS boolean EXTERNAL NAME inet.\"<<\"; GRANT EXECUTE ON FUNCTION \"left_shift\"(inet, inet) TO PUBLIC; CREATE FUNCTION \"right_shift\"(i1 inet, i2 inet) RETURNS boolean EXTERNAL NAME inet.\">>\"; GRANT EXECUTE ON FUNCTION \"right_shift\"(inet, inet) TO PUBLIC; CREATE FUNCTION \"left_shift_assign\"(i1 inet, i2 inet) RETURNS boolean EXTERNAL NAME inet.\"<<=\"; GRANT EXECUTE ON FUNCTION \"left_shift_assign\"(inet, inet) TO PUBLIC; CREATE FUNCTION \"right_shift_assign\"(i1 inet, i2 inet) RETURNS boolean EXTERNAL NAME inet.\">>=\"; GRANT EXECUTE ON FUNCTION \"right_shift_assign\"(inet, inet) TO PUBLIC;");
	if (pos >= bufsize)
		throw(SQL, "createdb.14_inet", SQLSTATE(42000) "SQL script to install is too large");
	printf("# loading sql script: 14_inet.sql\n");
	return SQLstatementIntern(c, &buf, "createdb.14_inet", 1, 0, NULL);
}

/*
 * The contents of this file are subject to the MonetDB Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.monetdb.org/Legal/MonetDBLicense
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the MonetDB Database System.
 *
 * The Initial Developer of the Original Code is CWI.
 * Portions created by CWI are Copyright (C) 1997-July 2008 CWI.
 * Copyright August 2008-2011 MonetDB B.V.
 * All Rights Reserved.
 */

/*
 * Use a single gauge structure to split the largest table.
 * @f centipede
 * @a M. Kersten
 * @- Centipede scheduling.
 * TBD when the partition manager works in single node.
 */
#include "monetdb_config.h"
#include "centipede.h"
#include "mal_builder.h"
#include <mapi.h>
#include "remote.h"
#include "mal_sabaoth.h"
#include "mal_recycle.h"

#include "mal_interpreter.h"

typedef struct REGMAL{
    str fcn;
    struct REGMAL *nxt;
} *Registry;

typedef struct {
    str uri;
    str usr;
    str pwd;
    Registry nxt; /* list of registered mal functions */
    bte active;
    str conn;
    int inuse;
} Peer;

static Peer peers[MAXSITES];    /* registry of peer servers */
static int nrpeers;				/* peers active in sliced processing */
static bte slicingLocal;		/* only use local node without remote calls*/

typedef	struct{
	InstrPtr target;
	str schema, table, column;
	int type, slice;
	int lgauge, hgauge;
	ValRecord bounds[MAXSITES];
} Gauge;

static int
OPTfindPeer(str uri)
{
    int i;
    for (i = 0; i < nrpeers; i++)
        if ( strcmp(uri, peers[i].uri) == 0 )
            return i;
    return -1;
}
/* Look for and add a peer with uri in the registry.  Return index in registry */
int
OPTgetPeer(str uri)
{
	int i;

	i = OPTfindPeer(uri);
	if ( i >=0 ) {
		peers[i].active = 1;
		return i;
	}
	if ( nrpeers == MAXSITES)
		return -1;
	i = nrpeers;
	peers[i].usr = GDKstrdup("monetdb");
	peers[i].uri = GDKstrdup(uri);
	peers[i].pwd = GDKstrdup("monetdb");
	peers[i].active = 1;
	peers[i].nxt = NULL;
	peers[i].inuse = 0;
	nrpeers++;
	return i;
}

/* Clean function registry of non-active peers */

void OPTcleanFunReg(int i)
{
	Registry r, q;
	mal_set_lock(mal_contextLock,"slicing.cleanFunReg");
	r = peers[i].nxt;
	peers[i].nxt = NULL;
	mal_unset_lock(mal_contextLock,"slicing.cleanFunReg");
	while ( r ) {
			q = r->nxt;
			GDKfree(r->fcn);
			GDKfree(r);
			r = q;
	}
}

str
OPTdiscover(Client cntxt)
{
	bat bid = 0;
	BAT *b;
	BUN p,q;
	str msg = MAL_SUCCEED;
	BATiter bi;
	char buf[BUFSIZ]= "*/slicing", *s= buf;
	int i, nrworkers = 0;

	slicingLocal = 0;

	/* we have a new list of candidate peers */
	for (i=0; i<nrpeers; i++)
		peers[i].active = 0;

	msg = RMTresolve(&bid,&s);
	if ( msg == MAL_SUCCEED) {
		b = BATdescriptor(bid);
		if ( b != NULL && BATcount(b) > 0 ) {
			bi = bat_iterator(b);
			BATloop(b,p,q){
				str t= (str) BUNtail(bi,p);
				nrworkers += OPTgetPeer(t) >= 0;
			}
		}
		BBPreleaseref(bid);
	} else
		GDKfree(msg);

	if ( !nrworkers  ) {
	 	/* there is a last resort, local execution */
		SABAOTHgetLocalConnection(&s);

		nrworkers += OPTgetPeer(s) >= 0;
		slicingLocal = 1;
	}

#ifdef DEBUG_RUN_OPT
	mnstr_printf(cntxt->fdout,"Active peers discovered %d\n",nrworkers);
	for (i=0; i<nrpeers; i++)
	if ( peers[i].uri )
		mnstr_printf(cntxt->fdout,"%s\n", peers[i].uri);
#else
		(void) cntxt;
#endif

	for (i=0; i<nrpeers; i++)
		if ( !peers[i].active )
			OPTcleanFunReg(i);

	return MAL_SUCCEED;
}

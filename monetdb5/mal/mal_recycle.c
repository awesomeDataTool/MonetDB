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
 * Copyright August 2008-2013 MonetDB B.V.
 * All Rights Reserved.
 */

/*
 * (c) M. Ivanova, M. Kersten, N. Nes
 * mal_recycle
 * Query optimization and processing in off-the-shelf database systems is often
 * still focussed on individual queries. The queries are analysed in isolation
 * and ran against a kernel regardless opportunities offered by concurrent or
 * previous invocations.
 *
 * This approach is far from optimal and two directions to improve
 * are explored: materialized views and (partial) result-set reuse.
 * Materialized views are derived from query logs. They represent
 * common sub-queries, whose materialization improves
 * subsequent processing times.
 * Re-use of (partial) results is used in those cases where a
 * zooming-in or navigational application is at stake.
 *
 * The Recycler module extends it with a middle out approach.
 * It exploits the materialize-all-intermediate approach of MonetDB
 * by deciding to keep a hold on them as long as deemed benificial.
 *
 * The approach taken is to mark the instructions in a MAL program
 * using an optimizer call, such that their result is retained
 * in a global recycle cache. A reference into the cache makes
 * is used to access the latest known version quickly.
 *
 * Upon execution, the Recycler first checks for
 * an up to date result to be picked up at no cost,
 * other than matching the arguments.
 * Otherwise, it evaluates the instruction and calls upon
 * policy functions to decide if it is worthwhile to
 * keep.
 *
 * The Recycler comes with a few policy controlling operators
 * to experiment with its effect in concrete settings.
 *
 * Caveats:
 * Updates in general should immediately invalidate the cache lines depending on the updated Bats. These are all instructions directly operating on the updates, as well as all instructions consuming the result of the first.
 * 
 * Unlike the background publication, we keep a single focus for the released functionality
 * ADM_ALL: infinite case, admission of all instructions subject to cache limits
 * REUSE_COVER: exploit potential range overlap
 * RCACHE_PROFIT: use semantic driven LRU§
 */
#include "monetdb_config.h"
#include "mal_recycle.h"
#include "mal_exception.h"
#include "mal_interpreter.h"
#include "mal_function.h"
#include "mal_listing.h"
#include "mal_runtime.h"

static MT_Lock recycleLock MT_LOCK_INITIALIZER("recycleLock");
MalBlkPtr recycleBlk = NULL;

#define set1(x,i) ( x | ((lng)1 << i) )
#define set0(x,i) ( x & ~((lng)1 << i) )
#define getbit(x,i) ( x & ((lng)1 << i) )
#define neg(x) ( (x)?FALSE:TRUE)

static void RECYCLEexitImpl(Client cntxt,MalBlkPtr mb, MalStkPtr stk, InstrPtr p, int pc, lng ticks);

/* ADM_ALL: infinite case, admission of all instructions subject to cache limits*/

lng recycleTime = 0;
lng recycleSearchTime = 0;	/* cache search time in ms*/
lng msFindTime = 0;			/* multi-subsume measurements */
lng msComputeTime = 0;
int recycleMaxInterest = REC_MAX_INTEREST;

/* REUSE_COVER: exploit potential range overlap */
int reusePolicy = REUSE_COVER;	

/*	evict items with smallest profit= weight * cost / lifetime adds aging to the benefit policy */

int recycleCacheLimit=0; /* No limit by default */
lng recycleMemory=0;	/* Units of memory permitted */

/*
 * Monitoring the Recycler
 */
lng recyclerMemoryUsed = 0;
int monitorRecycler = 0;
	/*	 1: print statistics for RecyclerPool only
		 2: print stat at the end of each query
		 4: print data transfer stat for octopus */

#ifdef _DEBUG_CACHE_
#define recycleSize recycleBlk->stop - cntxt->rcc->recycleRem
#else
#define recycleSize recycleBlk->stop
#endif

/*
 * The profiler record is re-used to store recycler information.
 * The clk is used by the LRU scheme, counter is the number of
 * times this pattern was used, ticks is the clock ticks
 * used to produce the result. rbytes+wbytes depict the storage
 * size of operands and result arguments.
 *
 * The cost function is a weighted balance between cpu and
 * storage cost. Often there is a direct relationship,
 */

#define recycleCost(X) (recycleBlk->profiler[X].wbytes)
#define recycleW(X)  ((recycleBlk->profiler[X].trace && (recycleBlk->profiler[X].calls >1 )) ? \
						(recycleBlk->profiler[X].calls -1) : 0.1 )

#define recycleLife(X) ((GDKusec() - recycleBlk->profiler[X].rbytes)/ 1000.0)
#define recycleProfit(X) (recycleCost(X) * recycleW(X) / recycleLife(X))


#define setIPtr(q,i,cst,c)								\
	do {												\
		VALset(&cst,TYPE_int,&i);						\
		c = defConstant(recycleBlk,TYPE_int, &cst);		\
		q = pushArgument(recycleBlk,q,c);				\
		setVarUsed(recycleBlk,c);						\
	} while (0)

/*
 * The recycler keeps a catalog of query templates
 * with statistics about number of calls, global/local reuses,
 * and credits per instruction.
 *
 */
RecyclePool recyclePool = NULL;

void RECYCLEinitRecyclePool(int sz)
{
	if (recyclePool == NULL) {
		MT_lock_set(&recycleLock, "recycle");
		if (recyclePool == NULL) {
			recyclePool = (RecyclePool) GDKzalloc(sizeof(RecyclePoolRec));
			recyclePool->ptrn = (QryStatPtr *) GDKzalloc(sz * sizeof(QryStatPtr));
			recyclePool->sz = sz;
		}
		MT_lock_unset(&recycleLock, "recycle");
	}
}

static void extendRecyclePool(void)
{
	int s, i;
	QryStatPtr *old;

	if (recyclePool == NULL)
		RECYCLEinitRecyclePool(1024);
	if (recyclePool->cnt < recyclePool->sz)
		return;
	MT_lock_set(&recycleLock, "recycle");
	old = recyclePool->ptrn;
	s = recyclePool->sz +1024;	/* lineare growth is enough */
	recyclePool->ptrn = (QryStatPtr *) GDKzalloc(s * sizeof(QryStatPtr));
	for( i=0; i< recyclePool->cnt; i++)
		 recyclePool->ptrn[i] = old[i];
	recyclePool->sz = s;
	MT_lock_unset(&recycleLock, "recycle");
	GDKfree(old);
}

static int findQryStat(lng recid)
{
	int i;

	if (recyclePool == NULL)
		return -1;
	// needs hash
	for(i = 0; i< recyclePool->cnt; i++)
		if ( recyclePool->ptrn[i]->recid == recid)
			return i;
	return -1;
}

int RECYCLEnewQryStat(MalBlkPtr mb)
{
	int idx, i;
	QryStatPtr qstat;

	/* no need to keep statistics for statements without instructions
	marked for recycling, for instance DML */
	if ( !mb->recycle )
		return -1;

    /* the pattern exists */
	if ((idx = findQryStat(mb->recid)) >= 0){
		qstat = recyclePool->ptrn[idx];
		qstat->calls++;
		return idx;
	}
    /* add new query pattern */
	qstat = (QryStatPtr) GDKzalloc(sizeof(QryStat));
	qstat->recid = mb->recid;
	qstat->calls = 1;
	qstat->stop = mb->stop;
	qstat->crd = (int *) GDKzalloc(sizeof(int)* qstat->stop);
	for (i=0; i<mb->stop; i++)
		qstat->crd[i] = mb->stmt[i]->recycle;
	qstat->gl = (bte *) GDKzalloc(sizeof(bte)* qstat->stop);
	extendRecyclePool();
	idx = recyclePool->cnt++;
	recyclePool->ptrn[idx] = qstat;

	return idx;
}

static void updateQryStat(int qidx, bit gluse, int i)
{
	QryStatPtr qs;

	if ( qidx < 0 || qidx >= recyclePool->cnt){
	/*	fprintf(stderr, "Query pattern %d does not exist\n",qidx);*/
		return;
	}
	qs = recyclePool->ptrn[qidx];
	if (gluse) {
		qs->greuse++;
		qs->gl[i] = 1;
		if ( qs->wl < i ) qs->wl = i;
	}
	else qs->lreuse++;
}

static void emptyRecyclePool(RecyclePool q)
{
    int i;
    QryStatPtr qstat;

    for( i = 0; i < q->cnt; i++){
        qstat = q->ptrn[i];
        GDKfree(qstat->crd);
		GDKfree(qstat->gl);
        GDKfree(qstat);
    }
    GDKfree(q->ptrn);
    GDKfree(q);
}

/* the source of a recycled instruction q receives its credit back */
static void returnCredit(InstrPtr q)
{
	int i, pc;
	QryStatPtr qs;

	i = q->argv[q->argc-1];
	pc = *(int*)getVarValue(recycleBlk,i);
	if ((q->recycle >= 0) && (q->recycle <recyclePool->cnt)){
		qs = recyclePool->ptrn[q->recycle];
		if (pc < qs->stop)
			if ( qs->crd[pc] < recycleMaxInterest )
				qs->crd[pc]++;
	}
}


/*
 * The Recycle catalog is a global structure, which should be
 * protected with locks when updated.
 * The recycle statistics can be kept in the performance table
 * associated with the recycle MAL block without problems, because
 * the block is never executed.
 */
static void RECYCLEspace(void)
{
	if (recycleBlk == NULL) {
		recycleBlk = newMalBlk(MAXVARS, STMT_INCREMENT);
		recycleBlk->profiler = (ProfPtr) GDKzalloc(
			recycleBlk->ssize*sizeof(ProfRecord));
	}
}

void RECYCLEinit(void){
#ifdef NEED_MT_LOCK_INIT
	MT_lock_init(&recycleLock,"recycleLock");
#endif
	RECYCLEinitRecyclePool(20);
}

/*
 * The cache of recycled instructions can be kept low.
 * Once the cache is filled, we have to drop instructions
 * and associated variables. At the same time we should
 * invalidate the cache version, such that others can
 * re-synchronize.
 *
 * For the recycle cache LRU scheme we mis-use a field in
 * the performance record.
 *
 * Removal of instructions should be done with care.
 * First, there may be references kept around to
 * variables in the recycle cache. They should be
 * invalidated.
 * Second, for each (BAT) variable removed, we
 * should also remove the dependent instructions.
 */
static void
RECYCLEgarbagecollect(MalBlkPtr mb, InstrPtr q, bte *used){
	int j;
	ValPtr v;

	for(j=0; j< q->argc; j++){
		v= &getVarConstant(mb,getArg(q,j));
		if(getArgType(mb,q,j)==TYPE_bat || isaBatType(getArgType(mb, q,j)) ){
			if( v->val.bval ){
				BBPdecref(ABS(v->val.bval), TRUE);
				if (!BBP_lrefs(v->val.bval)){
					v->vtype= TYPE_int;
					v->val.ival= 0;
				}
			}
		}
		if( v->vtype == TYPE_str && used[getArg(q,j)]<=1) {
			if(v->val.sval) {
				GDKfree(v->val.sval);
				v->val.sval = NULL;
			}
			v->len = 0;
		}
	}
}

/* lvs : array of indices of leaf instructions */

static
int chooseVictims(Client cntxt, int *lvs, int ltop, lng wr)
{
	dbl *wben, ben = 0, maxwb, tmp, ci_ben, tot_ben;
	int l, newtop, mpos, tmpl, ci = 0;
	lng sz, totmem = 0, targmem, smem;

	(void) cntxt;

	wben = (dbl *)GDKzalloc(sizeof(dbl)*ltop);
	for (l = 0; l < ltop; l++){
		sz = recycleBlk->profiler[lvs[l]].wbytes;
		ben = recycleProfit(lvs[l]);
		wben[l] = sz? ben / sz : -1;
		totmem += sz;
	}
	if (totmem <= wr) { /* all leaves need to be dropped */
		GDKfree(wben);
		return ltop;
	}

	/* reorder instructions on increasing weighted credit */
	/* knapsack: find a set with biggest wben fitting in totmem-wr.
	 They are most benefitial and can be saved from dropping */

	targmem = totmem - wr; /*sack volume */
	smem = 0;
	tot_ben = 0;
	for(newtop = ltop; newtop>0; newtop--){
		maxwb = 0; mpos = newtop-1;		/* find most benefitial item (weighted ben.)*/
		for(l = 0; l<newtop; l++){
			if ((lng) recycleBlk->profiler[lvs[l]].wbytes > targmem - smem)
				wben[l] = -1;
			if (maxwb < wben[l]){
				maxwb = wben[l];
				mpos = l;
			}
		}
		if ( maxwb ){			   /* add it to the sack (newtop - ltop)*/
			smem += recycleBlk->profiler[lvs[mpos]].wbytes;
			tmpl = lvs[mpos];
			lvs[mpos] = lvs[newtop-1];
			lvs[newtop-1] = tmpl;
			tmp = wben[mpos];
			wben[mpos] = wben[newtop-1];
			wben[newtop-1] = tmp;
			tot_ben += recycleProfit(tmpl);
#ifdef _DEBUG_CACHE_
		        mnstr_printf(cntxt->fdout,"#Don't drop instruction %d, credit %f\n" , tmpl,tmp);
#endif
		}
		else break;
	}
	/* compare benefits of knap-sack content and the critical item */
	ci_ben = 0;             /* find the critical item */
    for (l = 0; l < ltop; l++) {
		ben = recycleProfit(lvs[l]);
		if (recycleBlk->profiler[lvs[l]].wbytes <= targmem &&
			ben > ci_ben){
			ci = l;
			ci_ben = ben;
		}
	}

	if ( ci_ben > tot_ben ) { /* save the critical item instead */
		newtop = ltop - 1;
		tmpl = lvs[ci];
		lvs[ci] = lvs[newtop];
		lvs[newtop] = tmpl;
#ifdef _DEBUG_CACHE_
		mnstr_printf(cntxt->fdout,"#Don't drop critical item : instruction %d, credit %f\n" ,tmpl,ci_ben);
#endif
	}
	GDKfree(wben);
	return newtop;

}


static void RECYCLEcleanCache(Client cntxt, lng wr0){
	int j,i,l,ltop,v,vtop;
	InstrPtr p;
	InstrPtr *old, *newstmt;
	bit  *lmask, *dmask;
	int k, *lvs, *vm;
	int limit, mem, idx;
	int cont, reserve;
	lng oldclk, wr;
	dbl minben, ben;
	bte *used;
	lng memLimit = recycleMemory?recycleMemory:HARDLIMIT_MEM;

	if (!recycleBlk)
		return;

	cntxt->rcc->ccCalls++;
newpass:
	cont = 0;
	wr = wr0;
	used = (bte*)GDKzalloc(recycleBlk->vtop);

	/* set all used variables */
	for (i = 0; i < recycleBlk->stop; i++){
		p = recycleBlk->stmt[i];
#ifdef _DEBUG_CACHE_
                if ( p->token != NOOPsymbol )
#endif
		for( j = p->retc ; j< p->argc; j++)
			if (used[getArg(p,j)]<2)  used[getArg(p,j)]++;
	}

	/* find the leaves, ignore the most recent instruction */

	lmask = (bit*)GDKzalloc(recycleBlk->stop);
	ltop = 0; reserve = 0;
	for (i = 0; i < recycleBlk->stop; i++){
		p = recycleBlk->stmt[i];
#ifdef _DEBUG_CACHE_
                if ( p->token == NOOPsymbol ) continue;
#endif
		for( j = 0; j < p->retc ; j++)
			if (used[getArg(p,j)]) goto skip;
		if (i == cntxt->rcc->recent){
			reserve = i;
			continue;
		}
		lmask[i] = 1;
		ltop++;
		skip:;
	}


	if (ltop == 0 ){  /* ensure at least 1 entry to evict */
		if (reserve){
			lmask[reserve] = 1;
			ltop++;
		}
		else {	GDKfree(lmask);
			return;
		}
	}
	lvs = (int *)GDKzalloc(sizeof(int)*ltop);
	l = 0;
	for (i = 0; i < recycleBlk->stop; i++)
		if (lmask[i]) lvs[l++] = i;
	GDKfree(lmask);

	/* find the oldest */
	oldclk = recycleBlk->profiler[lvs[0]].clk;
	idx = 0;
	for (l = 0; l < ltop; l++){
		k = lvs[l];
		if( recycleBlk->profiler[k].clk < oldclk){
			oldclk = recycleBlk->profiler[k].clk;
			idx = l;
		}
	}

	/* protect leaves from current query invocation */

	if ( oldclk < cntxt->rcc->time0) {

#ifdef _DEBUG_CACHE_
			mnstr_printf(cntxt->fdout,"#Fresh-protected "LLFMT"\n", cntxt->rcc->time0);
      			mnstr_printf(cntxt->fdout,"#All leaves:");
			for (l = 0; l < ltop; l++)
                		mnstr_printf(cntxt->fdout,"%3d("LLFMT") \t",
	                		lvs[l],recycleBlk->profiler[lvs[l]].clk);
			mnstr_printf(cntxt->fdout,"\n");
#endif
		l = 0;
		for (j = 0; j < ltop; j++){
			if (recycleBlk->profiler[lvs[j]].clk < cntxt->rcc->time0)
				lvs[l++] = lvs[j];
		}
		ltop = l;
	}


#ifdef _DEBUG_CACHE_
        mnstr_printf(cntxt->fdout,"\n#RECYCLEcleanCache: policy=PROFIT mem="LLFMT" usedmem="LLFMT"\n",
                recycleMemory,recyclerMemoryUsed);
		mnstr_printf(cntxt->fdout,"#Target memory "LLFMT"KB Available "LLFMT"KB\n", wr,memLimit-recyclerMemoryUsed);
        mnstr_printf(cntxt->fdout,"#Candidates for eviction\n#(# LRU\t\tTicks\tLife\tSZ\tCnt\tWgt\tBen\tProf)\n");
		for (l = 0; l < ltop; l++)
        	mnstr_printf(cntxt->fdout,"%3d "LLFMT"\t"LLFMT"\t %5.2f\t "LLFMT"\t%3d\t%5.1f\n",
                	lvs[l],recycleBlk->profiler[lvs[l]].clk,
	                recycleBlk->profiler[lvs[l]].ticks,
        	        recycleLife(lvs[l]),
                	recycleBlk->profiler[lvs[l]].wbytes,
	                recycleBlk->profiler[lvs[l]].calls,
        	        recycleW(lvs[l]));
#endif

	/* find entries to evict */
	mem = recyclerMemoryUsed + wr > memLimit;
	vm = (int *)GDKzalloc(sizeof(int)*ltop);
	vtop = 0;

	if (!mem){	 /* evict 1 entry */
		minben = recycleProfit(lvs[0]);
		idx = 0;
		for (l = 1; l < ltop; l++){
			ben = recycleProfit(lvs[l]);
			if( ben < minben) {
				minben = ben;
				idx = l;
			}
		}
		vm[vtop++] = lvs[idx];
	} else {	/* evict several to get enough memory */
		k = 0;	/* exclude binds that don't free memory */
		for (l = 0; l < ltop; l++)
			if ( recycleBlk->profiler[lvs[l]].wbytes > 0 )
				lvs[k++] = lvs[l];
/*				mnstr_printf(cntxt->fdout,"ltop %d k %d\n",ltop, k); */
		if ( k > 0 )
			ltop = k;
		vtop = chooseVictims(cntxt,lvs, ltop, recyclerMemoryUsed + wr - memLimit);
		for (v = 0; v < vtop; v++){
			vm[v] = lvs[v];
			wr -= recycleBlk->profiler[lvs[v]].wbytes;
		}
	}

	/* check if a new pass of cache cleaning is needed */
	if (recyclerMemoryUsed + wr > memLimit)
		cont = 1;

#ifdef _DEBUG_CACHE_
	mnstr_printf(cntxt->fdout,"#Evicted %d instruction(s) \n ",vtop);
	for(v=0; v<vtop;v++){
		mnstr_printf(cntxt->fdout,"%d\t",vm[v]);
		printInstruction(cntxt->fdout,recycleBlk,0,recycleBlk->stmt[vm[v]], LIST_MAL_ALL);
		mnstr_printf(cntxt->fdout,"\n");
	}
#endif

	GDKfree(lvs);
	/* drop victims in one pass */
	dmask = (bit *)GDKzalloc(recycleBlk->stop);
	for (v = 0; v < vtop; v++)
		dmask[vm[v]] = 1;
	GDKfree(vm);

#ifdef _DEBUG_CACHE_
	/* instructions are marked with NOOPsymbol in debug mode */
	(void) old;
	(void) limit;
	for (i = 0; i < recycleBlk->stop ; i++){
		p = getInstrPtr(recycleBlk,i);
		if( dmask[i] ) {
			recyclerMemoryUsed -= recycleBlk->profiler[i].wbytes;
         p->token = NOOPsymbol;
			cntxt->rcc->recycleRem ++;
			cntxt->rcc->ccInstr++;
			if ( recycleBlk->profiler[i].calls >1)
				returnCredit(p);
		}
	}
	(void) newstmt;

#else
	old = recycleBlk->stmt;
	limit = recycleBlk->stop;
/*	newMalBlkStmt(recycleBlk,recycleBlk->ssize);
	we want to keep the profiler records and get only new stmt*/

    newstmt = (InstrPtr *) GDKzalloc(sizeof(InstrPtr) * recycleBlk->ssize);
    if (newstmt == NULL){
        GDKerror("newMalBlk:"MAL_MALLOC_FAIL);
        return;
    }
    recycleBlk->stmt = newstmt;
    recycleBlk->stop = 0;

	k = 0;
	for (i = 0; i < limit ; i++){
		p = old[i];
		if( dmask[i] ) {
			RECYCLEgarbagecollect(recycleBlk,p,used);
			recyclerMemoryUsed -= recycleBlk->profiler[i].wbytes;
			if ( recycleBlk->profiler[i].calls >1)
				returnCredit(p);
			freeInstruction(p);
			cntxt->rcc->ccInstr++;
		}
		else {
			pushInstruction(recycleBlk,p);
			recycleBlk->profiler[k++]= recycleBlk->profiler[i];
		}
	}

	GDKfree(old);
	GDKfree(used);
	/* remove all un-used variables as well */
	trimMalVariables(recycleBlk);
#endif
	GDKfree(dmask);
	if (cont) goto newpass;

}

/*
 * To avoid a polution of the recycle cache, we do not store any
 * intruction for which there is not function/command/pattern implementation.
 * Likewise, we avoid all simple constant assigments.
 */
int
RECYCLEinterest(InstrPtr p){
	if (p->recycle <= REC_NO_INTEREST || p->token== ASSIGNsymbol )
		return 0;
	return getFunctionId(p) != NULL;
}


bit
isBindInstr(InstrPtr p)
{
	static str sqlRef = 0, bindRef = 0, binddbatRef = 0, bindidxRef = 0;

	if (sqlRef == 0) {
		sqlRef = putName("sql",3);
		bindRef = putName("bind",4);
		binddbatRef = putName("bind_dbat",9);
		bindidxRef = putName("bind_idxbat",11);
	}

	if ( getModuleId(p) != sqlRef ) return 0;
	return ( bindRef == getFunctionId(p) ||
		 binddbatRef == getFunctionId(p) ||
		 bindidxRef == getFunctionId(p));
}

#ifdef _DEBUG_CACHE_
static void
RECYCLEsync(InstrPtr p)
{
	int i, j, k;
	InstrPtr q;
	ValPtr pa, qa;

	for (i=0; i<recycleBlk->stop; i++) {
		q = getInstrPtr(recycleBlk,i);
		if ( q->token != NOOPsymbol ) continue;
		if ((getFunctionId(p) != getFunctionId(q)) ||
			(p->argc != q->argc) ||
			(getModuleId(p) != getModuleId(q)))
			continue;
		for (j=p->retc; j<p->argc; j++)
			if( VALcmp( &getVarConstant(recycleBlk,getArg(p,j)),
						&getVarConstant(recycleBlk,getArg(q,j))))
				break;
		if (j == p->argc) {
			for(k=0; k< p->retc; k++){
				pa = &getVarConstant(recycleBlk,getArg(p,k));
				qa = &getVarConstant(recycleBlk,getArg(q,k));
				if (qa->vtype == TYPE_bat)
					BBPdecref( *(const int*)VALptr(qa), TRUE);
				VALcopy(qa,pa);
				if (qa->vtype == TYPE_bat)
					BBPincref( *(const int*)VALptr(qa), TRUE);
			}
		}
	}
}
#endif

/* algebra.subselect(r:bat[:oid,:any],low,hgh,true,true,false) */
static void
setSelectProp(InstrPtr q)
{
	ValPtr lb = NULL, ub = NULL;
	ValRecord lbval, ubval, nilval;
	int bid=0, tpe = 0;
	ptr nilptr = NULL;
	int (*cmp) (const void *, const void *) = NULL;

	static str subselectRef = 0, thetasubselectRef = 0;
	int tlbProp = PropertyIndex("tlb");
	int tubProp = PropertyIndex("tub");

	if (subselectRef == 0)
		subselectRef = putName("subselect",9);
	if (thetasubselectRef == 0)
		thetasubselectRef = putName("thetasubselect",14);


	if ( ((getFunctionId(q) == subselectRef ) || (getFunctionId(q) == thetasubselectRef )) &&
		BATatoms[getArgType(recycleBlk,q,3)].linear ){

		if ( getFunctionId(q) == subselectRef ) {
			lb = &getVar(recycleBlk,getArg(q,3))->value;
			if (q->argc == 6)
				ub = &getVar(recycleBlk,getArg(q,3))->value;
			else ub = &getVar(recycleBlk,getArg(q,4))->value;
			tpe = lb->vtype;
			nilptr = ATOMnilptr(tpe);
			cmp = BATatoms[tpe].atomCmp;
		}

		if ( getFunctionId(q) == thetasubselectRef ) {
			ValPtr qval;
			str qop;

			if (q->argc == 6){
				qop = (str)getVarValue(recycleBlk,getArg(q,3));
				qval = &getVar(recycleBlk,getArg(q,2))->value;
			} else {
				qop = (str)getVarValue(recycleBlk,getArg(q,3));
				qval = &getVar(recycleBlk,getArg(q,2))->value;
			}
			tpe = qval->vtype;
			nilptr = ATOMnilptr(tpe);
 			cmp = BATatoms[tpe].atomCmp;

			VALset(&nilval, tpe, ATOMnil(tpe));
			lb = &nilval;
			ub = &nilval;
			if ( qop[0] == '=') {
				lb = qval;
				ub = qval;
			}
			if ( qop[0] == '<')
				ub = qval;
			else if (qop[0] == '>')
				lb = qval;
        }

		bid = getVarConstant(recycleBlk, getArg(q,0)).val.bval;
		(void) bid;

		if ( (*cmp)(VALptr(lb), nilptr) == 0 ) {
			/* try to propagate from base relation */
			if ( varGetProp(recycleBlk, getArg(q,1), tlbProp) != NULL )
				lb = &varGetProp(recycleBlk, getArg(q,1), tlbProp)->value;
			else {
				lb = &lbval;
/*				msg = ALGminany(lb, &bid); */
				//(*minAggr)(lb, &bid);
				assert(0);
				lb->vtype = tpe;
				/* first computation - propagate to base relation  */
				varSetProp(recycleBlk, getArg(q,1), tlbProp, op_gte, lb);
			}
		}
		if ( (*cmp)(VALptr(ub), nilptr) == 0 ){
			if ( varGetProp(recycleBlk, getArg(q,1), tubProp) != NULL )
				ub = &varGetProp(recycleBlk, getArg(q,1), tubProp)->value;
			else {
				ub = &ubval;
				/*	msg = ALGmaxany(ub, &bid); */
				//(*maxAggr)(ub, &bid);
				assert(0);
				ub->vtype = tpe;
				/* propagate to base relation */
				varSetProp(recycleBlk, getArg(q,1), tubProp, op_lte, ub);
			}
		}
		varSetProp(recycleBlk, getArg(q,0), tlbProp, op_gte, lb);
		varSetProp(recycleBlk, getArg(q,0), tubProp, op_lte, ub);
	}

}

static void
RECYCLEkeep(Client cntxt, MalBlkPtr mb, MalStkPtr s, InstrPtr p, int pc, lng rd, lng wr, lng ticks)
{
	int i, j, c;
	ValRecord *v;
	ValRecord cst;
	InstrPtr q;
	lng memLimit;
	lng cacheLimit;
	static str octopusRef = 0, bindRef = 0, bindidxRef = 0;

	if (octopusRef == 0)
		octopusRef = putName("octopus",7);
	if (bindRef == 0)
		bindRef = putName("bind",4);
	if (bindidxRef == 0)
		bindidxRef = putName("bind_idxbat",11);

/*	(void) rd;	*/
	RECYCLEspace();
	cacheLimit = recycleCacheLimit?recycleCacheLimit:HARDLIMIT_STMT;
	if ( recycleSize >= cacheLimit)
		return ; /* no more caching */
	memLimit = recycleMemory?recycleMemory:HARDLIMIT_MEM;
	if ( recyclerMemoryUsed + wr > memLimit)
		return ; /* no more caching */

#ifdef _DEBUG_RECYCLE_
	mnstr_printf(cntxt->fdout,"#RECYCLE keep ");
	printTraceCall( cntxt->fdout,mb, s, getPC(mb,p),LIST_MAL_ALL);
	mnstr_printf(cntxt->fdout," Tolls %d\n",p->recycle);
#else
	(void) cntxt;
	(void) mb;
#endif
	/*
	 * The instruction is copied and the variables are
	 * all assigned to the symbol table. This means the
	 * link with their source disappears. We can later only
	 * compare by value.
	 */
	q = copyInstruction(p);
	for (i = 0; i< p->argc; i++) {
		j= getArg(p,i);
		v = &s->stk[j];
		VALcopy(&cst,v);
		c = fndConstant(recycleBlk, &cst, recycleBlk->vtop);
#ifdef _DEBUG_RECYCLE_
		printf("#CONSTANT %s %d\n", getVarName(mb,j), c);
#endif
		if (c<0)
			c = defConstant(recycleBlk, v->vtype, &cst);
		if (v->vtype == TYPE_bat)
			BBPincref( *(const int*)VALptr(v), TRUE);
		setVarUsed(recycleBlk,c);
	 	setArg(q,i,c);
	}
#ifdef _DEBUG_RECYCLE_
	mnstr_printf(cntxt->fdout,"#RECYCLE kept ");
	printInstruction( cntxt->fdout,recycleBlk, 0, q,LIST_MAL_ALL);
	mnstr_printf(cntxt->fdout,"\n");
#endif

	setIPtr(q,pc,cst,c);
	q->recycle = cntxt->rcc->curQ;
		/* use the field to refer to the query-owner index in the query pattern table */
	pushInstruction(recycleBlk,q);
	i = recycleBlk->stop-1;
/*	recycleBlk->profiler[i].rbytes = recycleBlk->profiler[i].clk = GDKusec(); */
	recycleBlk->profiler[i].clk = GDKusec();
	recycleBlk->profiler[i].calls =1;
	recycleBlk->profiler[i].ticks = ticks;
	recycleBlk->profiler[i].rbytes = rd;
	recycleBlk->profiler[i].wbytes = wr;
	recyclerMemoryUsed += wr;
	if (monitorRecycler == 1 )
		fprintf(stderr,
			"#memory="LLFMT", stop=%d, recycled=%d, executed=%d \n",
			recyclerMemoryUsed, recycleBlk->stop,
			cntxt->rcc->recycled0, cntxt->rcc->statements);

	if ( getModuleId(p) == octopusRef &&
			 (getFunctionId(p) == bindRef || getFunctionId(p) == bindidxRef) )
		recyclePool->ptrn[cntxt->rcc->curQ]->dt += wr;

	cntxt->rcc->recent = i;
	cntxt->rcc->RPadded0++;
	setSelectProp(q);

#ifdef _DEBUG_CACHE_
	RECYCLEsync(q);
#endif
}

/*
 * The generic wrappers for accessing the recycled instructions.
 * Before the interpreter loop is allowed to execute the instruction
 * we check the recycle table for available variables.
 *
 * Searching for useful recycle instructions is the real challenge.
 * There are two major approaches. The first approach is to search
 * for an identical operation as the target under investigation and
 * reuse its result if possible.
 * The second approach uses the semantics of operations and
 * replaces the arguments of the target to make it cheaper to execute.
 * For example, a previous result of a scan may be used instead
 * or it can be small compared to the target.
 *
 * We should avoid adding the same operation twice, which
 * means it should be easy to find them in the first place.
 * Furthermore, we should only search for instructions if
 * we are dealing with a function call.
 */
static int
RECYCLEfind(Client cntxt, MalBlkPtr mb, MalStkPtr s, InstrPtr p)
{
	int i, j;
	InstrPtr q;
	lng clk = GDKusec();

	(void) mb;
	if( recycleBlk == 0)
		return -1;

#ifdef _DEBUG_RECYCLE_
	mnstr_printf(cntxt->fdout,"#search ");
	printInstruction(cntxt->fdout,mb,0,p, LIST_MAL_ALL);
	mnstr_printf(cntxt->fdout,"\n");
#else
	(void) cntxt;
#endif
	for (i=0; i<recycleBlk->stop; i++) {
		q = getInstrPtr(recycleBlk,i);
		if ((getFunctionId(p) != getFunctionId(q)) ||
			(p->argc != q->argc-1) ||
			(getModuleId(p) != getModuleId(q)))
				continue;
		for (j=p->retc; j<p->argc; j++)
			if( VALcmp( &s->stk[getArg(p,j)], &getVarConstant(recycleBlk,getArg(q,j))))
				break;
		if (j == p->argc)
#ifdef _DEBUG_CACHE_
			if ( q->token != NOOPsymbol )
#endif
			return i;
	}
#ifdef _DEBUG_RECYCLE_
	mnstr_printf(cntxt->fdout,"# NOT found\n");
#endif
	recycleSearchTime = GDKusec()-clk;
	return -1;
}


#define boundcheck(flag,a) ((flag)?a<=0:a<0)
/* check if instruction p at the stack is a subset selection of the RecyclerPool instruction q */

static bit
selectSubsume(InstrPtr p, InstrPtr q, MalStkPtr s)
{
	int lcomp, rcomp;
	bit li, hi, lip, hip, cover=0;
	int base =2;
	if ( p->argc >7) // candidate list
		base = 3;

	lcomp = VALcmp(&getVar(recycleBlk,getArg(q,base))->value,
			&s->stk[getArg(p,base)]);
	if ( p->argc == base+1)
		rcomp = VALcmp( &s->stk[getArg(p,base)],
			&getVar(recycleBlk,getArg(q,base+1))->value);
	else
		rcomp = VALcmp( &s->stk[getArg(p,base+1)],
			&getVar(recycleBlk,getArg(q,base+1))->value);
	if ( q->argc == base +4)
		cover = ( lcomp <=0 && rcomp <=0 );
	if ( q->argc == base +5){
		li = *(bit*)getVarValue(recycleBlk,getArg(q,base+2));
		hi = *(bit*)getVarValue(recycleBlk,getArg(q,base+3));
		if (p->argc <=4)
			cover = boundcheck(li,lcomp) && boundcheck(hi,rcomp);
		else {
			lip = *(const bit*)VALptr(&s->stk[getArg(p,base+2)]);
			hip = *(const bit*)VALptr(&s->stk[getArg(p,base+3)]);
			cover = ( boundcheck(li || neg(lip),lcomp) && boundcheck(hi || neg(hip),rcomp) );
		}
	}
	return cover;
}


static bit
likeSubsume(InstrPtr p, InstrPtr q, MalStkPtr s)
{
	str ps, qs, pd, qd, ps0, qs0;
	size_t pl, ql;
	bit first = 1, endp = 0, stop = 0, cover = 0;

	ps = GDKstrdup(s->stk[getArg(p,2)].val.sval);
	qs = GDKstrdup(getVar(recycleBlk,getArg(q,2))->value.val.sval);

	ps0 = ps; qs0 = qs;

	while ((qd = strchr(qs,'%')) != NULL && !stop && !endp){
		*qd = 0;
		if ((pd = strchr(ps,'%')) == NULL)
			endp = 1;
		else *pd = 0;
		ql = strlen(qs);
   	    pl = strlen(ps);
	    if (ql > pl ){
			stop = 1;
			break;
	    }
		if (first){
			if (strncmp(ps,qs,ql) != 0) {
				stop = 1;
				break;
			}
			first = 0;
		}
		else {
	 		if (strstr(ps,qs) == NULL) {
				stop = 1;
				break;
			}
		}
		if (!endp) ps = pd+1;
		qs = qd+1;
 	}

	if (stop) cover = 0;
	else  /* compare remainders after the last % in some of the strings*/
		if (strcmp(ps,"")==0 && strcmp(qs,"")==0) { /*printf("Successful subsumption\n");*/
      cover = 1;
   } else {
	    ql = strlen(qs);
        pl = strlen(ps);
   	if (ql > pl ){			/* printf("No subsumption\n"); */
      	cover = 0;
   	}
		else if (strncmp(ps+pl-ql,qs,ql) != 0) {/*printf("No subsumption\n"); */
			cover = 0;
		}
		else cover = 1;
	}
	GDKfree(ps0);
	GDKfree(qs0);
	return cover;
}


/* p,q margins of 2 intervals. Returns true if p is less than q:
- p is nil (incl q and p are nil)
- p and q != nil and p < q
- if eq is TRUE also checks for equality
*/


static bit
lessEq(ValPtr p, bit pi, ValPtr q, bit qi, bit eq)
{

    int (*cmp) (const void *, const void *);
    int tpe, c;
    const void *nilptr, *pp, *pq;

    if( p == 0 || q == 0 ) return  0;
    if( (tpe = p ->vtype) != q->vtype ) return  0;

    cmp = BATatoms[tpe].atomCmp;
    nilptr = ATOMnilptr(tpe);
    pp = VALptr(p);
    pq = VALptr(q);
    if( (*cmp)(pp, nilptr) == 0 ) return 1; /* p is nil */
    if( (*cmp)(pq, nilptr) == 0 )  return 0;
    c = (*cmp)(pp, pq);
	 if ( c < 0 ) return 1;
	 if (c == 0 ) {
			if ( eq ) return (pi || neg(qi)); /* less or eq */
			else return (pi && neg(qi));		/* strict less */
	 }
	 return 0;
}

/* p,q margins of 2 intervals. Returns true if p is greater than q:
- p is nil (incl q and p are nil)
- p and q != nil and p > q
- if eq is TRUE also checks for equality
*/

static bit
greaterEq(ValPtr p, bit pi, ValPtr q, bit qi, bit eq)
{

    int (*cmp) (const void *, const void *);
    int tpe, c;
    const void *nilptr, *pp, *pq;

    if( p == 0 || q == 0 ) return  0;
    if( (tpe = p ->vtype) != q->vtype ) return  0;

    cmp = BATatoms[tpe].atomCmp;
    nilptr = ATOMnilptr(tpe);
    pp = VALptr(p);
    pq = VALptr(q);
    if( (*cmp)(pp, nilptr) == 0 ) return 1; /* p is nil */
    if( (*cmp)(pq, nilptr) == 0 )  return 0;
    c = (*cmp)(pp, pq);
	 if ( c > 0 ) return 1;
	 if (c == 0 ){
			if ( eq ) return (pi || neg(qi)); /* greater or eq */
			else return (pi && neg(qi));		/* strict greater */
	}
	return 0;
}

static bit
thetaselectSubsume(InstrPtr p, InstrPtr q, MalStkPtr s)
{
	ValPtr pval, qval;
	const char *pop, *qop;
	bit pi, qi;

	qval = &getVar(recycleBlk,getArg(q,2))->value;
	qop = (const char *)getVarValue(recycleBlk,getArg(q,3));

	pval = &s->stk[getArg(p,2)];
	pop = (const char *)VALptr(&s->stk[getArg(p,3)]);

	if ( pop[0] != qop[0] ) return 0;
	pi = ( pop[1] == '=' );
	qi = ( qop[1] == '=' );

	if ( qop[0] == '<' ) {
		if ( lessEq(pval,pi,qval,qi,FALSE) )
			return 1;
		else return 0;
	} else
	if ( qop[0] == '>' ) {
		if ( greaterEq(pval,pi,qval,qi,FALSE) )
			return 1;
		else return 0;
	}
	return 0;
}

static int
RECYCLEdataTransfer(Client cntxt, MalStkPtr s, InstrPtr p)
{
	int i, j, qidx, lcomp, rcomp, pc = -1;
	/*	bat bid; */
	bat sbid = -1;
	InstrPtr q;
	bit gluse = FALSE;
	BUN scnt = 0;
	BAT *b, *bn;
	/*	oid lval = 0, hval = 0;
	  dbl ratio; */
	static str octopusRef = 0, bindRef = 0, bindidxRef = 0;

	if (octopusRef == 0)
		octopusRef = putName("octopus",7);
	if (bindRef == 0)
        bindRef = putName("bind",4);
	if (bindidxRef == 0)
        bindidxRef = putName("bind_idxbat",11);

	MT_lock_set(&recycleLock, "recycle");
	for (i = 0; i < recycleBlk->stop; i++){
    	q = getInstrPtr(recycleBlk,i);
		if ( getModuleId(q) != octopusRef ||
			 (getFunctionId(q) != bindRef && getFunctionId(q) != bindidxRef) ||
			 (getFunctionId(q) != getFunctionId(p)) )
		continue;

		if (p->argc < q->argc-1) continue;
		/* sub-range instructions can be subsumed from entire table */

		for (j = p->retc + 1; j < 6; j++)
			if ( VALcmp(&s->stk[getArg(p,j)],
				&getVarConstant(recycleBlk, getArg(q,j))) )
				goto notfound;

		if ( q->argc-1 == 7 )	/* q is entire bat */
			if ( p->argc == 7 )
				goto exactmatch;
			else goto subsumption;

		else {		/* q is bat partition */

			lcomp = VALcmp(&getVar(recycleBlk,getArg(q,6))->value,
					&s->stk[getArg(p,6)]);
			rcomp = VALcmp( &s->stk[getArg(p,7)],
					&getVar(recycleBlk,getArg(q,7))->value);

			if ( lcomp == 0 && rcomp == 0 )            /* found an exact match */
				goto exactmatch;
/*			else if ( lcomp <= 0 && rcomp <= 0 )
				goto subsumption;*/
			else
				continue;
		}

		exactmatch:
            /* get the results on the stack */
            for( j=0; j<p->retc; j++){
                VALcopy(&s->stk[getArg(p,j)],
                    &getVarConstant(recycleBlk,getArg(q,j)) );
                if (s->stk[getArg(p,j)].vtype == TYPE_bat)
                    BBPincref( s->stk[getArg(p,j)].val.bval, TRUE);
            }
            recycleBlk->profiler[i].calls++;
            if ( recycleBlk->profiler[i].clk < cntxt->rcc->time0 )
                    gluse = recycleBlk->profiler[i].trace = TRUE;
            else { /*local use - return the credit */
			    returnCredit(q);
            }
            recycleBlk->profiler[i].clk = GDKusec();
            recyclePool->ptrn[cntxt->rcc->curQ]->dtreuse += recycleBlk->profiler[i].wbytes;
            qidx = *(int*)getVarValue(recycleBlk,q->argv[q->argc-1]);
            updateQryStat(q->recycle,gluse,qidx);
            cntxt->rcc->recycled0++;
            cntxt->rcc->recent = i;
            MT_lock_unset(&recycleLock, "recycle");
            return i;

    	subsumption:
			sbid = getVarConstant(recycleBlk, getArg(q,0)).val.bval;
			pc = i;
			break;

		notfound:
			continue;
	}

	if ( sbid >= 0 ) {	/* subsumption of octopus.bind */
		BUN psz;
		int part_nr = *(int *)getArgReference(s, p, 6);
		int nr_parts = *(int *)getArgReference(s, p, 7);

		b = BBPquickdesc(sbid, FALSE);
		scnt = BATcount(b);
		psz = scnt?(scnt/nr_parts):0;
		bn =  BATslice(b, part_nr*psz, (part_nr+1==nr_parts)?scnt:((part_nr+1)*psz));
		BATseqbase(bn, part_nr*psz);

		/*		lval = *(oid *)getArgReference(s, p, 6);
		hval = *(oid *)getArgReference(s, p, 7);
		bn = BATslice(b, lval, hval);
		BATseqbase(bn, lval);
		ratio = (dbl)BATcount(bn)/(dbl)scnt; */

		VALset(&s->stk[getArg(p,0)], TYPE_bat, &bn->batCacheid);
		BBPkeepref( bn->batCacheid);

		recycleBlk->profiler[pc].calls++;
		recycleBlk->profiler[pc].clk = GDKusec();
		recyclePool->ptrn[cntxt->rcc->curQ]->dtreuse +=
			(lng) scnt?(psz * recycleBlk->profiler[pc].wbytes / scnt):0;
		cntxt->rcc->recycled0++;
		cntxt->rcc->recent = i;
	}

	MT_lock_unset(&recycleLock, "recycle");
	return pc;
}


static int
RECYCLEreuse(Client cntxt, MalBlkPtr mb, MalStkPtr s, InstrPtr p, int pc)
{
    int i, j, ridx, idx, qidx, evicted=0;
    bat bid= -1, nbid= -1;
    InstrPtr q;
    static str subselectRef = 0, like_subselectRef = 0, thetasubselectRef = 0,
		octopusRef = 0, bindRef = 0, bindidxRef = 0;
    bit gluse = FALSE;
	lng ticks = GDKusec();

    if (subselectRef == 0)
        subselectRef= putName("subselect",9);
	if (like_subselectRef == 0)
        like_subselectRef= putName("like_subselect",14);
	if (thetasubselectRef == 0)
        thetasubselectRef= putName("thetasubselect",14);
    if (octopusRef == 0)
        octopusRef = putName("octopus",7);
	if (bindRef == 0)
        bindRef = putName("bind",4);
	if (bindidxRef == 0)
        bindidxRef = putName("bind_idxbat",11);

	/* separate matching of data transfer instructions: octopus.bind(), octopus.bind_idxbat() */
	if ( getModuleId(p) == octopusRef &&
		( getFunctionId(p) == bindRef || getFunctionId(p) == bindidxRef ))
		return RECYCLEdataTransfer(cntxt, s, p);

    MT_lock_set(&recycleLock, "recycle");

    for (i = 0; i < recycleBlk->stop; i++){
        q = getInstrPtr(recycleBlk,i);

        if ((getFunctionId(p) != getFunctionId(q)) ||
            (getModuleId(p) != getModuleId(q)))
            continue;

		/* 1: reuse smallest range covering */
		ridx= getArg(q,1);
		idx= getArg(p,1);
		if (q->argc-1 > 3 &&
			( getFunctionId(p) == subselectRef ||
				getFunctionId(p) == like_subselectRef ||
				getFunctionId(p) == thetasubselectRef ) &&
			getVarConstant(recycleBlk, ridx).val.bval == s->stk[idx].val.bval &&
			BATatoms[getArgType(recycleBlk,q,2)].linear )

		{	bit subsmp = 0;
				/* Time to check for the inclusion constraint */
				if ( getFunctionId(p) == subselectRef )
					subsmp = selectSubsume(p,q,s);
				else if ( getFunctionId(p) == like_subselectRef )
					subsmp = likeSubsume(p,q,s);
				else if ( getFunctionId(p) == thetasubselectRef )
					subsmp = thetaselectSubsume(p,q,s);

			if (subsmp){
				BAT *b1, *b2;
				nbid = getVarConstant(recycleBlk, getArg(q,0)).val.bval;
				if( bid == -1){
					bid = nbid;
					pc = i;

#ifdef _DEBUG_RECYCLE_
b1 = BBPquickdesc(bid, FALSE);
mnstr_printf(cntxt->fdout,"#counts A %d -> " BUNFMT " \n", bid,             BATcount(b1));
#endif
				} else {
					b1 = BBPquickdesc(bid, FALSE);
					b2 = BBPquickdesc(nbid, FALSE);

#ifdef _DEBUG_RECYCLE_
mnstr_printf(cntxt->fdout,"#counts B %d -> " BUNFMT " %d -> " BUNFMT "\n",
	bid, BATcount(b1), nbid, BATcount(b2));
#endif
					if (BATcount(b1) > BATcount(b2)){
						bid = nbid;
						pc = i;
					}
				}
#ifdef _DEBUG_RECYCLE_
mnstr_printf(cntxt->fdout,"#Inclusive range bid=%d ", bid);
printInstruction(cntxt->fdout,recycleBlk,0,q, LIST_MAL_ALL);
#endif
			}
		}
		/* 2: exact covering */
		if (p->argc > q->argc-1) continue;
		for (j = p->retc; j < p->argc; j++)
			if (VALcmp(&s->stk[getArg(p,j)], &getVarConstant(recycleBlk,    getArg(q,j))))
				goto notfound;
#ifdef _DEBUG_CACHE_
			if ( q->token == NOOPsymbol ){
					evicted = 1;
			mnstr_printf(cntxt->fdout,"#Miss of evicted instruction %d\n",  i);
					goto notfound;
			}
#endif

		/* found an exact match */
		/* get the results on the stack */
		for( j=0; j<p->retc; j++){
			VALcopy(&s->stk[getArg(p,j)],
				&getVarConstant(recycleBlk,getArg(q,j)) );
			if (s->stk[getArg(p,j)].vtype == TYPE_bat)
				BBPincref( s->stk[getArg(p,j)].val.bval , TRUE);
		}
		recycleBlk->profiler[i].calls++;
		if ( recycleBlk->profiler[i].clk < cntxt->rcc->time0 )
					gluse = recycleBlk->profiler[i].trace = TRUE;
		else { /*local use - return the credit */
			returnCredit(q);
		}
		recycleBlk->profiler[i].clk = GDKusec();
		if (!isBindInstr(q)){
			cntxt->rcc->recycled0++;
			qidx = *(int*)getVarValue(recycleBlk,q->argv[q->argc-1]);
			updateQryStat(q->recycle,gluse,qidx);
		}
		cntxt->rcc->recent = i;
		MT_lock_unset(&recycleLock, "recycle");
		return i;
		notfound:
			continue;
    }
    /*
     * We have a candidate table from which we can draw a subsection.
     * We execute it in place and safe the result upon need.
     */
    if (bid >=0) {
        int k;

        ticks = GDKusec();
        i= getPC(mb,p);
#ifdef _DEBUG_RECYCLE_REUSE
    mnstr_printf(cntxt->fdout,"#RECYCLEreuse subselect ");
    printInstruction(cntxt->fdout, recycleBlk, 0,getInstrPtr(recycleBlk,pc),    LIST_MAL_ALL);

    mnstr_printf(cntxt->fdout,">>>");
    printTraceCall(cntxt->fdout, mb, s,i, LIST_MAL_ALL);
    mnstr_printf(cntxt->fdout,"\n");
#endif
            nbid = s->stk[getArg(p,1)].val.bval;
        s->stk[getArg(p,1)].val.bval = bid;
        BBPincref(bid, TRUE);
        /* make sure the garbage collector is not called */
        j = s->keepAlive ;
        s->keepAlive = TRUE;
        k = p->recycle;
        p->recycle = NO_RECYCLING; /* No recycling for instructions with        subsumption */
        (void) reenterMAL(cntxt,mb,i,i+1,s);
        /* restore the situation */
/*        ticks = GDKusec() - ticks; */
        p->recycle= k;
        s->keepAlive= j;
        s->stk[getArg(p,1)].val.bval = nbid;
        BBPdecref(bid, TRUE);
        cntxt->rcc->recycled0++;
        recycleBlk->profiler[pc].calls++;
        if ( recycleBlk->profiler[pc].clk < cntxt->rcc->time0 )
			gluse = recycleBlk->profiler[pc].trace = TRUE;
        qidx = *(int*)getVarValue(recycleBlk,q->argv[q->argc-1]);
        updateQryStat(getInstrPtr(recycleBlk,pc)->recycle,gluse,qidx);
        recycleBlk->profiler[pc].clk = GDKusec();
        MT_lock_unset(&recycleLock, "recycle");
        RECYCLEexit(cntxt, mb, s, p, pc, ticks);
        return pc;
    }

#ifdef _DEBUG_CACHE_
    if ( evicted ) cntxt->rcc->recycleMiss++;
#else
    (void) evicted;
#endif

    MT_lock_unset(&recycleLock, "recycle");
	if ( pc >= 0 ) 		/* successful multi-subsumption */
		RECYCLEexit(cntxt,mb,s,p, pc,ticks);
    return pc;
}



int
RECYCLEentry(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p, int pc)
{
	int i=0;

	(void) pc;
/*	stk->clk= GDKusec(); timing moved to interpreter */
	if ( p->recycle == NO_RECYCLING )
        	return 0;       /* don't count subsumption instructions */
	cntxt->rcc->statements++;
	if ( recycleBlk == NULL )
		return 0;
	if ( !RECYCLEinterest(p) )  /* don't scan RecyclerPool for non-monitored instructions */
		return 0;
	if ( cntxt->rcc->curQ < 0 )	/* don't use recycling before initialization
				by prelude() */
		return 0;
	i = RECYCLEreuse(cntxt,mb,stk,p,pc) >= 0;
#ifdef _DEBUG_RECYCLE_
        mnstr_printf(cntxt->fdout,"#Reuse %d for ",i);
        printInstruction(cntxt->fdout,mb,0,p, LIST_MAL_ALL);
        mnstr_printf(cntxt->fdout,"\n");
#endif
	return i;
}

/*
 * The 'exit' instruction is called after the interpreter loop
 * itself and has to decide on the results obtained.
 * This is the place where we should call recycle optimization routines( admission policies).
 * It can use the timing information gathered from the previous call,
 * which is stored in the stack frame to avoid concurrency problems.
 */
void
RECYCLEexitImpl(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p, int pc, lng ticks){
	lng memLimit;
	lng cacheLimit;
	lng rd = getVolume(stk,p, 1)/ RU +1;
	lng wr = getVolume(stk,p, 0)/ RU +1;
	ValRecord *v;
	static str octopusRef = 0, bindRef = 0;

	memLimit = recycleMemory?recycleMemory:HARDLIMIT_MEM;
	cacheLimit = recycleCacheLimit?recycleCacheLimit:HARDLIMIT_STMT;

	v = &stk->stk[getArg(p,0)]; /* don't count memory for persistent bats */
	if ((v->vtype == TYPE_bat) && (BBP_status( *(const int*)VALptr(v)) & BBPPERSISTENT))
		wr = 0;

    if (octopusRef == 0)
        octopusRef = putName("octopus",7);
	if (bindRef == 0)
        bindRef = putName("bind",4);

	/* track data transfer */
	if ( getModuleId(p) == octopusRef && getFunctionId(p) == bindRef ) {
		cntxt->rcc->trans++;
		cntxt->rcc->transKB += wr;
	}

	if ( wr > memLimit)
		return;
	if (recycleBlk){
		if ( recyclerMemoryUsed +  wr > memLimit ||
	    		recycleSize >= cacheLimit )
			RECYCLEcleanCache(cntxt, wr);
	}

	/* ensure the right mal block is pointed in the context */
	if ( cntxt->rcc->curQ < 0 ||
		(cntxt->rcc->curQ >=0 && recyclePool->ptrn[cntxt->rcc->curQ]->recid != mb->recid ) )
		cntxt->rcc->curQ = findQryStat(mb->recid);

	if ( cntxt->rcc->curQ < 0 ) {
		mnstr_printf(cntxt->fdout,"#The query pattern should exist before adding its instruction to the cache\n");
		return;
	}

	if ( RECYCLEinterest(p)){
		/* ADM_ALL: infinite case, admit all new instructions */
		if (RECYCLEfind(cntxt,mb,stk,p)<0 )
			(void) RECYCLEkeep(cntxt,mb, stk, p, pc, rd, wr, ticks);
	}
}

void
RECYCLEexit(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p, int pc, lng clk0)
{
	if ( cntxt->rcc->curQ < 0 ) /* don't use recycling before initialization
				by prelude() */
		return;
	MT_lock_set(&recycleLock, "recycle");
	RECYCLEexitImpl(cntxt,mb,stk,p, pc, GDKusec()-clk0);
	MT_lock_unset(&recycleLock, "recycle");
}

/*
 * At the end of session we should remove all
 * knowledge from the recycle cache.
 */
void
RECYCLEshutdown(Client cntxt){
	MalBlkPtr mb = recycleBlk;
	int i;
	bte *used;
	Client c;

	if( recycleBlk == NULL)
		return ;

#ifdef _DEBUG_RECYCLE_
	mnstr_printf(cntxt->fdout,"#RECYCLE shutdown\n");
	printFunction(cntxt->fdout, recycleBlk,0,0);
	printStack(cntxt->fdout,mb,0);
#else
	(void) cntxt;
#endif

	used = (bte*)GDKzalloc(recycleBlk->vtop);
	MT_lock_set(&recycleLock, "recycle");
	recycleBlk = NULL;
	recycleSearchTime = 0;
	recyclerMemoryUsed = 0;
	for(c = mal_clients; c < mal_clients+MAL_MAXCLIENTS; c++)
		if (c->mode != FREECLIENT) {
			memset((char *)c->rcc, 0, sizeof(RecStat));
			c->rcc->curQ = -1;
    }
	emptyRecyclePool(recyclePool);
	recyclePool = NULL;
	MT_lock_unset(&recycleLock, "recycle");
	for (i=mb->stop-1; i>=0; i--)
		RECYCLEgarbagecollect(mb, getInstrPtr(mb,i),used);
	freeMalBlk(mb);
	GDKfree(used);
}

/*
 * Evict a bat from recycle cache, for instance if an update on it
 * has been detected.
 */
static void
RECYCLEevict(Client cntxt, bat *bats, int btop){
	int i,j,k,top = 0, rbid;
	int action = 1, limit;
	int *dropped;
	InstrPtr p;
	InstrPtr *old, *newstmt;
	bte *used;
	bte *dmask;

	if( recycleBlk == NULL)
		return;

#ifdef _DEBUG_RECYCLE_
	mnstr_printf(cntxt->fdout,"#RECYCLE evict\n");
	printFunction(cntxt->fdout, recycleBlk, 0,0);
#else
	(void) cntxt;
#endif

	dropped = (int *) GDKzalloc(sizeof(int)*recycleBlk->vtop);
	used = (bte*)GDKzalloc(recycleBlk->vtop);
    dmask = (bte *)GDKzalloc(recycleBlk->stop);

	for( i=0; i<btop; i++)
		dropped[top++] = bats[i];
	for (i = 0; i < recycleBlk->stop; i++){
		p = recycleBlk->stmt[i];
#ifdef _DEBUG_CACHE_
                if ( p->token != NOOPsymbol )
#endif
		for( j = 0 ; j< p->argc; j++)
			if (used[getArg(p,j)]<2)  used[getArg(p,j)]++;
	}
	action= 0;
	for (i=0; i<recycleBlk->stop; i++){
		p = getInstrPtr(recycleBlk,i);
#ifdef _DEBUG_CACHE_
        if ( p->token == NOOPsymbol ) continue;
#endif
		for (j=0; j<p->argc; j++)
			if(getArgType(recycleBlk,p,j)==TYPE_bat ||
				isaBatType(getArgType(recycleBlk, p,j)) ){
				int nbid = getVarConstant(recycleBlk, getArg(p,j)).val.bval;
				if (nbid == 0) continue;
				for (k=0; k<top; k++)
					if (dropped[k]== nbid)
						break;
				if (k < top) break;
		}
		if ( j < p->argc ){  /* instruction argument or result is updated bat */

			if ( j >= p->retc ){ 			/* if some argument bat is updated */
				for (j=0;j<p->retc; j++)	/* mark result bats as dropped */
				if(getArgType(recycleBlk,p,j)==TYPE_bat ||
					isaBatType(getArgType(recycleBlk, p,j)) ){
						rbid = getVarConstant(recycleBlk, getArg(p,j)).val.bval;
						for (k=0; k<top; k++)
							if ( dropped[k] == rbid ) break;
						if ( k == top )
							dropped[top++]= rbid;
				}
			}
            dmask[i] = 1;
            action++;
        }
    }   /* for i */

        /* delete all marked instructions in 1 pass */
    old = recycleBlk->stmt;
    limit = recycleBlk->stop;
    newstmt = (InstrPtr *) GDKzalloc(sizeof(InstrPtr) * recycleBlk->ssize);
    if (newstmt == NULL){
        RECYCLEshutdown(cntxt);
        goto cln;
    }
    recycleBlk->stmt = newstmt;
    recycleBlk->stop = 0;

    k = 0;
    for (i = 0; i < limit ; i++){
        p = old[i];
        if( dmask[i] ) {
           RECYCLEgarbagecollect(recycleBlk,p,used);
           recyclerMemoryUsed -= recycleBlk->profiler[i].wbytes;
           if ( recycleBlk->profiler[i].calls >1)
               returnCredit(p);
           freeInstruction(p);
           cntxt->rcc->RPreset0++;
        }
        else {
           pushInstruction(recycleBlk,p);
           recycleBlk->profiler[k++]= recycleBlk->profiler[i];
        }
    }

#ifdef _DEBUG_CACHE_
		        /* instructions are marked with NOOPsymbol in debug mode
                        	recyclerMemoryUsed -= recycleBlk->profiler[i].wbytes;
	                        p->token = NOOPsymbol;
				cntxt->rcc->recycleRem ++;
				if ( recycleBlk->profiler[i].calls >1) {
		                	returnCredit(p);
				}
*/
#endif

    /*
     * we assume that the variables defined are only used later on in the recycle
     * cache. This can be enforced by never re-sorting it. Under this condition
     * we only have to make one pass.
     */
    cln:
	GDKfree(dropped);
	GDKfree(used);
    GDKfree(dmask);
	if (action){
#ifndef _DEBUG_CACHE_
		trimMalVariables(recycleBlk);
#endif
	}
}

/*
 * Once we encounter an update we check and clean the recycle cache from
 * instructions dependent on the updated bat.
 */
str RECYCLEreset(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p, int pc)
{
	int i,j, btop=0, k;
	int *b;
	ValRecord *v;
	InstrPtr q;
	static str sqlRef = 0, bindRef = 0, bind_idxRef = 0, bind_dbatRef = 0;
	int bid;
	BAT *bref = NULL;
	lng t0 = GDKusec();
	(void) pc;

	if (sqlRef == 0)
		sqlRef = putName("sql",3);
	if (bindRef == 0)
		bindRef = putName("bind",4);
	if (bind_idxRef == 0)
		bind_idxRef = putName("bind_idxbat",11);
	if (bind_dbatRef == 0)
		bind_dbatRef = putName("bind_dbat",9);

#ifdef _DEBUG_RESET_
	mnstr_printf(cntxt->fdout,"#RECYCLE reset\n");
#else
	(void) cntxt;
	(void) bref;
#endif
	if( recycleBlk == NULL)
		return MAL_SUCCEED;
	b = (int *)GDKzalloc(sizeof(int)*recycleBlk->stop);

	if (p->argc == 2){
		if(getArgType(mb,p,1)==TYPE_bat ||
			isaBatType(getArgType(mb, p,1)) ){
 			v = &stk->stk[getArg(p,1)];
			if(v->vtype == TYPE_bat && v->val.bval )
				b[btop++] = v->val.bval;
		}

	} else if (p->argc > 2) {
		for (i=0; i<recycleBlk->stop; i++) {
			q = getInstrPtr(recycleBlk,i);

#ifdef _DEBUG_CACHE_
                        if ( q->token == NOOPsymbol ) continue;
#endif

			if ( getModuleId(q) != sqlRef ) continue;
			if ( isBindInstr(q) ){
#ifdef _DEBUG_RESET_
				bid = getVarConstant(recycleBlk, getArg(q,0)).val.bval;
				if ( (bref = BBPquickdesc(bid, FALSE)) )
					mnstr_printf(cntxt->fdout,"#Bid %d, count "BUNFMT"\n", bid, BATcount(bref));
				else mnstr_printf(cntxt->fdout,"#Bid %d, NULL bat ref\n", bid);
#endif
				if ( ((getFunctionId(q) == bindRef ||
					getFunctionId(q) == bind_idxRef ) &&
					( getVarConstant(recycleBlk, getArg(q,5)).val.ival <=
						getVarConstant(mb, getArg(p,1)).val.ival )) ||
					( getFunctionId(q) == bind_dbatRef ) ){
					for (j=p->retc+1; j<p->argc; j++)
						if( VALcmp( &stk->stk[getArg(p,j)], &getVarConstant(recycleBlk,getArg(q,j))))
						break;
					if (j == p->argc){
						v = &getVarConstant(recycleBlk,getArg(q,0));
						bid = v->val.bval;
						if( bid ){
							for (k=0; k<btop; k++)
								if ( b[k] == bid ) break;
							if ( k == btop ) {
								b[btop++] = bid;
#ifdef _DEBUG_RESET_
							mnstr_printf(cntxt->fdout,"#Marked for eviction due to update\n ");
							printInstruction(cntxt->fdout,recycleBlk,0,q, LIST_MAL_ALL);
							mnstr_printf(cntxt->fdout," Bid %d\n ", b[btop-1]);
#endif
							}
						}
					}
				}
			}
		} /* loop on recycleBlk */
	}
	if (btop){
		MT_lock_set(&recycleLock, "recycle");
	        RECYCLEevict(cntxt,b,btop);
		MT_lock_unset(&recycleLock, "recycle");
	}
	GDKfree(b);
	recycleTime = GDKusec() - t0;
	return MAL_SUCCEED;
}

str
RECYCLEstart(Client cntxt, MalBlkPtr mb)
{
    cntxt->rcc->recent = -1;
    cntxt->rcc->recycled0 = 0;
    cntxt->rcc->time0 = GDKusec();
    cntxt->rcc->curQ = RECYCLEnewQryStat(mb);
    msFindTime = 0;         /* multi-subsume measurements */
    msComputeTime = 0;
    recycleTime = 0;
    cntxt->rcc->trans = cntxt->rcc->recTrans = 0;
    cntxt->rcc->transKB = cntxt->rcc->recTransKB =0;
    cntxt->rcc->RPadded0 = 0;
    cntxt->rcc->RPreset0 = 0;
	return MAL_SUCCEED;
}

str
RECYCLEstop(Client cntxt, MalBlkPtr mb)
{
    cntxt->rcc->curQ = -1;
    cntxt->rcc->recycled += cntxt->rcc->recycled0;
    if ( monitorRecycler )
        return RECYCLErunningStat(cntxt,mb);
	return MAL_SUCCEED;
}

void
RECYCLEdump(stream *s)
{
    int i, incache;
    str msg= MAL_SUCCEED;
    lng sz, persmem=0;
    ValPtr v;
    Client c;
    lng statements=0, recycled=0, recycleMiss=0, recycleRem=0;
    lng ccCalls=0, ccInstr=0;

    if (!recycleBlk) return;

    mnstr_printf(s,"#Recycler  catalog\n");
    mnstr_printf(s,"#admission ADM_ALL time ="LLFMT"\n", recycleTime);
    mnstr_printf(s,"#rcache= PROFIT limit= %d memlimit="LLFMT"\n", recycleCacheLimit, recycleMemory);
    mnstr_printf(s,"#hard stmt = %d hard var = %d hard mem="LLFMT"\n",
                 HARDLIMIT_STMT, HARDLIMIT_VAR, HARDLIMIT_MEM);

    for(i=0; i< recycleBlk->stop; i++){
#ifdef _DEBUG_CACHE_
                if ( getInstrPtr(recycleBlk,i)->token == NOOPsymbol ) continue;
#endif
        v = &getVarConstant(recycleBlk,getArg(recycleBlk->stmt[i],0));
        if ((v->vtype == TYPE_bat) &&
             (BBP_status( *(const int*)VALptr(v)) & BBPPERSISTENT)) {
			assert(0);
            //msg = BKCbatsize(&sz, (int*)VALget(v));
            if ( msg == MAL_SUCCEED )
                persmem += sz;
        }
    }
    persmem = (lng) persmem/RU;

    for(c = mal_clients; c < mal_clients+MAL_MAXCLIENTS; c++)
        if (c->mode != FREECLIENT) {
            recycled += c->rcc->recycled;
            statements += c->rcc->statements;
            recycleMiss += c->rcc->recycleMiss;
            recycleRem += c->rcc->recycleRem;
            ccCalls += c->rcc->ccCalls;
            ccInstr += c->rcc->ccInstr;
        };

    incache = recycleBlk->stop;
    mnstr_printf(s,"#recycled = "LLFMT" incache= %d executed = "LLFMT" memory(KB)= "LLFMT" PersBat memory="LLFMT"\n",
         recycled, incache,statements, recyclerMemoryUsed, persmem);
#ifdef _DEBUG_CACHE_
    mnstr_printf(s,"#RPremoved = "LLFMT" RPactive= "LLFMT" RPmisses = "LLFMT"\n",
                 recycleRem, incache-recycleRem, recycleMiss);
#endif
    mnstr_printf(s,"#Cache search time= "LLFMT"(usec) cleanCache: "LLFMT" calls evicted "LLFMT" instructions\n",recycleSearchTime, ccCalls,ccInstr);

    /* and dump the statistics per instruction*/
        mnstr_printf(s,"# CL\t   lru\t\tcnt\t ticks\t rd\t wr\t Instr\n");
    for(i=0; i< recycleBlk->stop; i++){
        if (getInstrPtr(recycleBlk,i)->token == NOOPsymbol)
            mnstr_printf(s,"#NOOP ");
        else mnstr_printf(s,"#     ");
        mnstr_printf(s,"%4d\t"LLFMT"\t%d\t"LLFMT"\t"LLFMT"\t"LLFMT"\t%s\n", i,
            recycleBlk->profiler[i].clk,
            recycleBlk->profiler[i].calls,
            recycleBlk->profiler[i].ticks,
            recycleBlk->profiler[i].rbytes,
            recycleBlk->profiler[i].wbytes,
            instruction2str(recycleBlk,0,getInstrPtr(recycleBlk,i),TRUE));
    }

}

void
RECYCLEdumpRecyclerPool(stream *s)
{
    int i;
    QryStatPtr qs;

    if (!recyclePool) {
        mnstr_printf(s,"#No query patterns\n");
        return;
    }

    mnstr_printf(s,"#Query patterns %d\n",  recyclePool->cnt);
    mnstr_printf(s,"#RecID\tcalls\tglobRec\tlocRec\tCreditWL\n");
    for(i=0; i< recyclePool->cnt; i++){
        qs = recyclePool->ptrn[i];
        mnstr_printf(s,"# "LLFMT"\t%2d\t%2d\t%2d\t%2d\n",
            qs->recid, qs->calls, qs->greuse, qs->lreuse, qs->wl);
    }
}

void
RECYCLEdumpDataTrans(stream *s)
{
    int i, n;
    lng dt, sum = 0, rdt, rsum = 0;

    if (!recycleBlk || !recyclePool)
        return;

    n = recyclePool->cnt;

    mnstr_printf(s,"#Query  \t Data   \t DT Reused\n");
    mnstr_printf(s,"#pattern\t transf.\t from others\n");
    for( i=0; i < n; i++){
        rdt = recyclePool->ptrn[i]->dtreuse;
        dt = recyclePool->ptrn[i]->dt;
        mnstr_printf(s," %d \t\t "LLFMT"\t\t"LLFMT"\n", i, dt, rdt);
        sum += dt;
        rsum += rdt;
    }
    mnstr_printf(s,"#########\n# Total transfer "LLFMT" Total reused "LLFMT"\n", sum, rsum);
}

str
RECYCLErunningStat(Client cntxt, MalBlkPtr mb)
{
    static int q=0;
    InstrPtr p;
    int potrec=0, nonbind=0, i, trans=0;
    lng reusedmem=0;

    for(i=0; i< mb->stop; i++){
        p = mb->stmt[i];
        if ( RECYCLEinterest(p) ){
            potrec++;
            if ( !isBindInstr(p) ) nonbind++;
            else if ( getModuleId(p) == putName("octopus",7) ) trans++;
        }
    }

    for(i=0; i < recycleBlk->stop; i++)
#ifdef _DEBUG_CACHE_
        if ( getInstrPtr(recycleBlk,i)->token != NOOPsymbol )
#endif
        if ( recycleBlk->profiler[i].calls >1)
            reusedmem += recycleBlk->profiler[i].wbytes;

    mnstr_printf(cntxt->fdout,"%d\t %7.2f\t ", ++q, (GDKusec()-cntxt->rcc->time0)/1000.0);
    if ( monitorRecycler & 2) { /* Current query stat */
        mnstr_printf(cntxt->fdout,"%3d\t %3d\t %3d\t ", mb->stop, potrec, nonbind);
        mnstr_printf(cntxt->fdout,"%3d\t %3d\t ", cntxt->rcc->recycled0, cntxt->rcc->recycled);
        mnstr_printf(cntxt->fdout,"|| %3d\t %3d\t ", cntxt->rcc->RPadded0, cntxt->rcc->RPreset0);
        mnstr_printf(cntxt->fdout,"%3d\t%5.2f\t"LLFMT"\t"LLFMT"\t", recycleBlk?recycleBlk->stop:0, recycleTime/1000.0,recyclerMemoryUsed,reusedmem);
    }

    if ( monitorRecycler & 1) { /* RecyclerPool stat */
        mnstr_printf(cntxt->fdout,"| %4d\t %4d\t ",cntxt->rcc->statements,recycleBlk?recycleBlk->stop:0);
        mnstr_printf(cntxt->fdout, LLFMT "\t" LLFMT "\t ", recyclerMemoryUsed, reusedmem);
#ifdef _DEBUG_CACHE_
        mnstr_printf(cntxt->fdout,"%d\t %d\t ",cntxt->rcc->recycleRem,cntxt->rcc->recycleMiss);
#endif
    }

    if ( monitorRecycler & 4) { /* Data transfer stat */
        mnstr_printf(cntxt->fdout,"| %2d\t "LLFMT"\t ",cntxt->rcc->trans, cntxt->rcc->transKB);
        mnstr_printf(cntxt->fdout,"%2d\t "LLFMT"\t ",cntxt->rcc->recTrans, cntxt->rcc->recTransKB);
    }

    mnstr_printf(cntxt->fdout,"\n");
    return MAL_SUCCEED;
}

#include "postgres.h"

#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/hash.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/scanner.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "tcop/utility.h"

#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/elog.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "access/htup_details.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"

#define PG_CATALOG "pg_catalog"
#define PG_CLASS "pg_class"
#define COL_RELKIND "relkind"

PG_MODULE_MAGIC;

/* Saved hook values in case of unload */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

static void fte_post_parse_analyse(ParseState *pstate, Query *query);
static void rewrite_query(Query *query);
static bool walker(Node *node, List *relkinds);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * Install hooks.
	 */
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = fte_post_parse_analyse;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks. */
	post_parse_analyze_hook = prev_post_parse_analyze_hook;
}

/*
 * Post-parse-analysis hook: mark query with a queryId
 */
static void
fte_post_parse_analyse(ParseState *pstate, Query *query)
{
	if (prev_post_parse_analyze_hook)
    {
		prev_post_parse_analyze_hook(pstate, query);
    }

	/*
	 * Utility statements get queryId zero.  We do this even in cases where
	 * the statement contains an optimizable statement for which a queryId
	 * could be derived (such as EXPLAIN or DECLARE CURSOR).  For such cases,
	 * runtime control will first go through ProcessUtility and then the
	 * executor, and we don't want the executor hooks to do anything, since we
	 * are already measuring the statement's costs at the utility level.
	 */
	if (query->utilityStmt)
	{
		return;
	}

    rewrite_query(query);
}

typedef struct PgClassRelKindPos
{
    Index       varno;
    AttrNumber  varattno;
} PgClassRelKindPos;

static void
rewrite_query(Query *query)
{
    int varno_index;
    ListCell   *lc_rtable;
    List *relkinds = NIL;

    if (query->type != T_Query || query->commandType != CMD_SELECT || query->querySource != QSRC_ORIGINAL)
    {
        return;
    }

    varno_index = 1;
    foreach(lc_rtable, query->rtable)
    {
        bool is_pg_class = false;
        Relation rd;
        RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc_rtable);

        rd = RelationIdGetRelation(rte->relid);

        if (rd != NULL && rd->rd_rel != NULL)
        {
            HeapTuple tp = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(rd->rd_rel->relnamespace));
            if (HeapTupleIsValid(tp))
            {
                Form_pg_namespace nsptup = (Form_pg_namespace) GETSTRUCT(tp);
                if (nsptup != NULL)
                {
                    if (strcmp(NameStr(nsptup->nspname), PG_CATALOG) == 0 &&
                        strcmp(NameStr(rd->rd_rel->relname), PG_CLASS) == 0)
                    {
                        ereport(DEBUG1, (errmsg("Found pg_catalog.pg_class")));
                        is_pg_class = true;
                    }
                }
                ReleaseSysCache(tp);
            }
            RelationClose(rd);
        }

        if (is_pg_class && rte->eref != NULL) {
            int varattrno_index = 1;
            ListCell *lc_colname;
            foreach(lc_colname, rte->eref->colnames)
            {
                char *colname = strVal(lfirst(lc_colname));
                if (strcmp(colname, COL_RELKIND) == 0)
                {
                    ereport(DEBUG1, (errmsg("Found pg_catalog.pg_class.relkind: [varno:%d, varattno:%d]", varno_index, varattrno_index)));
                    PgClassRelKindPos *relkindPos = (PgClassRelKindPos*) palloc(sizeof(PgClassRelKindPos));
                    relkindPos->varno = varno_index;
                    relkindPos->varattno = varattrno_index;
                    relkinds = lappend(relkinds, relkindPos);
                }
                varattrno_index++;
            }
        }

        /* TODO: Take care of subquery properly */
        if (rte->subquery != NULL)
        {
            rewrite_query(rte->subquery);
        }

        varno_index++;
    }

    if (relkinds != NIL && relkinds->length > 0 && query->jointree != NULL)
    {
        FromExpr *fromExpr = query->jointree;
        expression_tree_walker(fromExpr->quals, walker, relkinds);
    }
}

static
bool walker(Node *node, List *relkinds)
{
     if (node == NULL)
     {
         return false;
     }

     if (IsA(node, OpExpr))
     {
         /* TODO */
     }
     else if (IsA(node, ScalarArrayOpExpr)) 
     {
         ScalarArrayOpExpr *arrayOpExpr = (ScalarArrayOpExpr*) node;
         if (arrayOpExpr->args->length == 2 && IsA(list_nth(arrayOpExpr->args, 0), Var) && IsA(list_nth(arrayOpExpr->args, 1), ArrayExpr))
         {
             ListCell *lc_relkind;
             Var *var = (Var*) list_nth(arrayOpExpr->args, 0);
             ArrayExpr *array = (ArrayExpr*) list_nth(arrayOpExpr->args, 1);
             foreach(lc_relkind, relkinds)
             {
                 PgClassRelKindPos *relkindPos = lfirst(lc_relkind);
                 if (var->varno == relkindPos->varno && var->varattno == relkindPos->varattno)
                 {
                     bool regularTableExists = false;
                     bool foreignTableExists = false;
                     ListCell *lc_elm;
                     foreach(lc_elm, array->elements)
                     {
                         Node *elm = (Node*)lfirst(lc_elm);
                         if (IsA(elm, Const))
                         {
                            Const *c = (Const*)elm;
                            if (c->constbyval && c->constlen == 1 && c->consttype == CHAROID)
                            {
                                switch (DatumGetChar(c->constvalue))
                                {
                                    case RELKIND_RELATION:
                                        regularTableExists = true;
                                        break;
                                    case RELKIND_FOREIGN_TABLE:
                                        foreignTableExists = true;
                                        break;
                                    default:
                                        break;
                                }
                            }
                         }
                     }
                     ereport(DEBUG1, (errmsg("regularTableExists=%d, foreignTableExists=%d", regularTableExists, foreignTableExists)));
                     if (regularTableExists && !foreignTableExists)
                     {
                         array->elements =
                             lappend(array->elements,
                                     makeConst(CHAROID, -1, InvalidOid, 1, CharGetDatum(RELKIND_FOREIGN_TABLE), false, true));
                     }
                 }
             }
         }
         else
         {
            ereport(WARNING, (errmsg("arrayOpExpr->args has unexpected values: %s", nodeToString(arrayOpExpr->args))));
         }
     }
     return expression_tree_walker(node, walker, (void *) relkinds);
}


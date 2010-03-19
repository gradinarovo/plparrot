/* Parrot header files */
#include "parrot/embed.h"
#include "parrot/extend.h"
#include "parrot/imcc.h"
#include "include/embed_string.h"

/* Postgres header files */
#include "postgres.h"
#include "access/heapam.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/**********************************************************************
 * The information we cache about loaded procedures
 **********************************************************************/
typedef struct plparrot_proc_desc
{
    char    *proname;       /* user name of procedure */
    TransactionId fn_xmin;
    ItemPointerData fn_tid;
    bool    fn_readonly;
    bool    lanpltrusted;
    bool    fn_retistuple;  /* true, if function returns tuple */
    bool    fn_retisset;    /* true, if function returns set */
    bool    fn_retisarray;  /* true if function returns array */
    Oid     result_oid;     /* Oid of result type */
    FmgrInfo    result_in_func; /* I/O function and arg for result type */
    Oid         result_typioparam;
    int         nargs;
    FmgrInfo    arg_out_func[FUNC_MAX_ARGS];
    bool        arg_is_rowtype[FUNC_MAX_ARGS];
   /* SV          *reference; */
} plparrot_proc_desc;

/*
 * The information we cache for the duration of a single call to a
 * function.
 */
typedef struct plparrot_call_data
{
    plparrot_proc_desc *prodesc;
    FunctionCallInfo fcinfo;
    Tuplestorestate *tuple_store;
    TupleDesc   ret_tdesc;
    AttInMetadata *attinmeta;
    MemoryContext tmp_cxt;
} plparrot_call_data;

Parrot_Interp interp;

void plparrot_elog(int level, char *message);

/* this is saved and restored by plparrot_call_handler */
static plparrot_call_data *current_call_data = NULL;

/* Be sure we do initialization only once */
static bool inited = false;

PG_FUNCTION_INFO_V1(plparrot_call_handler);
Datum plparrot_call_handler(PG_FUNCTION_ARGS);
void _PG_init(void);
void _PG_fini(void);

void
_PG_init(void)
{
    if (inited)
        return;

    interp = Parrot_new(NULL);
    imcc_initialize(interp);

    if (!interp) {
        elog(ERROR,"Could not create a Parrot interpreter!\n");
        return;
    }

    inited = true;
}

/*
 *  Per PostgreSQL 9.0 documentation, _PG_fini only gets called when a module
 *  is un-loaded, which isn't yet supported. But I'm putting this here for good
 *  measure, anyway
 */
void
_PG_fini(void)
{
    Parrot_destroy(interp);
    inited = false;
}

/*
 * The PostgreSQL function+trigger managers call this function for execution of
 * PL/Parrot procedures.
 */

Datum
plparrot_call_handler(PG_FUNCTION_ARGS)
{
    Datum retval, procsrc_datum;
    Form_pg_proc procstruct;
    HeapTuple proctup;
    Oid returntype, *argtypes;
    int numargs, rc;
    char **argnames, *argmodes;
    plparrot_call_data *save_call_data = current_call_data;
    char *proc_src, *errmsg, *tmp;
    bool isnull;
    Parrot_PMC func_pmc;
    Parrot_String err;

    if ((rc = SPI_connect()) != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(rc));

    elog(NOTICE,"enter plparrot_call_handler");
    proctup = SearchSysCache(PROCOID, ObjectIdGetDatum(fcinfo->flinfo->fn_oid), 0, 0, 0);
    if (!HeapTupleIsValid(proctup))
        elog(ERROR, "Failed to look up procedure with OID %u", fcinfo->flinfo->fn_oid);
    procstruct = (Form_pg_proc) GETSTRUCT(proctup);
    returntype = procstruct->prorettype;
    procsrc_datum = SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_prosrc, &isnull);
    numargs = get_func_arg_info(proctup, &argtypes, &argnames, &argmodes);
    if (isnull)
        elog(ERROR, "Couldn't load function source for function with OID %u", fcinfo->flinfo->fn_oid);
#ifdef TextDatumGetCString
    proc_src = pstrdup(TextDatumGetCString(procsrc_datum));
#else
    /* For PostgreSQL versions 8.3 and prior */
    proc_src = pstrdup(DatumGetCString(DirectFunctionCall1(textout, procsrc_datum)));
#endif

    /* procstruct probably isn't valid after this ReleaseSysCache call, so don't use it anymore */
    ReleaseSysCache(proctup);

    // This makes the test_void test pass, but it is cheating, since it never compiles/runs the PIR
    /*
    if (returntype == VOIDOID)
        PG_RETURN_VOID();
    */
    // Why is this ever the right thing to do?
    /*
    if (fcinfo->nargs == 0)
        PG_RETURN_NULL();
    */

    /* Assume from here on out that the first argument type is the same as the return type */
    retval = PG_GETARG_DATUM(0);

    elog(NOTICE,"entering PG_TRY");
    PG_TRY();
    {
        if (CALLED_AS_TRIGGER(fcinfo)) {
                TriggerData *tdata = (TriggerData *) fcinfo->context;
                /* we need a trigger handler */
        } else {
            elog(NOTICE,"about to compile a PIR string: %s", proc_src);
            func_pmc = Parrot_compile_string(interp, Parrot_new_string(interp, "PIR", 3, (const char *) NULL, 0), proc_src, &err);
            elog(NOTICE,"compiled a PIR string");
            if (!STRING_is_null(interp, err)) {
                elog(NOTICE,"got an error compiling PIR string");
                tmp = Parrot_str_to_cstring(interp, err);
                errmsg = pstrdup(tmp);
                elog(NOTICE,"about to free parrot cstring");
                Parrot_str_free_cstring(tmp);
                elog(ERROR, "Error compiling PIR function");
            }
            /* See Parrot's src/extend.c for interpretations of the third argument */
            elog(NOTICE,"about to call compiled PIR string with Parrot_ext_call");
            Parrot_ext_call(interp, func_pmc, "->");
        }
    }
    PG_CATCH();
    {
        current_call_data = save_call_data;
        PG_RE_THROW();
    }
    PG_END_TRY();

    current_call_data = save_call_data;

    if ((rc = SPI_finish()) != SPI_OK_FINISH)
        elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));

    return retval;
}

void
plparrot_elog(int level, char *message)
{
    elog(level, "%s", message);
}

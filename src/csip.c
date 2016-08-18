#include "csip.h"
#include "scip/scip.h"
#include "scip/pub_misc.h"
#include "scip/pub_var.h"
#include "scip/scipdefplugins.h"

// map return codes: SCIP -> CSIP
static inline int retCodeSCIPtoCSIP(int scipRetCode)
{
    switch (scipRetCode)
    {
    case SCIP_OKAY:
        return CSIP_RETCODE_OK;
    case SCIP_NOMEMORY:
        return CSIP_RETCODE_NOMEMORY;
    default: // all the same for us
        return CSIP_RETCODE_ERROR;
    }
}

// map return codes: CSIP -> SCIP
static inline int retCodeCSIPtoSCIP(int csipRetCode)
{
    switch (csipRetCode)
    {
    case CSIP_RETCODE_OK:
        return SCIP_OKAY;
    case CSIP_RETCODE_NOMEMORY:
        return SCIP_NOMEMORY;
    default: // CSIP_RETCODE_ERROR
        return SCIP_ERROR;
    }
}


// catch return code within CSIP, like SCIP_CALL defined in SCIP
#define CSIP_CALL(x)                                                       \
    do {                                                                   \
        CSIP_RETCODE _retcode = (x);                                       \
        if(_retcode != CSIP_RETCODE_OK)                                    \
        {                                                                  \
            printf("Failing with retcode %d at %d\n", _retcode, __LINE__); \
            return _retcode;                                               \
        }                                                                  \
    } while(0)

// catch SCIP return code from CSIP
#define SCIP_in_CSIP(x) CSIP_CALL( retCodeSCIPtoCSIP(x) )

// catch CSIP return code from SCIP
#define CSIP_in_SCIP(x) SCIP_CALL( retCodeCSIPtoSCIP(x) )

// variable sized arrays
#define INITIALSIZE 64
#define GROWFACTOR   2

struct csip_model
{
    SCIP *scip;

    // variable sized array for variables
    int nvars;
    int varssize;
    SCIP_VAR **vars;

    // variable sized array for constraints
    int nconss;
    int consssize;
    SCIP_CONS **conss;

    // counter for callbacks
    int nlazycb;
    int nheur;

    // store the user-defined optimization sense, because SCIP always minimizes
    SCIP_OBJSENSE sense;

    // user-defined solution, is checked before solving
    SCIP_SOL *initialsol;

    // store the optimization status (for after freeTransform)
    CSIP_STATUS status;

    // store the best bound (for after freeTransform)
    double objbound;
};

/*
 * local methods
 */

static
CSIP_STATUS getStatus(CSIP_MODEL *model)
{
    SCIP_STATUS status = SCIPgetStatus(model->scip);
    switch (status)
    {
    case SCIP_STATUS_UNKNOWN:
        return CSIP_STATUS_UNKNOWN;
    case SCIP_STATUS_USERINTERRUPT:
        return CSIP_STATUS_USERLIMIT;
    case SCIP_STATUS_NODELIMIT:
        return CSIP_STATUS_NODELIMIT;
    case SCIP_STATUS_TOTALNODELIMIT:
        return CSIP_STATUS_NODELIMIT;
    case SCIP_STATUS_STALLNODELIMIT:
        return CSIP_STATUS_USERLIMIT;
    case SCIP_STATUS_TIMELIMIT:
        return CSIP_STATUS_TIMELIMIT;
    case SCIP_STATUS_MEMLIMIT:
        return CSIP_STATUS_MEMLIMIT;
    case SCIP_STATUS_GAPLIMIT:
        return CSIP_STATUS_USERLIMIT;
    case SCIP_STATUS_SOLLIMIT:
        return CSIP_STATUS_USERLIMIT;
    case SCIP_STATUS_BESTSOLLIMIT:
        return CSIP_STATUS_USERLIMIT;
    case SCIP_STATUS_RESTARTLIMIT:
        return CSIP_STATUS_USERLIMIT;
    case SCIP_STATUS_OPTIMAL:
        return CSIP_STATUS_OPTIMAL;
    case SCIP_STATUS_INFEASIBLE:
        return CSIP_STATUS_INFEASIBLE;
    case SCIP_STATUS_UNBOUNDED:
        return CSIP_STATUS_UNBOUNDED;
    case SCIP_STATUS_INFORUNBD:
        return CSIP_STATUS_INFORUNBD;
    default:
        return CSIP_STATUS_UNKNOWN;
    }
}

static
CSIP_RETCODE createLinCons(CSIP_MODEL *model, int numindices, int *indices,
                           double *coefs, double lhs, double rhs, SCIP_CONS **cons)
{
    SCIP *scip;
    SCIP_VAR *var;
    int i;

    scip = model->scip;

    SCIP_in_CSIP(SCIPcreateConsBasicLinear(scip, cons, "lincons", 0, NULL, NULL,
                                           lhs, rhs));

    for (i = 0; i < numindices; ++i)
    {
        var = model->vars[indices[i]];
        SCIP_in_CSIP(SCIPaddCoefLinear(scip, *cons, var, coefs[i]));
    }

    return CSIP_RETCODE_OK;
}

static
CSIP_RETCODE addCons(CSIP_MODEL *model, SCIP_CONS *cons, int *idx)
{
    SCIP *scip;

    scip = model->scip;

    SCIP_in_CSIP(SCIPaddCons(scip, cons));

    // do we need to resize?
    if (model->nconss >= model->consssize)
    {
        model->consssize = GROWFACTOR * model->consssize;
        model->conss = (SCIP_CONS **) realloc(
                           model->conss,  model->consssize * sizeof(SCIP_CONS *));
        if (model->conss == NULL)
        {
            return CSIP_RETCODE_NOMEMORY;
        }
    }

    if (idx != NULL)
    {
        *idx = model->nconss;
        model->conss[*idx] = cons;
    }
    else
    {
        model->conss[model->nconss] = cons;
    }

    ++(model->nconss);

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE reformSenseMinimize(CSIP_MODEL *model)
{
    if (model->sense == SCIP_OBJSENSE_MAXIMIZE)
    {
        // negate all objective coefficients
        for (int i = 0; i < model->nvars; ++i)
        {
            SCIP_VAR *var = model->vars[i];
            double coef = SCIPvarGetObj(var);
            SCIP_in_CSIP(SCIPchgVarObj(model->scip, var, -1.0 * coef));
        }

    }
    return CSIP_RETCODE_OK;
}

/*
 * interface methods
 */
CSIP_RETCODE CSIPcreateModel(CSIP_MODEL **modelptr)
{
    CSIP_MODEL *model;

    *modelptr = (CSIP_MODEL *)malloc(sizeof(CSIP_MODEL));
    if (*modelptr == NULL)
    {
        return CSIP_RETCODE_NOMEMORY;
    }

    model = *modelptr;

    SCIP_in_CSIP(SCIPcreate(&model->scip));
    SCIP_in_CSIP(SCIPincludeDefaultPlugins(model->scip));
    SCIP_in_CSIP(SCIPcreateProbBasic(model->scip, "name"));

    model->nvars = 0;
    model->varssize = INITIALSIZE;
    model->vars = (SCIP_VAR **) malloc(INITIALSIZE * sizeof(SCIP_VAR *));
    if (model->vars == NULL)
    {
        return CSIP_RETCODE_NOMEMORY;
    }

    model->nconss = 0;
    model->consssize = INITIALSIZE;
    model->conss = (SCIP_CONS **) malloc(INITIALSIZE * sizeof(SCIP_CONS *));
    if (model->conss == NULL)
    {
        return CSIP_RETCODE_NOMEMORY;
    }

    model->nlazycb = 0;
    model->nheur = 0;
    model->sense = SCIP_OBJSENSE_MINIMIZE;
    model->initialsol = NULL;
    model->status = CSIP_STATUS_UNKNOWN;
    model->objbound = strtod("NaN", NULL);

    CSIP_CALL(CSIPsetParameter(model, "display/width", 80));

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPfreeModel(CSIP_MODEL *model)
{
    int i;

    if (model->initialsol != NULL) // solve was not called?
    {
        SCIP_in_CSIP(SCIPfreeSol(model->scip, &model->initialsol));
    }

    /* SCIPreleaseVar sets the given pointer to NULL. However, this pointer is
     * needed when SCIPfree is called, because it will call the lock method again
     * which works on the vars stored at model, so we give another pointer
     * TODO: maybe this is still wrong and one should free the transformed problem
     * first and then release the vars... we have to check for BMS memory leaks
     */
    for (i = 0; i < model->nvars; ++i)
    {
        SCIP_VAR *var;
        var = model->vars[i];
        SCIP_in_CSIP(SCIPreleaseVar(model->scip, &var));
    }
    for (i = 0; i < model->nconss; ++i)
    {
        SCIP_in_CSIP(SCIPreleaseCons(model->scip, &model->conss[i]));
    }
    SCIP_in_CSIP(SCIPfree(&model->scip));

    free(model->conss);
    free(model->vars);
    free(model);

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPaddVar(CSIP_MODEL *model, double lowerbound, double upperbound,
                        int vartype, int *idx)
{
    SCIP *scip;
    SCIP_VAR *var;

    scip = model->scip;

    SCIP_in_CSIP(SCIPcreateVarBasic(scip, &var, NULL, lowerbound, upperbound, 0.0,
                                    vartype));
    SCIP_in_CSIP(SCIPaddVar(scip, var));

    // do we need to resize?
    if (model->nvars >= model->varssize)
    {
        model->varssize = GROWFACTOR * model->varssize;
        model->vars = (SCIP_VAR **) realloc(
                          model->vars,  model->varssize * sizeof(SCIP_VAR *));
        if (model->vars == NULL)
        {
            return CSIP_RETCODE_NOMEMORY;
        }
    }

    if (idx != NULL)
    {
        *idx = model->nvars;
        model->vars[*idx] = var;
    }
    else
    {
        model->vars[model->nvars] = var;
    }
    ++(model->nvars);

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPchgVarLB(CSIP_MODEL *model, int numindices, int *indices,
                          double *lowerbounds)
{
    int i;
    SCIP *scip;
    SCIP_VAR *var;

    scip = model->scip;

    for (i = 0; i < numindices; ++i)
    {
        var = model->vars[indices[i]];
        SCIP_in_CSIP(SCIPchgVarLb(scip, var, lowerbounds[i]));
    }

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPchgVarUB(CSIP_MODEL *model, int numindices, int *indices,
                          double *upperbounds)
{
    int i;
    SCIP *scip;
    SCIP_VAR *var;

    scip = model->scip;

    for (i = 0; i < numindices; ++i)
    {
        var = model->vars[indices[i]];
        SCIP_in_CSIP(SCIPchgVarUb(scip, var, upperbounds[i]));
    }

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPchgVarType(
    CSIP_MODEL *model, int varindex, CSIP_VARTYPE vartype)
{
    SCIP *scip = model->scip;
    SCIP_VAR *var = model->vars[varindex];
    SCIP_Bool infeas = FALSE;

    SCIP_in_CSIP(SCIPchgVarType(scip, var, vartype, &infeas));
    // TODO: don't ignore `infeas`?
    // for SCIP, solving a problem with a binary variable with bounds not in [0,1] produces an error
    // here we change them to the correct value, since JuMP seems to expect that behaviour
    // see JuMP tests: [probmod] Test bound modification on binaries
    if (vartype == CSIP_VARTYPE_BINARY && SCIPvarGetLbLocal(var) < 0.0)
    {
        SCIP_in_CSIP(SCIPchgVarLb(scip, var, 0.0));
    }
    if (vartype == CSIP_VARTYPE_BINARY && SCIPvarGetUbLocal(var) > 1.0)
    {
        SCIP_in_CSIP(SCIPchgVarUb(scip, var, 1.0));
    }

    return CSIP_RETCODE_OK;
}

CSIP_VARTYPE CSIPgetVarType(CSIP_MODEL *model, int varindex)
{
    assert(varindex >= 0 && varindex < model->nvars);

    switch (SCIPvarGetType(model->vars[varindex]))
    {
    case SCIP_VARTYPE_BINARY:
        return CSIP_VARTYPE_BINARY;
    case SCIP_VARTYPE_INTEGER:
        return CSIP_VARTYPE_INTEGER;
    case SCIP_VARTYPE_IMPLINT:
        return CSIP_VARTYPE_IMPLINT;
    case SCIP_VARTYPE_CONTINUOUS:
        return CSIP_VARTYPE_CONTINUOUS;
    }
    return -1;
}

CSIP_RETCODE CSIPaddLinCons(CSIP_MODEL *model, int numindices, int *indices,
                            double *coefs, double lhs, double rhs, int *idx)
{
    SCIP_CONS *cons;

    CSIP_CALL(createLinCons(model, numindices, indices, coefs, lhs, rhs, &cons));
    CSIP_CALL(addCons(model, cons, idx));

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPaddQuadCons(CSIP_MODEL *model, int numlinindices,
                             int *linindices,
                             double *lincoefs, int numquadterms,
                             int *quadrowindices, int *quadcolindices,
                             double *quadcoefs, double lhs, double rhs, int *idx)
{
    int i;
    SCIP *scip;
    SCIP_VAR *linvar;
    SCIP_VAR *var1;
    SCIP_VAR *var2;
    SCIP_CONS *cons;

    scip = model->scip;

    SCIP_in_CSIP(SCIPcreateConsBasicQuadratic(scip, &cons, "quadcons", 0, NULL,
                 NULL, 0, NULL, NULL, NULL, lhs, rhs));

    for (i = 0; i < numlinindices; ++i)
    {
        linvar = model->vars[linindices[i]];
        SCIP_in_CSIP(SCIPaddLinearVarQuadratic(scip, cons, linvar, lincoefs[i]));
    }

    for (i = 0; i < numquadterms; ++i)
    {
        var1 = model->vars[quadrowindices[i]];
        var2 = model->vars[quadcolindices[i]];
        SCIP_in_CSIP(SCIPaddBilinTermQuadratic(scip, cons, var1, var2, quadcoefs[i]));
    }

    CSIP_CALL(addCons(model, cons, idx));

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPaddSOS1(
    CSIP_MODEL *model, int numindices, int *indices, double *weights, int *idx)
{
    SCIP *scip = model->scip;
    SCIP_CONS *cons;
    SCIP_VAR **vars = (SCIP_VAR **) malloc(numindices * sizeof(SCIP_VAR *));
    if (vars == NULL)
    {
        return CSIP_RETCODE_NOMEMORY;
    }
    for (int i = 0; i < numindices; ++i)
    {
        vars[i] = model->vars[indices[i]];
    }

    SCIP_in_CSIP(SCIPcreateConsBasicSOS1(
                     scip, &cons, "sos1", numindices, vars, weights));
    CSIP_CALL(addCons(model, cons, idx));

    free(vars);

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPaddSOS2(
    CSIP_MODEL *model, int numindices, int *indices, double *weights, int *idx)
{
    SCIP *scip = model->scip;
    SCIP_CONS *cons;
    SCIP_VAR **vars = (SCIP_VAR **) malloc(numindices * sizeof(SCIP_VAR *));
    if (vars == NULL)
    {
        return CSIP_RETCODE_NOMEMORY;
    }
    for (int i = 0; i < numindices; ++i)
    {
        vars[i] = model->vars[indices[i]];
    }

    SCIP_in_CSIP(SCIPcreateConsBasicSOS2(
                     scip, &cons, "sos2", numindices, vars, weights));
    CSIP_CALL(addCons(model, cons, idx));

    free(vars);

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPsetObj(CSIP_MODEL *model, int numindices, int *indices,
                        double *coefs)
{
    int i;
    SCIP *scip;
    SCIP_VAR *var;

    scip = model->scip;

    for (i = 0; i < numindices; ++i)
    {
        var = model->vars[indices[i]];
        SCIP_in_CSIP(SCIPchgVarObj(scip, var, coefs[i]));
    }

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPsetSenseMinimize(CSIP_MODEL *model)
{
    model->sense = SCIP_OBJSENSE_MINIMIZE;
    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPsetSenseMaximize(CSIP_MODEL *model)
{
    model->sense = SCIP_OBJSENSE_MAXIMIZE;
    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPsolve(CSIP_MODEL *model)
{
    // always pose the problem as a minimization, because otherwise the order of
    // the stored solutions will be messed up after the freeTransform :-(
    CSIP_CALL(reformSenseMinimize(model));

    // add initial solution
    if (model->initialsol != NULL)
    {
        unsigned int stored;
        SCIP_in_CSIP(SCIPaddSolFree(model->scip, &model->initialsol, &stored));
    }

    SCIP_in_CSIP(SCIPsolve(model->scip));
    model->status = getStatus(model);

    double dual = SCIPgetDualbound(model->scip);
    model->objbound = (model->sense == SCIP_OBJSENSE_MINIMIZE ? dual : -dual);

    SCIP_in_CSIP(SCIPfreeTransform(model->scip));

    // reset the objective (might be negated)
    CSIP_CALL(reformSenseMinimize(model));

    return CSIP_RETCODE_OK;
}


CSIP_STATUS CSIPgetStatus(CSIP_MODEL *model)
{
    return model->status;
}

double CSIPgetObjValue(CSIP_MODEL *model)
{
    SCIP_SOL *sol = SCIPgetBestSol(model->scip);
    if (sol == NULL)
    {
        return CSIP_RETCODE_ERROR;
    }

    double objval = SCIPgetSolOrigObj(model->scip, sol);
    return model->sense == SCIP_OBJSENSE_MINIMIZE ? objval : -objval;
}

double CSIPgetObjBound(CSIP_MODEL *model)
{
    return model->objbound;
}


CSIP_RETCODE CSIPgetVarValues(CSIP_MODEL *model, double *output)
{
    int i;
    SCIP *scip;
    SCIP_VAR *var;

    scip = model->scip;

    if (SCIPgetBestSol(scip) == NULL)
    {
        return CSIP_RETCODE_ERROR;
    }

    for (i = 0; i < model->nvars; ++i)
    {
        var = model->vars[i];
        output[i] = SCIPgetSolVal(scip, SCIPgetBestSol(scip), var);
    }

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPsetParameterGeneric(
    CSIP_MODEL *model, const char *name, void *value)
{
    SCIP_in_CSIP(SCIPsetParam(model->scip, name, value));
    return CSIP_RETCODE_OK;
}

int CSIPgetNumVars(CSIP_MODEL *model)
{
    return model->nvars;
}

CSIP_RETCODE CSIPsetInitialSolution(CSIP_MODEL *model, double *values)
{
    if (model->initialsol != NULL) // was solution already given?
    {
        SCIP_in_CSIP(SCIPfreeSol(model->scip, &model->initialsol));
    }
    assert(model->initialsol == NULL);

    // create new solution object
    SCIP_in_CSIP(SCIPcreateSol(model->scip, &model->initialsol, NULL));

    // copy the given values
    SCIP_in_CSIP(SCIPsetSolVals(model->scip, model->initialsol, model->nvars,
                                model->vars, values));

    // it will be given to SCIP in the CSIPsolve

    return CSIP_RETCODE_OK;
}

void *CSIPgetInternalSCIP(CSIP_MODEL *model)
{
    return model->scip;
}


/*
 * Constraint Handler
 */

/* constraint handler data */
struct SCIP_ConshdlrData
{
    CSIP_MODEL *model;
    CSIP_LAZYCALLBACK callback;
    void *userdata;
    SCIP_Bool checkonly;
    SCIP_Bool feasible;
    SCIP_SOL *sol;
};

SCIP_DECL_CONSFREE(consFreeLazy)
{
    SCIP_CONSHDLRDATA *conshdlrdata;

    conshdlrdata = SCIPconshdlrGetData(conshdlr);
    assert(conshdlrdata != NULL);

    SCIPfreeMemory(scip, &conshdlrdata);

    SCIPconshdlrSetData(conshdlr, NULL);

    return SCIP_OKAY;
}

SCIP_DECL_CONSENFOLP(consEnfolpLazy)
{
    SCIP_CONSHDLRDATA *conshdlrdata;

    *result = SCIP_FEASIBLE;

    conshdlrdata = SCIPconshdlrGetData(conshdlr);
    conshdlrdata->checkonly = FALSE;
    conshdlrdata->feasible = TRUE;

    CSIP_in_SCIP(conshdlrdata->callback(conshdlrdata->model,
                                        conshdlrdata, conshdlrdata->userdata));

    if (!conshdlrdata->feasible)
    {
        *result = SCIP_CONSADDED;
    }

    return SCIP_OKAY;
}

/* enfo pseudo solution just call enfo lp solution */
SCIP_DECL_CONSENFOPS(consEnfopsLazy)
{
    return consEnfolpLazy(scip, conshdlr, conss, nconss, nusefulconss,
                          solinfeasible, result);
}

/* check callback */
SCIP_DECL_CONSCHECK(consCheckLazy)
{
    SCIP_CONSHDLRDATA *conshdlrdata;

    *result = SCIP_FEASIBLE;

    conshdlrdata = SCIPconshdlrGetData(conshdlr);
    conshdlrdata->checkonly = TRUE;
    conshdlrdata->feasible = TRUE;
    conshdlrdata->sol = sol;

    CSIP_in_SCIP(conshdlrdata->callback(conshdlrdata->model,
                                        conshdlrdata, conshdlrdata->userdata));

    if (!conshdlrdata->feasible)
    {
        *result = SCIP_INFEASIBLE;
    }

    return SCIP_OKAY;
}

/* locks callback */
SCIP_DECL_CONSLOCK(consLockLazy)
{
    int i;
    SCIP_VAR *var;
    SCIP_CONSHDLRDATA *conshdlrdata;

    conshdlrdata = SCIPconshdlrGetData(conshdlr);

    assert(scip == conshdlrdata->model->scip);

    for (i = 0; i < conshdlrdata->model->nvars; ++i)
    {
        var = conshdlrdata->model->vars[i];
        SCIP_CALL(SCIPaddVarLocks(scip, var, nlockspos + nlocksneg,
                                  nlockspos + nlocksneg));
    }

    return SCIP_OKAY;
}

/*
 * callback methods
 */

CSIP_RETCODE CSIPaddLazyCallback(CSIP_MODEL *model, CSIP_LAZYCALLBACK callback,
                                 int fractional, void *userdata)
{
    SCIP_CONSHDLRDATA *conshdlrdata;
    SCIP_CONSHDLR *conshdlr;
    SCIP *scip;
    int priority;
    char name[SCIP_MAXSTRLEN];

    scip = model->scip;

    /* it is -1 or 1 because cons_integral has priority 0 */
    priority = fractional ? -1 : 1;

    SCIP_in_CSIP(SCIPallocMemory(scip, &conshdlrdata));

    conshdlrdata->model = model;
    conshdlrdata->callback = callback;
    conshdlrdata->userdata = userdata;

    SCIPsnprintf(name, SCIP_MAXSTRLEN, "lazycons_%d", model->nlazycb);
    SCIP_in_CSIP(SCIPincludeConshdlrBasic(
                     scip, &conshdlr, name, "lazy constraint callback",
                     priority, -1, -1, FALSE,
                     consEnfolpLazy, consEnfopsLazy, consCheckLazy, consLockLazy,
                     conshdlrdata));

    SCIP_in_CSIP(SCIPsetConshdlrFree(scip, conshdlr, consFreeLazy));
    model->nlazycb += 1;

    return CSIP_RETCODE_OK;
}

/* returns LP or given solution depending whether we are called from check or enfo */
CSIP_RETCODE CSIPcbGetVarValues(CSIP_CBDATA *cbdata, double *output)
{
    int i;
    SCIP *scip;
    SCIP_VAR *var;
    SCIP_SOL *sol;

    scip = cbdata->model->scip;

    if (cbdata->checkonly)
    {
        sol = cbdata->sol;
    }
    else
    {
        sol = NULL;
    }

    for (i = 0; i < cbdata->model->nvars; ++i)
    {
        var = cbdata->model->vars[i];
        output[i] = SCIPgetSolVal(scip, sol, var);
    }

    return CSIP_RETCODE_OK;
}

CSIP_RETCODE CSIPcbAddLinCons(CSIP_CBDATA *cbdata, int numindices, int *indices,
                              double *coefs, double lhs, double rhs, int islocal)
{
    SCIP *scip;
    SCIP_CONS *cons;
    SCIP_SOL *sol;
    SCIP_RESULT result;

    scip = cbdata->model->scip;

    if (cbdata->checkonly)
    {
        sol = cbdata->sol;
    }
    else
    {
        sol = NULL;
    }

    /* Is it reasonable to assume that if we solved the problem, then the lazy constraint
     * is satisfied in the original problem? We get the error:
     * "method <SCIPcreateCons> cannot be called in the solved stage"
     * and I am guessing this is because SCIP is checking whether the solution found in the
     * presolved problem is feasible for the original problem. It could happen it is not feasible
     * because of numerics mainly, hence the question in the comment
     */
    if (SCIPgetStage(scip) == SCIP_STAGE_SOLVED)
    {
        assert(cbdata->checkonly);
        cbdata->feasible = TRUE; /* to be very explicit */
        return CSIP_RETCODE_OK;
    }

    CSIP_CALL(createLinCons(cbdata->model, numindices, indices, coefs, lhs, rhs,
                            &cons));
    SCIP_in_CSIP(SCIPcheckCons(scip, cons, sol, FALSE, FALSE, FALSE, &result));

    if (result == SCIP_INFEASIBLE)
    {
        cbdata->feasible = FALSE;
    }

    if (!cbdata->checkonly)
    {
        /* we do not store cons, because the original problem does not contain them;
         * and there is an issue when freeTransform is called
         */
        SCIP_in_CSIP(SCIPaddCons(scip, cons));
    }
    SCIP_in_CSIP(SCIPreleaseCons(cbdata->model->scip, &cons));

    return CSIP_RETCODE_OK;
}

/* Heuristic Plugin */

struct SCIP_HeurData
{
    CSIP_MODEL *model;
    CSIP_HEURCALLBACK callback;
    void *userdata;
    SCIP_HEUR *heur;
    SCIP_SOL *sol;
};

static
SCIP_DECL_HEURFREE(heurFreeUser)
{
    SCIP_HEURDATA *heurdata;

    heurdata = SCIPheurGetData(heur);
    assert(heurdata != NULL);

    SCIPfreeMemory(scip, &heurdata);
    SCIPheurSetData(heur, NULL);

    return SCIP_OKAY;
}

static
SCIP_DECL_HEUREXEC(heurExecUser)
{
    SCIP_HEURDATA *heurdata = SCIPheurGetData(heur);
    assert(heurdata != NULL);

    *result = SCIP_DIDNOTFIND;
    assert(heurdata->sol == NULL);

    CSIP_in_SCIP(heurdata->callback(heurdata->model, heurdata,
                                    heurdata->userdata));

    if (heurdata->sol != NULL)
    {
        unsigned int stored = 0;
        SCIP_CALL(SCIPtrySolFree(heurdata->model->scip, &heurdata->sol,
                                 FALSE, TRUE, TRUE, TRUE, &stored));
        if (stored)
        {
            *result = SCIP_FOUNDSOL;
        }
    }
    assert(heurdata->sol == NULL);

    return SCIP_OKAY;
}

// Copy values of solution to output array. Call this function from your
// heuristic callback. Solution is LP relaxation of current node.
CSIP_RETCODE CSIPheurGetVarValues(CSIP_HEURDATA *heurdata, double *output)
{
    CSIP_MODEL *model = heurdata->model;
    SCIP_in_CSIP(SCIPgetSolVals(model->scip, NULL, model->nvars, model->vars,
                                output));
    return CSIP_RETCODE_OK;
}

// Supply a solution (as a dense array). Only complete solutions are supported.
CSIP_RETCODE CSIPheurSetSolution(CSIP_HEURDATA *heurdata, double *values)
{
    CSIP_MODEL *model = heurdata->model;
    SCIP_in_CSIP(SCIPcreateSol(model->scip, &heurdata->sol, heurdata->heur));
    SCIP_in_CSIP(SCIPsetSolVals(model->scip, heurdata->sol, model->nvars,
                                model->vars, values));
    return CSIP_RETCODE_OK;
}

// Add a heuristic callback to the model.
// You may use userdata to pass any data.
CSIP_RETCODE CSIPaddHeuristicCallback(
    CSIP_MODEL *model, CSIP_HEURCALLBACK callback, void *userdata)
{
    SCIP_HEURDATA *heurdata;
    SCIP_HEUR *heur;
    SCIP *scip;
    char name[SCIP_MAXSTRLEN];

    scip = model->scip;

    SCIP_in_CSIP(SCIPallocMemory(scip, &heurdata));

    SCIPsnprintf(name, SCIP_MAXSTRLEN, "heur_%d", model->nheur);
    SCIP_in_CSIP(SCIPincludeHeurBasic(
                     scip, &heur, name, "heuristic callback", 'x',
                     1, 1, 0, -1, SCIP_HEURTIMING_AFTERNODE, FALSE,
                     heurExecUser, heurdata));
    heurdata->model = model;
    heurdata->callback = callback;
    heurdata->userdata = userdata;
    heurdata->sol = NULL;
    heurdata->heur = heur;

    SCIP_in_CSIP(SCIPsetHeurFree(scip, heur, heurFreeUser));
    model->nheur += 1;

    return CSIP_RETCODE_OK;
}

/* $Id$
 *
 *
 * Copyright (C) 1999-2003 The Omega Project for Statistical Computing.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "RS-SQLite.h"

/* size_t getline(char**, size_t*, FILE*); */
char *compiledVarsion = SQLITE_VERSION;

/* the following are for parameter binding */
RS_SQLite_bindParam *RS_SQLite_createParameterBinding(int n,
                        s_object *bind_data, sqlite3_stmt *stmt,
                        char *errorMsg);
void RS_SQLite_freeParameterBinding(int n,
                                     RS_SQLite_bindParam *param);
int RS_sqlite_import(sqlite3 *db, const char *zTable,
       const char *zFile, const char *separator, const char *eol, int skip);
int corrected_sqlite3_step(sqlite3_stmt *pStatement);

/* The macro NA_STRING is a CHRSXP in R but a char * in Splus */
#ifdef USING_R
/*#  define RS_NA_STRING "<NA>" */           /* CHR_EL(NA_STRING,0)  */
#  define RS_NA_STRING CHAR_DEREF(NA_STRING)
#else
#  define RS_NA_STRING NA_STRING
#endif

/* R and S Database Interface to the SQLite embedded SQL engine
 *
 * C Function library which can be used to run SQL queries from
 * inside of Splus5.x, or R.
 * This driver hooks R/S and SQLite and implements the proposed S-DBI
 * generic R/S-database interface 0.2.
 *
 * We need to simulate (fake) exception objects. We do this piggy-
 * backing on the member "drvData" of the RS_DBI_connection structure.
 * The exception is a 2-member struct with errorNum and erroMsg
 * (this should be extended to allow multiple errors in the structure,
 * like in the ODBC API.)
 *
 * For details on SQLite see http://www.sqlite.org.
 * TODO:
 *    1. Make sure the code is thread-safe, in particular,
 *       we need to remove the PROBLEM ... ERROR macros
 *       in RS_DBI_errorMessage() because it's definetely not
 *       thread-safe.  But see RS_DBI_setException().
 *     2. Use proper types instead of getting everything as character
 */


Mgr_Handle *
RS_SQLite_init(s_object *config_params, s_object *reload, s_object *cache)
{
  S_EVALUATOR

  /* Currently we can specify the 2 defaults max conns and records per
   * fetch (this last one can be over-ridden explicitly in the S call to fetch).
   */
  RS_DBI_manager *mgr;
  Mgr_Handle *mgrHandle;
  Sint  fetch_default_rec, force_reload, max_con;
  Sint  *shared_cache;
  const char *drvName = "SQLite";
  const char *clientVersion = sqlite3_libversion();

  /* make sure we're running with the "right" version of the SQLite library */
  if(strcmp(clientVersion, compiledVarsion)){
     char  buf[256];
     (void) sprintf(buf,
                    "%s mismatch between compiled version %s and runtime version %s",
                    drvName, compiledVarsion, clientVersion);
     RS_DBI_errorMessage(buf, RS_DBI_WARNING);
  }
  if(GET_LENGTH(config_params)!=2){
     RS_DBI_errorMessage(
        "initialization error: must specify max num of conenctions and default number of rows per fetch",
        RS_DBI_ERROR);
  }
  max_con = INT_EL(config_params, 0);
  fetch_default_rec = INT_EL(config_params,1);
  force_reload = LGL_EL(reload,0);

  mgrHandle = RS_DBI_allocManager(drvName, max_con, fetch_default_rec,
           force_reload);

  mgr = RS_DBI_getManager(mgrHandle);

  shared_cache = (Sint *)malloc(sizeof(Sint));
  if(!shared_cache){
    RS_DBI_errorMessage(
        "could not malloc space for driver data", RS_DBI_ERROR);
  }

  *shared_cache = LGL_EL(cache,0);
  mgr->drvData = (void *)shared_cache;

  if(*shared_cache)
      sqlite3_enable_shared_cache(1);

  return mgrHandle;
}

s_object *
RS_SQLite_closeManager(Mgr_Handle *mgrHandle)
{
  S_EVALUATOR

  RS_DBI_manager *mgr;
  s_object *status;
  Sint *shared_cache;

  mgr = RS_DBI_getManager(mgrHandle);
  if(mgr->num_con)
    RS_DBI_errorMessage("there are opened connections -- close them first",
      RS_DBI_ERROR);

  sqlite3_enable_shared_cache(0);
  shared_cache = (Sint *)mgr->drvData;
  if(shared_cache){
    free(shared_cache);
  }

  RS_DBI_freeManager(mgrHandle);

  MEM_PROTECT(status = NEW_LOGICAL((Sint) 1));
  LGL_EL(status,0) = TRUE;
  MEM_UNPROTECT(1);
  return status;
}

/* open a connection with the same parameters used for in conHandle */
Con_Handle *
RS_SQLite_cloneConnection(Con_Handle *conHandle)
{
  S_EVALUATOR

  Mgr_Handle  *mgrHandle;
  RS_DBI_connection  *con;
  RS_SQLite_conParams *conParams;
  s_object    *con_params;
  char   buf1[256];

  /* get connection params used to open existing connection */
  con = RS_DBI_getConnection(conHandle);
  conParams = (RS_SQLite_conParams *) con->conParams;

  mgrHandle = RS_DBI_asMgrHandle(MGR_ID(conHandle));

  /* copy dbname and loadable_extensions into a 2-element character
   * vector to be passed to the RS_SQLite_newConnection() function.
   */
  MEM_PROTECT(con_params = NEW_CHARACTER((Sint) 2));
  SET_CHR_EL(con_params, 0, C_S_CPY(conParams->dbname));
  sprintf(buf1, "%d", (int) conParams->loadable_extensions);
  SET_CHR_EL(con_params, 1, C_S_CPY(buf1));
  MEM_UNPROTECT(1);

  return RS_SQLite_newConnection(mgrHandle, con_params);
}

RS_SQLite_conParams *
RS_SQLite_allocConParams(const char *dbname, int loadable_extensions)
{
  RS_SQLite_conParams *conParams;

  conParams = (RS_SQLite_conParams *) malloc(sizeof(RS_SQLite_conParams));
  if(!conParams){
    RS_DBI_errorMessage("could not malloc space for connection params",
                       RS_DBI_ERROR);
  }
  conParams->dbname = RS_DBI_copyString(dbname);
  conParams->loadable_extensions = loadable_extensions;
  return conParams;
}

void
RS_SQLite_freeConParams(RS_SQLite_conParams *conParams)
{
  if(conParams->dbname)
     free(conParams->dbname);
  /* conParams->loadable_extensions is an int, thus needs no free */
  free(conParams);
  conParams = (RS_SQLite_conParams *)NULL;
  return;
}

/* set exception object (allocate memory if needed) */
void
RS_SQLite_setException(RS_DBI_connection *con, int err_no, const char *err_msg)
{
   RS_SQLite_exception *ex;

   ex = (RS_SQLite_exception *) con->drvData;
   if(!ex){    /* brand new exception object */
      ex = (RS_SQLite_exception *) malloc(sizeof(RS_SQLite_exception));
      if(!ex)
         RS_DBI_errorMessage("could not allocate SQLite exception object",
                RS_DBI_ERROR);
   }
   else
      free(ex->errorMsg);      /* re-use existing object */

   ex->errorNum = err_no;
   if(err_msg)
      ex->errorMsg = RS_DBI_copyString(err_msg);
   else
      ex->errorMsg = (char *) NULL;

   con->drvData = (void *) ex;
   return;
}

void
RS_SQLite_freeException(RS_DBI_connection *con)
{
   RS_SQLite_exception *ex = (RS_SQLite_exception *) con->drvData;

   if(!ex) return;
   if(ex->errorMsg) free(ex->errorMsg);
   free(ex);
   return;
}

Con_Handle *
RS_SQLite_newConnection(Mgr_Handle *mgrHandle, s_object *s_con_params)
{
  S_EVALUATOR

  RS_DBI_connection   *con;
  RS_SQLite_conParams *conParams;
  Con_Handle  *conHandle;
  sqlite3     *db_connection, **pDb;
  char        *dbname = NULL;
  int         rc, loadable_extensions;

  if(!is_validHandle(mgrHandle, MGR_HANDLE_TYPE))
    RS_DBI_errorMessage("invalid SQLiteManager", RS_DBI_ERROR);

  /* unpack connection parameters from S object */
  dbname = CHR_EL(s_con_params, 0);
  loadable_extensions = (Sint) atol(CHR_EL(s_con_params,1));
  pDb = (sqlite3 **) calloc((size_t) 1, sizeof(sqlite3 *));

  rc = sqlite3_open(dbname, pDb);
  db_connection = *pDb;           /* available, even if open fails! See API */
  if(rc != SQLITE_OK){
     char buf[256];
     sprintf(buf, "could not connect to dbname \"%s\"\n", dbname);
     RS_DBI_errorMessage(buf, RS_DBI_ERROR);
  }

  /* SQLite connections can only have 1 result set open at a time */
  conHandle = RS_DBI_allocConnection(mgrHandle, (Sint) 1);
  con = RS_DBI_getConnection(conHandle);
  if(!con){
    (void) sqlite3_close(db_connection);
    RS_DBI_freeConnection(conHandle);
    RS_DBI_errorMessage("could not alloc space for connection object",
                        RS_DBI_ERROR);
  }
  /* save connection parameters in the connection object */
  conParams = RS_SQLite_allocConParams(dbname, loadable_extensions);
  con->drvConnection = (void *) db_connection;
  con->conParams = (void *) conParams;
  RS_SQLite_setException(con, SQLITE_OK, "OK");

  /* enable loadable extensions if required */
  if(loadable_extensions != 0)
    sqlite3_enable_load_extension(db_connection, 1);

  return conHandle;
}

s_object *
RS_SQLite_closeConnection(Con_Handle *conHandle)
{
  S_EVALUATOR

  RS_DBI_connection *con;
  sqlite3 *db_connection;
  s_object *status;
  int      rc;

  con = RS_DBI_getConnection(conHandle);
  if(con->num_res>0){
    RS_DBI_errorMessage(
     "close the pending result sets before closing this connection",
     RS_DBI_ERROR);
  }

  db_connection = (sqlite3 *) con->drvConnection;
  rc = sqlite3_close(db_connection);  /* it also frees db_connection */
  if(rc==SQLITE_BUSY){
    RS_DBI_errorMessage(
     "finalize the pending prepared statements before closing this connection",
     RS_DBI_ERROR);
  }
  else if(rc!=SQLITE_OK){
    RS_DBI_errorMessage(
     "internal error: SQLite could not close the connection",
     RS_DBI_ERROR);
  }

  /* make sure we first free the conParams and SQLite connection from
   * the RS-RBI connection object.
   */

  if(con->conParams){
     RS_SQLite_freeConParams(con->conParams);
     /* we must set con->conParms to NULL (not just free it) to signal
      * RS_DBI_freeConnection that it is okay to free the connection itself.
      */
     con->conParams = (RS_SQLite_conParams *) NULL;
  }
  /* free(db_connection); */    /* freed by sqlite3_close? */
  con->drvConnection = (void *) NULL;

  RS_SQLite_freeException(con);
  con->drvData = (void *) NULL;
  RS_DBI_freeConnection(conHandle);

  MEM_PROTECT(status = NEW_LOGICAL((Sint) 1));
  LGL_EL(status, 0) = TRUE;
  MEM_UNPROTECT(1);

  return status;
}


int RS_SQLite_get_row_count(sqlite3* db, const char* tname) {
    char* sqlQuery;
    const char* sqlFmt = "select rowid from %s order by rowid desc limit 1";
    int qrylen = strlen(sqlFmt);
    int rc = 0;
    int i, ans;
    sqlite3_stmt* stmt;
    const char* tail;
    
    qrylen += strlen(tname) + 1;
    sqlQuery = (char*)  R_alloc(qrylen, sizeof(char));
    snprintf(sqlQuery, qrylen, sqlFmt, tname);
    rc = sqlite3_prepare(db, sqlQuery, -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
        error("SQL error: %s\n", sqlite3_errmsg(db));
    }
    rc = sqlite3_step(stmt);
    ans = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return ans;
}


SEXP RS_SQLite_quick_column(Con_Handle *conHandle, SEXP table, SEXP column)
{
    SEXP ans;
    RS_DBI_connection *con;
    sqlite3           *db_connection;
    int               numrows;
    char              sqlQuery[500];
    char              *table_name;
    char              *column_name;
    int               rc;
    sqlite3_stmt      *stmt;
    const char        *tail;
    int               i = 0;
    int               col_type;
    int              *intans;
    double           *doubleans;

    con = RS_DBI_getConnection(conHandle);
    db_connection = (sqlite3 *) con->drvConnection;
    table_name = CHAR(STRING_ELT(table, 0));
    column_name = CHAR(STRING_ELT(column, 0));
    numrows = RS_SQLite_get_row_count(db_connection, table_name);
    snprintf(sqlQuery, sizeof(sqlQuery), "select %s from %s", 
             column_name, table_name);
    
    rc = sqlite3_prepare(db_connection, sqlQuery, strlen(sqlQuery), &stmt, &tail);
    /* FIXME: how should we be handling errors?
       Could either follow the pattern in the rest of the package or
       start to use the condition system and raise specific conditions.
     */
    if(rc != SQLITE_OK) {
        error("SQL error: %s\n", sqlite3_errmsg(db_connection));
    }

    rc = sqlite3_step(stmt);
    col_type = sqlite3_column_type(stmt, 0);
    switch(col_type) {
    case SQLITE_INTEGER:
        PROTECT(ans = allocVector(INTSXP, numrows));
        intans = INTEGER(ans);
        break;
    case SQLITE_FLOAT:
        PROTECT(ans = allocVector(REALSXP, numrows));
        doubleans = REAL(ans);
        break;
    case SQLITE_TEXT:
        PROTECT(ans = allocVector(STRSXP, numrows));
        break;
    case SQLITE_NULL:
        error("RS_SQLite_quick_column: encountered NULL column");
        break;
    case SQLITE_BLOB: 
        error("RS_SQLite_quick_column: BLOB column handling not implementing");
        break;
    default:
        error("RS_SQLite_quick_column: unknown column type %d", col_type);
    }

    i = 0;
    while (rc == SQLITE_ROW && i < numrows) {
        switch (col_type) {
        case SQLITE_INTEGER:
            intans[i] = sqlite3_column_int(stmt, 0);
            break;
        case SQLITE_FLOAT:
            doubleans[i] = sqlite3_column_double(stmt, 0);
            break;
        case SQLITE_TEXT:
            SET_STRING_ELT(ans, i, mkChar(sqlite3_column_text(stmt, 0)));
        }
        i++;
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    UNPROTECT(1);
    return ans;
}


Res_Handle *
RS_SQLite_exec(Con_Handle *conHandle, s_object *statement,
               s_object *bind_data)
{
  S_EVALUATOR

  RS_DBI_connection *con;
  Res_Handle        *rsHandle;
  RS_DBI_resultSet  *res;
  sqlite3           *db_connection;
  sqlite3_stmt      *db_statement;
  int      state, bind_count;
  int      i, j, rows = 0, cols = 0;
  char     *dyn_statement;

  con = RS_DBI_getConnection(conHandle);
  db_connection = (sqlite3 *) con->drvConnection;
  dyn_statement = RS_DBI_copyString(CHR_EL(statement,0));

  /* Do we have a pending resultSet in the current connection?
   * SQLite only allows  one resultSet per connection.
   */
  if(con->num_res>0){
    Sint res_id = (Sint) con->resultSetIds[0]; /* recall, SQLite has only 1 res */
    rsHandle = RS_DBI_asResHandle(MGR_ID(conHandle),
                                  CON_ID(conHandle), res_id);
    res = RS_DBI_getResultSet(rsHandle);
    if(res->completed != 1){
      free(dyn_statement);
      RS_DBI_errorMessage(
        "connection with pending rows, close resultSet before continuing",
        RS_DBI_ERROR);
    }
    else
      RS_SQLite_closeResultSet(rsHandle);
  }

  /* allocate and init a new result set */

  rsHandle = RS_DBI_allocResultSet(conHandle);
  res = RS_DBI_getResultSet(rsHandle);
  res->completed = (Sint) 0;
  res->statement = dyn_statement;
  res->drvResultSet = NULL;

  state = sqlite3_prepare(db_connection, dyn_statement, -1,
                          &db_statement, NULL);

  if(state!=SQLITE_OK){
    char buf[2048];
    (void) sprintf(buf, "error in statement: %s",
                     sqlite3_errmsg(db_connection));

    RS_SQLite_setException(con, state, buf);
    RS_DBI_freeResultSet(rsHandle);
    RS_DBI_errorMessage(buf, RS_DBI_ERROR);
  }

  if(db_statement == NULL){
    char *message = "nothing to execute";
    RS_SQLite_setException(con, 0, message);
    RS_DBI_freeResultSet(rsHandle);
    RS_DBI_errorMessage(message, RS_DBI_ERROR);
  }
  res->drvResultSet = (void *) db_statement;

  bind_count = sqlite3_bind_parameter_count(db_statement);
  if (bind_count > 0 && bind_data != R_NilValue) {
      rows = GET_LENGTH(GET_ROWNAMES(bind_data));
      cols = GET_LENGTH(bind_data);
  }

  /* this will return 0 if the statement is not a SELECT */
  if(sqlite3_column_count(db_statement) > 0){
    if(bind_count > 0){
      char *message = "cannot have bound parameters on a SELECT statement";

      sqlite3_finalize(db_statement);
      res->drvResultSet = (void *)NULL;

      RS_SQLite_setException(con, 0, message);
      RS_DBI_freeResultSet(rsHandle);
      RS_DBI_errorMessage(message, RS_DBI_ERROR);
    }

    res->isSelect = (Sint) 1;           /* statement is a select  */
    res->rowCount = 0;                  /* fake's cursor's row count */
    res->rowsAffected = (Sint) -1;     /* no rows affected */
    res->fields = RS_SQLite_createDataMappings(rsHandle);

    RS_SQLite_setException(con, state, "OK");
  }
  else {
    /* if no bind parameters exist, we directly execute the query */
    if(bind_count == 0){
      state = corrected_sqlite3_step(db_statement);
      if(state!=SQLITE_DONE){
        char errMsg[2048];
        sprintf(errMsg, "RS_SQLite_exec: could not execute: %s",
                        sqlite3_errmsg(db_connection));

        RS_SQLite_setException(con, sqlite3_errcode(db_connection),
                               errMsg);

        sqlite3_finalize(db_statement);
        res->drvResultSet = (void *)NULL;
        RS_DBI_freeResultSet(rsHandle);
        RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
      }
    }
    else {
      char bindingErrorMsg[2048];

      /* get the binding parameter information */
      RS_SQLite_bindParam *params =
        RS_SQLite_createParameterBinding(bind_count, bind_data,
                                         db_statement, bindingErrorMsg);
      if(params == NULL){
        RS_SQLite_setException(con, -1, bindingErrorMsg);
        sqlite3_finalize(db_statement);
        res->drvResultSet = (void *)NULL;
        RS_DBI_freeResultSet(rsHandle);
        RS_DBI_errorMessage(bindingErrorMsg, RS_DBI_ERROR);
      }

      /* we need to step through the query for each row */
      for(i=0; i<rows; i++){

        /* bind each parameter to the statement */
        for(j=0; j<cols; j++){
          RS_SQLite_bindParam param = params[j];
          int integer;
          double number;
          char *string;

          switch(param.type){
            case INTEGER_TYPE:
              integer = INT_EL(param.data, i);
              if(IS_NA( &integer, INTEGER_TYPE ))
                state = sqlite3_bind_null(db_statement, j+1);
              else
                state = sqlite3_bind_int(db_statement, j+1, integer);
              break;

            case NUMERIC_TYPE:
              number = NUM_EL(param.data, i);
              if(IS_NA( &number, NUMERIC_TYPE ))
                state = sqlite3_bind_null(db_statement, j+1);
              else
                state = sqlite3_bind_double(db_statement, j+1, number);
              break;

            case CHARACTER_TYPE:
              /* falls through */
            default:
              string = CHR_EL(param.data, i);
              /* why does IS_NA for character crash? */
              if(strcmp(string, RS_NA_STRING) == 0)
                state = sqlite3_bind_null(db_statement, j+1);
              else
                state = sqlite3_bind_text(db_statement, j+1,
                                          string, -1, SQLITE_TRANSIENT);
              break;
          }
          if(state!=SQLITE_OK){
            char errMsg[2048];
            sprintf(errMsg, "RS_SQLite_exec: could not bind data: %s",
                            sqlite3_errmsg(db_connection));

            RS_SQLite_setException(con, sqlite3_errcode(db_connection),
                                   errMsg);

            RS_SQLite_freeParameterBinding(cols, params);

            sqlite3_finalize(db_statement);
            res->drvResultSet = (void *)NULL;
            RS_DBI_freeResultSet(rsHandle);
            RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
          }
        }

        /* execute the statement */
        state = corrected_sqlite3_step(db_statement);
        if(state!=SQLITE_DONE){
          char errMsg[2048];
          sprintf(errMsg, "RS_SQLite_exec: could not execute: %s",
                          sqlite3_errmsg(db_connection));
          RS_SQLite_setException(con, sqlite3_errcode(db_connection),
                                 errMsg);

          RS_SQLite_freeParameterBinding(cols, params);

          sqlite3_finalize(db_statement);
          res->drvResultSet = (void *)NULL;
          RS_DBI_freeResultSet(rsHandle);
          RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
        }

        /* reset the bind parameters */
        state = sqlite3_reset(db_statement);
        if(state!=SQLITE_OK){
          char errMsg[2048];
          sprintf(errMsg, "RS_SQLite_exec: could not reset statement: %s",
                          sqlite3_errmsg(db_connection));
          RS_SQLite_setException(con, sqlite3_errcode(db_connection),
                                 errMsg);

          RS_SQLite_freeParameterBinding(cols, params);

          sqlite3_finalize(db_statement);
          res->drvResultSet = (void *)NULL;
          RS_DBI_freeResultSet(rsHandle);
          RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
        }
      }

      /* free the binding parameter information */
      RS_SQLite_freeParameterBinding(cols, params);
    }

    res->isSelect  = (Sint) 0;          /* statement is not a select  */
    res->completed = (Sint) 1;          /* BUG: what if query is async?*/
    res->rowsAffected = (Sint) sqlite3_changes(db_connection);
    RS_SQLite_setException(con, state, "OK");
  }

  return rsHandle;
}

RS_SQLite_bindParam *
RS_SQLite_createParameterBinding(int n, s_object *bind_data,
                                 sqlite3_stmt *stmt, char *errorMsg)
{
  S_EVALUATOR

  RS_SQLite_bindParam *params;
  int i, j, *used_index, current;
  s_object *colNames, *data, *levels;

  /* check that we have enough columns in the data frame */
  colNames = GET_NAMES(bind_data);
  if(GET_LENGTH(colNames) < n){
    sprintf(errorMsg,
            "incomplete data binding: expected %d parameters, got %d",
            n, GET_LENGTH(colNames));
    return NULL;
  }

  /* allocate and initalize the structures*/
  params = (RS_SQLite_bindParam *)malloc(sizeof(RS_SQLite_bindParam) * n);
  if(params==NULL){
    sprintf(errorMsg, "could not allocate memory");
    return NULL;
  }

  used_index = (int *)malloc(sizeof(int)*n);
  if(used_index==NULL){
    free(params);
    sprintf(errorMsg, "could not allocate memory");
    return NULL;
  }

  for(i=0; i<n; i++){
    used_index[i] = -1;
    params[i].is_protected = 0;
    params[i].data = NULL;
  }

  for(i=0; i<n; i++){
    const char *paramName = sqlite3_bind_parameter_name(stmt, i+1);

    current = -1;
    if(paramName == NULL){
      /* assume the first non-used column is the one we want */
      for(j=0; j<n; j++){
        if(used_index[j] == -1){
          current = j;
          used_index[j] = 1;
          break;
        }
      }
    }
    else{
      for(j=0; j<LENGTH(colNames); j++){
        /* skip past initial bind identifier */
        if(strcmp(paramName+1, CHR_EL(colNames, j)) == 0){
          if(used_index[j] == -1){
            current = j;
            used_index[j] = 1;
            break;
          }
          /* it's already in use! throw an error */
          sprintf(errorMsg,
            "attempted to re-bind column [%s] to positional parameter %d",
            CHR_EL(colNames, j), i+1);
          free(params);
          free(used_index);
          return NULL;
        }
      }
    }

    if(current == -1){
      sprintf(errorMsg,
        "unable to bind data for positional parameter %d", i+1);
      free(params);
      free(used_index);
      return NULL;
    }

    data = LST_EL(bind_data, current);

    params[i].is_protected = 0;

    if(isInteger(data)){
      params[i].type = INTEGER_TYPE;
      params[i].data = data;
    }
    else if(isNumeric(data)){
      params[i].type = NUMERIC_TYPE;
      params[i].data = data;
    }
    else if(isString(data)){
      params[i].type = STRING_TYPE;
      params[i].data = data;
    }
    else if(isFactor(data)){
      /* need to convert to a string vector */
      params[i].type = STRING_TYPE;
      levels = GET_LEVELS(data);

      PROTECT( params[i].data = allocVector(STRING_TYPE, LENGTH(data)) );
      for(j=0; j<LENGTH(data); j++)
        SET_CHR_EL(params[i].data, j, STRING_ELT(levels, INT_EL(data, j)-1));

      params[i].is_protected = 1;
    }
    else{
      params[i].type = STRING_TYPE;
      PROTECT( params[i].data = AS_CHARACTER(data) );
      params[i].is_protected = 1;
    }
  }

  return params;
}

void
RS_SQLite_freeParameterBinding(int n, RS_SQLite_bindParam *params)
{
  S_EVALUATOR
  int i;

  for(i=0; i<n; i++){
    if(params[i].is_protected)
      UNPROTECT(1);
  }

  free(params);
}

RS_DBI_fields *
RS_SQLite_createDataMappings(Res_Handle *rsHandle)
{
  S_EVALUATOR

  sqlite3_stmt  *db_statement;
  RS_DBI_resultSet   *result;
  RS_DBI_fields      *flds;
  int     j, ncol;

  result = RS_DBI_getResultSet(rsHandle);
  db_statement = (sqlite3_stmt *) result->drvResultSet;

  ncol = sqlite3_column_count(db_statement);
  flds = RS_DBI_allocFields(ncol); /* BUG: mem leak if this fails? */
  flds->num_fields = (Sint) ncol;

   for(j=0; j<ncol; j++){
    flds->name[j] = RS_DBI_copyString(sqlite3_column_name(db_statement, j));

    /* interpret everything as a string */
    flds->type[j] = SQL92_TYPE_CHAR_VAR;
    flds->Sclass[j] = CHARACTER_TYPE;
    flds->length[j] = (Sint) -1;   /* unknown */
    flds->precision[j] = (Sint) -1;
    flds->scale[j] = (Sint) -1;
    flds->nullOk[j] = (Sint) -1;   /* actually we may be able to get(?) */
    flds->isVarLength[j] = (Sint) -1;
  }

  return flds;
}

/* we will return a data.frame with character data and then invoke
 * the .Internal(type.convert(...)) as in read.table in the
 * calling R/S function.  Grrr!
 */
s_object *      /* data.frame */
RS_SQLite_fetch(s_object *rsHandle, s_object *max_rec)
{
  S_EVALUATOR

  RS_DBI_resultSet *res;
  RS_DBI_fields    *flds;
  sqlite3_stmt     *db_statement;
  s_object  *output, *s_tmp;
  int    i, j, state, expand;
  Sint   num_rec;
  int    num_fields;

  res = RS_DBI_getResultSet(rsHandle);
  if(res->isSelect != 1){
    RS_DBI_errorMessage("resultSet does not correspond to a SELECT statement",
    RS_DBI_WARNING);
    return S_NULL_ENTRY;
  }

  if(res->completed == 1)
    return S_NULL_ENTRY;

  db_statement = (sqlite3_stmt *)res->drvResultSet;
  if(db_statement == NULL){
    RS_DBI_errorMessage("corrupt SQLite resultSet, missing statement handle",
      RS_DBI_ERROR);
  }

  flds = res->fields;
  if(!flds){
     RS_DBI_errorMessage("corrupt SQLite resultSet, missing fieldDescription",
      RS_DBI_ERROR);
  }

  num_fields = flds->num_fields;
  num_rec = INT_EL(max_rec,0);
  expand = (num_rec < 0);   /* dyn expand output to accommodate all rows*/
  if(expand || num_rec == 0){
    num_rec = RS_DBI_getManager(rsHandle)->fetch_default_rec;
  }

  MEM_PROTECT(output = NEW_LIST((Sint) num_fields));
  RS_DBI_allocOutput(output, flds, num_rec, 0);
#ifndef USING_R
  if(IS_LIST(output))
    output = AS_LIST(output);
  else
    RS_DBI_errorMessage("internal error: could not alloc output list",
      RS_DBI_ERROR);
#endif

  for(i = 0; ; i++){
    if(i==num_rec){  /* exhausted the allocated space */
      if(expand){    /* do we extend or return the records fetched so far*/
        num_rec = 2 * num_rec;
        RS_DBI_allocOutput(output, flds, num_rec, expand);
#ifndef USING_R
        if(IS_LIST(output))
          output = AS_LIST(output);
        else
          RS_DBI_errorMessage("internal error: could not alloc output list",
            RS_DBI_ERROR);
#endif
      }
      else
        break;       /* okay, no more fetching for now */
    }

    state = corrected_sqlite3_step(db_statement);
    if(state!=SQLITE_ROW && state!=SQLITE_DONE){
      char errMsg[2048];
      (void)sprintf(errMsg, "RS_SQLite_fetch: failed: %s",
                     sqlite3_errmsg(sqlite3_db_handle(db_statement)));
      RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
    }

    if(state==SQLITE_DONE){
      res->completed = (Sint) 1;
      break;
    }

    for(j = 0; j < num_fields; j++){
      int null_item = (sqlite3_column_type(db_statement, j) == SQLITE_NULL);

      switch(flds->Sclass[j]){
        case INTEGER_TYPE:
          if(null_item)
            NA_SET(&(LST_INT_EL(output,j,i)), INTEGER_TYPE);
          else
            LST_INT_EL(output,j,i) =
                      sqlite3_column_int(db_statement, j);
          break;
        case NUMERIC_TYPE:
          if(null_item)
            NA_SET(&(LST_NUM_EL(output,j,i)), NUMERIC_TYPE);
          else
            LST_NUM_EL(output,j,i) =
                      sqlite3_column_double(db_statement, j);
          break;
        case CHARACTER_TYPE:
          /* falls through */
        default:
          if(null_item)
            SET_LST_CHR_EL(output,j,i, NA_STRING);
          else
            SET_LST_CHR_EL(output,j,i,
                           C_S_CPY(sqlite3_column_text(db_statement, j)));
          break;
      }
    } /* end column loop */
  } /* end row loop */

  /* size to actual number of records fetched */
  if(i < num_rec){
    num_rec = i;
    /* adjust the length of each of the members in the output_list */
    for(j = 0; j<num_fields; j++){
      s_tmp = LST_EL(output,j);
      MEM_PROTECT(SET_LENGTH(s_tmp, num_rec));
      SET_ELEMENT(output, j, s_tmp);
      MEM_UNPROTECT(1);
    }
  }

  res->rowCount += num_rec;

  MEM_UNPROTECT(1);
  return output;
}

s_object* RS_SQLite_mget(s_object *rsHandle, s_object *max_rec)
{
    RS_DBI_resultSet *res;
    RS_DBI_fields    *flds;
    sqlite3_stmt     *db_statement;
    s_object  *s_tmp;
    SEXP output, v_tmp;
    int    i, j, state, expand, vec_i;
    Sint   num_rec;
    int    num_fields, null_item;
    const char *cur_key = NULL;
    char *prev_key = NULL;
    s_object *cur_vect = R_NilValue;

    res = RS_DBI_getResultSet(rsHandle);
    if(res->isSelect != 1){
        RS_DBI_errorMessage("resultSet does not correspond to a SELECT statement",
                            RS_DBI_WARNING);
        return S_NULL_ENTRY;
    }

    if(res->completed == 1)
        return S_NULL_ENTRY;

    db_statement = (sqlite3_stmt *)res->drvResultSet;
    if(db_statement == NULL){
        RS_DBI_errorMessage("corrupt SQLite resultSet, missing statement handle",
                            RS_DBI_ERROR);
    }

    flds = res->fields;
    if(!flds){
        RS_DBI_errorMessage("corrupt SQLite resultSet, missing fieldDescription",
                            RS_DBI_ERROR);
    }

    num_fields = flds->num_fields;
    num_rec = INT_EL(max_rec,0);
    expand = (num_rec < 0);   /* dyn expand output to accommodate all rows*/
    if(expand || num_rec == 0){
        num_rec = RS_DBI_getManager(rsHandle)->fetch_default_rec;
    }

    PROTECT(output = R_NewHashedEnv(R_NilValue));
    for (i = 0; ; i++) {
        if (i == num_rec) {  /* exhausted the allocated space */
            if (expand) {    /* do we extend or return the records fetched so far*/
                num_rec = 2 * num_rec;
                UNPROTECT(1);
                PROTECT(SET_LENGTH(cur_vect, num_rec));
            }
            else
                break;       /* okay, no more fetching for now */
        }
        state = corrected_sqlite3_step(db_statement);
        if (state != SQLITE_ROW && state != SQLITE_DONE) {
            char errMsg[2048];
            (void)sprintf(errMsg, "RS_SQLite_fetch: failed: %s",
                          sqlite3_errmsg(sqlite3_db_handle(db_statement)));
            RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
        }
        if (state == SQLITE_DONE) {
            res->completed = (Sint) 1;
            break;
        }

        cur_key = sqlite3_column_text(db_statement, 0);
        if (prev_key == NULL || strcmp(prev_key, cur_key) != 0) {
            if (cur_vect != R_NilValue) {
                UNPROTECT(1);
                PROTECT(SET_LENGTH(cur_vect, vec_i));
                defineVar(install(prev_key), cur_vect, output);                
                UNPROTECT(1);
            }
            switch (flds->Sclass[1]) {
            case INTEGER_TYPE:
                cur_vect = PROTECT(allocVector(INTSXP, num_rec));
                break;
            case NUMERIC_TYPE:
                cur_vect = PROTECT(allocVector(REALSXP, num_rec));
                break;
            case CHARACTER_TYPE: /* falls through */
            default:
                cur_vect = PROTECT(allocVector(STRSXP, num_rec));
                break;
            }
            if (prev_key != NULL)
                free(prev_key);
            prev_key = (char *)malloc(sizeof(char)*strlen(cur_key) + 1);
            strcpy(prev_key, cur_key);
            defineVar(install(cur_key), cur_vect, output);
            vec_i = 0;
        }
        null_item = (sqlite3_column_type(db_statement, 1) == SQLITE_NULL);
        switch (flds->Sclass[1]) {
        case INTEGER_TYPE:
            if(null_item)
                INTEGER(cur_vect)[vec_i++] = NA_INTEGER;
            else
                INTEGER(cur_vect)[vec_i++] = sqlite3_column_int(db_statement, 1);
            break;
        case NUMERIC_TYPE:
            if(null_item)
                REAL(cur_vect)[vec_i++] = NA_REAL;
            else
                REAL(cur_vect)[vec_i++] = sqlite3_column_double(db_statement, 1);
            break;
        case CHARACTER_TYPE:
            /* falls through */
        default:
            if(null_item)
                SET_STRING_ELT(cur_vect, vec_i++, NA_STRING);
            else
                SET_STRING_ELT(cur_vect, vec_i++, 
                               mkChar(sqlite3_column_text(db_statement, 1)));
            break;
        }
    } /* end row loop */

#if 0
    /* size to actual number of records fetched */
    if(i < num_rec){
        num_rec = i;
        /* adjust the length of each of the members in the output_list */
        for(j = 0; j<num_fields; j++){
            s_tmp = LST_EL(output,j);
            MEM_PROTECT(SET_LENGTH(s_tmp, num_rec));
            SET_ELEMENT(output, j, s_tmp);
            MEM_UNPROTECT(1);
        }
    }
#endif
    res->rowCount += num_rec;

    if (prev_key != NULL)
        free(prev_key);

    MEM_UNPROTECT(2);
    return output;
}

/* return a 2-elem list with the last exception number and exception message on a given connection.
 * NOTE: RS_SQLite_getException() is meant to be used mostly directory R.
 */
s_object *
RS_SQLite_getException(s_object *conHandle)
{
  S_EVALUATOR

  s_object  *output;
  RS_DBI_connection   *con;
  RS_SQLite_exception *err;
  Sint  n = 2;
  char *exDesc[] = {"errorNum", "errorMsg"};
  Stype exType[] = {INTEGER_TYPE, CHARACTER_TYPE};
  Sint  exLen[]  = {1, 1};

  con = RS_DBI_getConnection(conHandle);
  if(!con->drvConnection)
    RS_DBI_errorMessage("internal error: corrupt connection handle",
      RS_DBI_ERROR);
  output = RS_DBI_createNamedList(exDesc, exType, exLen, n);
#ifndef USING_R
  if(IS_LIST(output))
    output = AS_LIST(output);
  else
    RS_DBI_errorMessage("internal error: could not allocate named list",
      RS_DBI_ERROR);
#endif
  err = (RS_SQLite_exception *) con->drvData;
  LST_INT_EL(output,0,0) = (Sint) err->errorNum;
  SET_LST_CHR_EL(output,1,0,C_S_CPY(err->errorMsg));

  return output;
}

s_object *
RS_SQLite_closeResultSet(s_object *resHandle)
{
  S_EVALUATOR

  sqlite3_stmt     *db_statement;
  RS_DBI_resultSet *result;
  s_object *status;

  result = RS_DBI_getResultSet(resHandle);
  db_statement = (sqlite3_stmt *)result->drvResultSet;
  if(db_statement == NULL)
    RS_DBI_errorMessage("corrupt SQLite resultSet, missing statement handle",
      RS_DBI_ERROR);

  sqlite3_finalize(db_statement);

  result->drvResultSet = (void *) NULL;
  /* need to NULL drvResultSet, otherwise can't free the rsHandle */
  if(result->drvData)
     free(result->drvData);
  result->drvData = (void *) NULL;
  RS_DBI_freeResultSet(resHandle);

  MEM_PROTECT(status = NEW_LOGICAL((Sint) 1));
  LGL_EL(status, 0) = TRUE;
  MEM_UNPROTECT(1);

  return status;
}

s_object *
RS_SQLite_managerInfo(Mgr_Handle *mgrHandle)
{
  S_EVALUATOR

  RS_DBI_manager *mgr;
  s_object *output;
  Sint i, num_con, max_con, *cons, ncon, *shared_cache;
  Sint j, n = 9;
  char *mgrDesc[] = {"drvName",   "connectionIds", "fetch_default_rec",
                     "managerId", "length",        "num_con",
                     "counter",   "clientVersion", "shared_cache"};
  Stype mgrType[] = {CHARACTER_TYPE, INTEGER_TYPE, INTEGER_TYPE,
                     INTEGER_TYPE,   INTEGER_TYPE, INTEGER_TYPE,
                     INTEGER_TYPE,   CHARACTER_TYPE, CHARACTER_TYPE };
  Sint  mgrLen[]  = {1, 1, 1, 1, 1, 1, 1, 1, 1};

  mgr = RS_DBI_getManager(mgrHandle);
  if(!mgr)
    RS_DBI_errorMessage("driver not loaded yet", RS_DBI_ERROR);
  num_con = (Sint) mgr->num_con;
  max_con = (Sint) mgr->length;
  shared_cache = (Sint *) mgr->drvData;
  mgrLen[1] = num_con;

  output = RS_DBI_createNamedList(mgrDesc, mgrType, mgrLen, n);
  if(IS_LIST(output))
    output = AS_LIST(output);
  else
    RS_DBI_errorMessage("internal error: could not alloc named list",
      RS_DBI_ERROR);
  j = (Sint) 0;
  if(mgr->drvName)
    SET_LST_CHR_EL(output,j++,0,C_S_CPY(mgr->drvName));
  else
    SET_LST_CHR_EL(output,j++,0,C_S_CPY(""));

  cons = (Sint *) S_alloc((long)max_con, (int)sizeof(Sint));
  ncon = RS_DBI_listEntries(mgr->connectionIds, mgr->length, cons);
  if(ncon != num_con){
    RS_DBI_errorMessage(
    "internal error: corrupt RS_DBI connection table",
    RS_DBI_ERROR);
  }
  for(i = 0; i < num_con; i++)
    LST_INT_EL(output, j, i) = cons[i];
  j++;
  LST_INT_EL(output,j++,0) = mgr->fetch_default_rec;
  LST_INT_EL(output,j++,0) = mgr->managerId;
  LST_INT_EL(output,j++,0) = mgr->length;
  LST_INT_EL(output,j++,0) = mgr->num_con;
  LST_INT_EL(output,j++,0) = mgr->counter;
  SET_LST_CHR_EL(output,j++,0,C_S_CPY(SQLITE_VERSION));
  if(*shared_cache)
    SET_LST_CHR_EL(output,j++,0,C_S_CPY("on"));
  else
    SET_LST_CHR_EL(output,j++,0,C_S_CPY("off"));

  return output;
}

s_object *
RS_SQLite_connectionInfo(Con_Handle *conHandle)
{
  S_EVALUATOR

  RS_SQLite_conParams *conParams;
  RS_DBI_connection  *con;
  s_object   *output;
  Sint       i, n = 8, *res, nres;
  char *conDesc[] = {"host", "user", "dbname", "conType",
             "serverVersion", "threadId", "rsId", "loadableExtensions"};
  Stype conType[] = {CHARACTER_TYPE, CHARACTER_TYPE, CHARACTER_TYPE,
          CHARACTER_TYPE, CHARACTER_TYPE,
              INTEGER_TYPE, INTEGER_TYPE, CHARACTER_TYPE};
  Sint  conLen[]  = {1, 1, 1, 1, 1, 1, 1, 1};

  con = RS_DBI_getConnection(conHandle);
  conLen[6] = con->num_res;         /* num of open resultSets */
  output = RS_DBI_createNamedList(conDesc, conType, conLen, n);
#ifndef USING_R
  if(IS_LIST(output))
    output = AS_LIST(output);
  else
    RS_DBI_errorMessage("internal error: could not alloc named list",
      RS_DBI_ERROR);
#endif
  conParams = (RS_SQLite_conParams *) con->conParams;
  SET_LST_CHR_EL(output,0,0,C_S_CPY("localhost"));
  SET_LST_CHR_EL(output,1,0,C_S_CPY(RS_NA_STRING));
  SET_LST_CHR_EL(output,2,0,C_S_CPY(conParams->dbname));
  SET_LST_CHR_EL(output,3,0,C_S_CPY("direct"));
  SET_LST_CHR_EL(output,4,0,C_S_CPY(SQLITE_VERSION));

  LST_INT_EL(output,5,0) = (Sint) -1;

  res = (Sint *) S_alloc( (long) con->length, (int) sizeof(Sint));
  nres = RS_DBI_listEntries(con->resultSetIds, con->length, res);
  if(nres != con->num_res){
    RS_DBI_errorMessage(
    "internal error: corrupt RS_DBI resultSet table",
    RS_DBI_ERROR);
  }
  for( i = 0; i < con->num_res; i++){
    LST_INT_EL(output,6,i) = (Sint) res[i];
  }

  if(conParams->loadable_extensions)
    SET_LST_CHR_EL(output,7,0,C_S_CPY("on"));
  else
    SET_LST_CHR_EL(output,7,0,C_S_CPY("off"));

  return output;
}
s_object *
RS_SQLite_resultSetInfo(Res_Handle *rsHandle)
{
  S_EVALUATOR

  RS_DBI_resultSet   *result;
  s_object  *output, *flds;
  Sint  n = 6;
  char  *rsDesc[] = {"statement", "isSelect", "rowsAffected",
         "rowCount", "completed", "fieldDescription"};
  Stype rsType[]  = {CHARACTER_TYPE, INTEGER_TYPE, INTEGER_TYPE,
         INTEGER_TYPE,   INTEGER_TYPE, LIST_TYPE};
  Sint  rsLen[]   = {1, 1, 1, 1, 1, 1};

  result = RS_DBI_getResultSet(rsHandle);
  if(result->fields)
      PROTECT(flds = RS_DBI_getFieldDescriptions(result->fields));
  else
      PROTECT(flds = S_NULL_ENTRY);

  PROTECT(output = RS_DBI_createNamedList(rsDesc, rsType, rsLen, n));
  SET_LST_CHR_EL(output,0,0,C_S_CPY(result->statement));
  LST_INT_EL(output,1,0) = result->isSelect;
  LST_INT_EL(output,2,0) = result->rowsAffected;
  LST_INT_EL(output,3,0) = result->rowCount;
  LST_INT_EL(output,4,0) = result->completed;
  if(flds != S_NULL_ENTRY)
     SET_ELEMENT(LST_EL(output, 5), (Sint) 0, flds);

  UNPROTECT(2);
  return output;
}

s_object *
RS_SQLite_typeNames(s_object *typeIds)
{
  s_object *typeNames;
  Sint n;
  Sint *typeCodes;
  int i;
  char *s;

  n = LENGTH(typeIds);
  typeCodes = INTEGER_DATA(typeIds);
  MEM_PROTECT(typeNames = NEW_CHARACTER(n));
  for(i = 0; i < n; i++) {
    s = RS_DBI_getTypeName(typeCodes[i], RS_SQLite_fieldTypes);
    SET_CHR_EL(typeNames, i, C_S_CPY(s));
  }
  MEM_UNPROTECT(1);
  return typeNames;
}

s_object *    /* returns TRUE/FALSE */
RS_SQLite_importFile(
  Con_Handle *conHandle,
  s_object *s_tablename,
  s_object *s_filename,
  s_object *s_separator,
  s_object *s_eol,
  s_object *s_skip
)
{
  S_EVALUATOR

  RS_DBI_connection *con;
  sqlite3           *db_connection;
  char              *zFile, *zTable, *zSep, *s, *s1, *zEol;
  Sint              rc, skip;
  s_object          *output;


  s = CHR_EL(s_tablename, 0);
  zTable = (char *) malloc( strlen(s)+1);
  if(!zTable){
    RS_DBI_errorMessage("could not allocate memory", RS_DBI_ERROR);
  }
  (void) strcpy(zTable, s);

  s = CHR_EL(s_filename, 0);
  zFile = (char *) malloc( strlen(s)+1);
  if(!zFile){
    free(zTable);
    RS_DBI_errorMessage("could not allocate memory", RS_DBI_ERROR);
  }
  (void) strcpy(zFile, s);

  s = CHR_EL(s_separator, 0);
  s1 = CHR_EL(s_eol, 0);
  zSep = (char *) malloc( strlen(s)+1);
  zEol = (char *) malloc(strlen(s1)+1);
  if(!zSep || !zEol){
    free(zTable);
    free(zFile);
    if(zSep) free(zSep);
    if(zEol) free(zEol);
    RS_DBI_errorMessage("could not allocate memory", RS_DBI_ERROR);
  }
  (void) strcpy(zSep, s);
  (void) strcpy(zEol, s1);

  skip = (Sint) INT_EL(s_skip, 0);

  con = RS_DBI_getConnection(conHandle);
  db_connection = (sqlite3 *) con->drvConnection;

  rc = RS_sqlite_import(db_connection, zTable, zFile, zSep, zEol, skip);

  free(zTable);
  free(zFile);
  free(zSep);

  MEM_PROTECT(output = NEW_LOGICAL((Sint) 1));
  LOGICAL_POINTER(output)[0] = rc;
  MEM_UNPROTECT(1);
  return output;
}

/* The following code comes directly from SQLite's shell.c, with
 * obvious minor changes.
 */
int
RS_sqlite_import(
   sqlite3 *db,
   const char *zTable,          /* table must already exist */
   const char *zFile,
   const char *separator,
   const char *eol,
   Sint skip
)
{
    sqlite3_stmt *pStmt;        /* A statement */
    int rc;                     /* Result code */
    int nCol;                   /* Number of columns in the table */
    int nByte;                  /* Number of bytes in an SQL string */
    int i, j;                   /* Loop counters */
    int nSep;                   /* Number of bytes in separator[] */
    char *zSql;                 /* An SQL statement */
    char *zLine = NULL;         /* A single line of input from the file */
    char **azCol;               /* zLine[] broken up into columns */
    char *zCommit;              /* How to commit changes */
    FILE *in;                   /* The input file */
    int lineno = 0;             /* Line number of input file */

    char *z;

    nSep = strlen(separator);
    if( nSep==0 ){
      RS_DBI_errorMessage(
         "RS_sqlite_import: non-null separator required for import",
         RS_DBI_ERROR);
    }
    zSql = sqlite3_mprintf("SELECT * FROM '%q'", zTable);
    if( zSql==0 ) return 0;
    nByte = strlen(zSql);
    rc = sqlite3_prepare(db, zSql, -1, &pStmt, 0);
    sqlite3_free(zSql);
    if( rc ){
      char errMsg[512];
      (void) sprintf(errMsg, "RS_sqlite_import: %s", sqlite3_errmsg(db));
      RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
      nCol = 0;
    }else{
      nCol = sqlite3_column_count(pStmt);
    }
    sqlite3_finalize(pStmt);
    if( nCol==0 ) return 0;
    zSql = malloc( nByte + 20 + nCol*2 );
    if( zSql==0 ) return 0;
    sqlite3_snprintf(nByte+20, zSql, "INSERT INTO '%q' VALUES(?", zTable);
    j = strlen(zSql);
    for(i=1; i<nCol; i++){
      zSql[j++] = ',';
      zSql[j++] = '?';
    }
    zSql[j++] = ')';
    zSql[j] = 0;
    rc = sqlite3_prepare(db, zSql, -1, &pStmt, 0);
    free(zSql);
    if( rc ){
      char errMsg[512];
      (void) sprintf(errMsg, "RS_sqlite_import: %s", sqlite3_errmsg(db));
      RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
      sqlite3_finalize(pStmt);
      return 0;
    }
    in = fopen(zFile, "rb");
    if( in==0 ){
      char errMsg[512];
      (void) sprintf(errMsg, "RS_sqlite_import: cannot open file %s", zFile);
      RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
      sqlite3_finalize(pStmt);
      return 0;
    }
    azCol = malloc( sizeof(azCol[0])*(nCol+1) );
    if( azCol==0 ) return 0;
    sqlite3_exec(db, "BEGIN", 0, 0, 0);
    zCommit = "COMMIT";
    while( (zLine = RS_sqlite_getline(in, eol)) != NULL){
      lineno++;
      if(lineno <= skip) continue;
      i = 0;
      azCol[0] = zLine;
      for(i=0, z=zLine; *z && *z!='\n' && *z!='\r'; z++){
        if( *z==separator[0] && strncmp(z, separator, nSep)==0 ){
          *z = 0;
          i++;
          if( i<nCol ){
            azCol[i] = &z[nSep];
            z += nSep-1;
          }
        }
      }
      if( i+1!=nCol ){
        char errMsg[512];
        (void) sprintf(errMsg,
               "RS_sqlite_import: %s line %d expected %d columns of data but found %d",
               zFile, lineno, nCol, i+1);
        RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
        zCommit = "ROLLBACK";
        break;
      }

      for(i=0; i<nCol; i++){
        if(azCol[i][0]=='\\' && azCol[i][1]=='N'){   /* insert NULL for NA */
          sqlite3_bind_null(pStmt, i+1);
        }
        else {
          sqlite3_bind_text(pStmt, i+1, azCol[i], -1, SQLITE_STATIC);
        }
      }

      if(corrected_sqlite3_step(pStmt)!=SQLITE_DONE){
        char errMsg[512];
        (void) sprintf(errMsg,
                 "RS_sqlite_import: internal error: sqlite3_step() filed");
        RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
      }
      rc = sqlite3_reset(pStmt);
      free(zLine);
      zLine = NULL;
      if( rc!=SQLITE_OK ){
        char errMsg[512];
        (void) sprintf(errMsg,"RS_sqlite_import: %s", sqlite3_errmsg(db));
        RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
        zCommit = "ROLLBACK";
        break;
      }
    }
    free(azCol);
    fclose(in);
    sqlite3_finalize(pStmt);
    sqlite3_exec(db, zCommit, 0, 0, 0);
    return 1;
}

/* the following is only needed (?) on windows (getline is a GNU extension
 * and it gave me problems with minGW).  Note that we drop the (UNIX)
 * new line character.  The R function safe.write() explicitly uses
 * eol = '\n' even on Windows.
 */

char *
RS_sqlite_getline(FILE *in, const char *eol)
{
   /* caller must free memory */
   char   *buf, ceol;
   size_t nc, i, neol;
   int    c;

   nc = 1024; i = 0;
   buf = (char *) malloc(nc);
   if(!buf)
      RS_DBI_errorMessage("RS_sqlite_getline could not malloc", RS_DBI_ERROR);

   neol = strlen(eol);  /* num of eol chars */
   ceol = eol[neol-1];  /* last char in eol */
   while(TRUE){
      c=fgetc(in);
      if(i==nc){
        nc = 2 * nc;
        buf = (char *) realloc((void *) buf, nc);
        if(!buf)
          RS_DBI_errorMessage(
             "RS_sqlite_getline could not realloc", RS_DBI_ERROR);
      }
      if(c==EOF)
        break;
      buf[i++] = c;
      if(c==ceol){           /* '\n'){ */
        buf[i-neol] = '\0';   /* drop the newline char(s) */
        break;
      }
    }

    if(i==0){              /* empty line */
      free(buf);
      buf = (char *) NULL;
    }

    return buf;
}

/* from http://www.sqlite.org/capi3ref.html#sqlite3_step */
int corrected_sqlite3_step(sqlite3_stmt *pStatement){
  int rc;
  rc = sqlite3_step(pStatement);
  if( rc==SQLITE_ERROR ){
    rc = sqlite3_reset(pStatement);
  }
  return rc;
}
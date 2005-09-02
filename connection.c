/*------
 * Module:			connection.c
 *
 * Description:		This module contains routines related to
 *					connecting to and disconnecting from the Postgres DBMS.
 *
 * Classes:			ConnectionClass (Functions prefix: "CC_")
 *
 * API functions:	SQLAllocConnect, SQLConnect, SQLDisconnect, SQLFreeConnect,
 *					SQLBrowseConnect(NI)
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *-------
 */
/* Multibyte support	Eiji Tokuya 2001-03-15 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#ifndef	WIN32
#include <errno.h>
#endif /* WIN32 */

#include "environ.h"
#include "statement.h"
#include "qresult.h"
#include "lobj.h"
#include "dlg_specific.h"

#include "multibyte.h"

#include "pgapifunc.h"
#include "md5.h"

#define STMT_INCREMENT 16		/* how many statement holders to allocate
								 * at a time */

#define PRN_NULLCHECK

extern GLOBAL_VALUES globals;

#ifdef USE_LIBPQ
#include "libpqconnection.h"
#include "pgtypes.h"
#include <libpq-fe.h>
#else
#include "connection.h"
#include "socket.h"
#endif /* USE_LIBPQ */


RETCODE		SQL_API
PGAPI_AllocConnect(
				   HENV henv,
				   HDBC FAR * phdbc)
{
	EnvironmentClass *env = (EnvironmentClass *) henv;
	ConnectionClass *conn;
	CSTR func = "PGAPI_AllocConnect";

	mylog("%s: entering...\n", func);

	conn = CC_Constructor();
	mylog("**** %s: henv = %u, conn = %u\n", func, henv, conn);

	if (!conn)
	{
		env->errormsg = "Couldn't allocate memory for Connection object.";
		env->errornumber = ENV_ALLOC_ERROR;
		*phdbc = SQL_NULL_HDBC;
		EN_log_error(func, "", env);
		return SQL_ERROR;
	}

	if (!EN_add_connection(env, conn))
	{
		env->errormsg = "Maximum number of connections exceeded.";
		env->errornumber = ENV_ALLOC_ERROR;
		CC_Destructor(conn);
		*phdbc = SQL_NULL_HDBC;
		EN_log_error(func, "", env);
		return SQL_ERROR;
	}

	if (phdbc)
		*phdbc = (HDBC) conn;

	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_Connect(
			  HDBC hdbc,
			  UCHAR FAR * szDSN,
			  SWORD cbDSN,
			  UCHAR FAR * szUID,
			  SWORD cbUID,
			  UCHAR FAR * szAuthStr,
			  SWORD cbAuthStr)
{
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	ConnInfo   *ci;
	CSTR func = "PGAPI_Connect";

	mylog("%s: entering...\n", func);

	if (!conn)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	ci = &conn->connInfo;

	make_string(szDSN, cbDSN, ci->dsn, sizeof(ci->dsn));

	/* get the values for the DSN from the registry */
	memcpy(&ci->drivers, &globals, sizeof(globals));
	getDSNinfo(ci, CONN_OVERWRITE);
	logs_on_off(1, ci->drivers.debug, ci->drivers.commlog);
	/* initialize pg_version from connInfo.protocol    */
	CC_initialize_pg_version(conn);

	/*
	 * override values from DSN info with UID and authStr(pwd) This only
	 * occurs if the values are actually there.
	 */
	make_string(szUID, cbUID, ci->username, sizeof(ci->username));
	make_string(szAuthStr, cbAuthStr, ci->password, sizeof(ci->password));

	/* fill in any defaults */
	getDSNdefaults(ci);

	qlog("conn = %u, %s(DSN='%s', UID='%s', PWD='%s')\n", conn, func, ci->dsn, ci->username, ci->password ? "xxxxx" : "");

	if (CC_connect(conn, AUTH_REQ_OK, NULL) <= 0)
	{
		/* Error messages are filled in */
		CC_log_error(func, "Error on CC_connect", conn);
		return SQL_ERROR;
	}

	mylog("%s: returning...\n", func);

	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_BrowseConnect(
					HDBC hdbc,
					UCHAR FAR * szConnStrIn,
					SWORD cbConnStrIn,
					UCHAR FAR * szConnStrOut,
					SWORD cbConnStrOutMax,
					SWORD FAR * pcbConnStrOut)
{
	CSTR func = "PGAPI_BrowseConnect";
	ConnectionClass *conn = (ConnectionClass *) hdbc;

	mylog("%s: entering...\n", func);

	CC_set_error(conn, CONN_NOT_IMPLEMENTED_ERROR, "not implemented");
	CC_log_error(func, "Function not implemented", conn);
	return SQL_ERROR;
}


/* Drop any hstmts open on hdbc and disconnect from database */
RETCODE		SQL_API
PGAPI_Disconnect(
				 HDBC hdbc)
{
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	CSTR func = "PGAPI_Disconnect";


	mylog("%s: entering...\n", func);

	if (!conn)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	qlog("conn=%u, %s\n", conn, func);

	if (conn->status == CONN_EXECUTING)
	{
		CC_set_error(conn, CONN_IN_USE, "A transaction is currently being executed");
		CC_log_error(func, "", conn);
		return SQL_ERROR;
	}

	logs_on_off(-1, conn->connInfo.drivers.debug, conn->connInfo.drivers.commlog);
	mylog("%s: about to CC_cleanup\n", func);

	/* Close the connection and free statements */
	CC_cleanup(conn);

	mylog("%s: done CC_cleanup\n", func);
	mylog("%s: returning...\n", func);

	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_FreeConnect(
				  HDBC hdbc)
{
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	CSTR func = "PGAPI_FreeConnect";

	mylog("%s: entering...\n", func);
	mylog("**** in %s: hdbc=%u\n", func, hdbc);

	if (!conn)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	/* Remove the connection from the environment */
	if (!EN_remove_connection(conn->henv, conn))
	{
		CC_set_error(conn, CONN_IN_USE, "A transaction is currently being executed");
		CC_log_error(func, "", conn);
		return SQL_ERROR;
	}

	CC_Destructor(conn);

	mylog("%s: returning...\n", func);

	return SQL_SUCCESS;
}


void
CC_conninfo_init(ConnInfo *conninfo)
{
		memset(conninfo, 0, sizeof(ConnInfo));
		conninfo->disallow_premature = -1;
		conninfo->allow_keyset = -1;
		conninfo->lf_conversion = -1;
		conninfo->true_is_minus1 = -1;
		conninfo->int8_as = -101;
		conninfo->bytea_as_longvarbinary = -1;
		conninfo->use_server_side_prepare = -1;
		memcpy(&(conninfo->drivers), &globals, sizeof(globals));
}


/*	Return how many cursors are opened on this connection */
int
CC_cursor_count(ConnectionClass *self)
{
	StatementClass *stmt;
	int			i,
				count = 0;

	mylog("CC_cursor_count: self=%u, num_stmts=%d\n", self, self->num_stmts);

	for (i = 0; i < self->num_stmts; i++)
	{
		stmt = self->stmts[i];
		if (stmt && SC_get_Result(stmt) && SC_get_Result(stmt)->cursor)
			count++;
	}

	mylog("CC_cursor_count: returning %d\n", count);

	return count;
}


void
CC_clear_error(ConnectionClass *self)
{
	self->__error_number = 0;
	if (self->__error_message)
		free(self->__error_message);
	self->__error_message = NULL;
	self->errormsg_created = FALSE;
}


/*
 *	Used to begin a transaction.
 */
char
CC_begin(ConnectionClass *self)
{
	char	ret = TRUE;
	if (!CC_is_in_trans(self))
	{
		QResultClass *res = CC_send_query(self, "BEGIN", NULL, CLEAR_RESULT_ON_ABORT);
		mylog("CC_begin:  sending BEGIN!\n");

		if (res != NULL)
		{
			ret = QR_command_maybe_successful(res);
			QR_Destructor(res);
		}
		else
			return FALSE;
	}

	return ret;
}

/*
 *	Used to commit a transaction.
 *	We are almost always in the middle of a transaction.
 */
char
CC_commit(ConnectionClass *self)
{
	char	ret = FALSE;
	if (CC_is_in_trans(self))
	{
		QResultClass *res = CC_send_query(self, "COMMIT", NULL, CLEAR_RESULT_ON_ABORT);
		mylog("CC_commit:  sending COMMIT!\n");
		if (res != NULL)
		{
			ret = QR_command_maybe_successful(res);
			QR_Destructor(res);
		}
		else
			return FALSE;
	}

	return ret;
}

/*
 *	Used to cancel a transaction.
 *	We are almost always in the middle of a transaction.
 */
char
CC_abort(ConnectionClass *self)
{
	if (CC_is_in_trans(self))
	{
		QResultClass *res = CC_send_query(self, "ROLLBACK", NULL, CLEAR_RESULT_ON_ABORT);
		mylog("CC_abort:  sending ABORT!\n");
		if (res != NULL)
			QR_Destructor(res);
		else
			return FALSE;
	}

	return TRUE;
}



int
CC_set_translation(ConnectionClass *self)
{

#ifdef WIN32

	if (self->translation_handle != NULL)
	{
		FreeLibrary(self->translation_handle);
		self->translation_handle = NULL;
	}

	if (self->connInfo.translation_dll[0] == 0)
		return TRUE;

	self->translation_option = atoi(self->connInfo.translation_option);
	self->translation_handle = LoadLibrary(self->connInfo.translation_dll);

	if (self->translation_handle == NULL)
	{
		CC_set_error(self, CONN_UNABLE_TO_LOAD_DLL, "Could not load the translation DLL.");
		return FALSE;
	}

	self->DataSourceToDriver
		= (DataSourceToDriverProc) GetProcAddress(self->translation_handle,
												"SQLDataSourceToDriver");

	self->DriverToDataSource
		= (DriverToDataSourceProc) GetProcAddress(self->translation_handle,
												"SQLDriverToDataSource");

	if (self->DataSourceToDriver == NULL || self->DriverToDataSource == NULL)
	{
		CC_set_error(self, CONN_UNABLE_TO_LOAD_DLL, "Could not find translation DLL functions.");
		return FALSE;
	}
#endif
	return TRUE;
}


char
CC_add_statement(ConnectionClass *self, StatementClass *stmt)
{
	int			i;

	mylog("CC_add_statement: self=%u, stmt=%u\n", self, stmt);

	for (i = 0; i < self->num_stmts; i++)
	{
		if (!self->stmts[i])
		{
			stmt->hdbc = self;
			self->stmts[i] = stmt;
			return TRUE;
		}
	}

	/* no more room -- allocate more memory */
	self->stmts = (StatementClass **) realloc(self->stmts, sizeof(StatementClass *) * (STMT_INCREMENT + self->num_stmts));
	if (!self->stmts)
		return FALSE;

	memset(&self->stmts[self->num_stmts], 0, sizeof(StatementClass *) * STMT_INCREMENT);

	stmt->hdbc = self;
	self->stmts[self->num_stmts] = stmt;

	self->num_stmts += STMT_INCREMENT;

	return TRUE;
}


char
CC_remove_statement(ConnectionClass *self, StatementClass *stmt)
{
	int			i;

	if (stmt->status == STMT_EXECUTING)
		return FALSE;
	for (i = 0; i < self->num_stmts; i++)
	{
		if (self->stmts[i] == stmt)
		{
			self->stmts[i] = NULL;
			return TRUE;
		}
	}

	return FALSE;
}


void
CC_set_error(ConnectionClass *self, int number, const char *message)
{
	if (self->__error_message)
		free(self->__error_message);
	self->__error_number = number;
	self->__error_message = message ? strdup(message) : NULL;
}


void
CC_set_errormsg(ConnectionClass *self, const char *message)
{
	if (self->__error_message)
		free(self->__error_message);
	self->__error_message = message ? strdup(message) : NULL;
}


char
CC_get_error(ConnectionClass *self, int *number, char **message)
{
	int			rv;
	char *msgcrt;

	mylog("enter CC_get_error\n");

	/* Create a very informative errormsg if it hasn't been done yet. */
	if (!self->errormsg_created)
	{
		msgcrt = CC_create_errormsg(self);
		if (self->__error_message)
			free(self->__error_message);
		self->__error_message = msgcrt;
		self->errormsg_created = TRUE;
	}

	if (CC_get_errornumber(self))
	{
		*number = CC_get_errornumber(self);
		*message = CC_get_errormsg(self);
	}
	rv = (CC_get_errornumber(self) != 0);

	self->__error_number = 0;		/* clear the error */

	mylog("exit CC_get_error\n");

	return rv;
}

static void CC_clear_cursors(ConnectionClass *self, BOOL allcursors)
{
	int	i;
	StatementClass	*stmt;
	QResultClass	*res;

	for (i = 0; i < self->num_stmts; i++)
	{
		stmt = self->stmts[i];
		if (stmt && (res = SC_get_Result(stmt)) &&
			res->cursor && res->cursor[0])
		{
		/*
		 * non-holdable cursors are automatically closed
		 * at commit time.
		 * all cursors are automatically closed
		 * at rollback time.
		 */
			if (res->cursor)
			{
				free(res->cursor);
				res->cursor = NULL;
			}
		}
	}
}

void	CC_on_commit(ConnectionClass *conn)
{
	if (CC_is_in_trans(conn))
	{
#ifdef	DRIVER_CURSOR_IMPLEMENT
		if (conn->result_uncommitted)
			ProcessRollback(conn, FALSE);
#endif /* DRIVER_CURSOR_IMPLEMENT */
		CC_set_no_trans(conn);
		CC_set_no_manual_trans(conn);
	}
	conn->result_uncommitted = 0;
	CC_clear_cursors(conn, TRUE);
	CC_discard_marked_plans(conn);
}


char
CC_send_settings(ConnectionClass *self)
{
	/* char ini_query[MAX_MESSAGE_LEN]; */
	ConnInfo   *ci = &(self->connInfo);

/* QResultClass *res; */
	HSTMT		hstmt;
	StatementClass *stmt;
	RETCODE		result;
	char		status = TRUE;
	char	   *cs,
			   *ptr;
#ifdef	HAVE_STRTOK_R
	char	*last;
#endif /* HAVE_STRTOK_R */
	CSTR func = "CC_send_settings";


	mylog("%s: entering...\n", func);

/*
 *	This function must use the local odbc API functions since the odbc state
 *	has not transitioned to "connected" yet.
 */

	result = PGAPI_AllocStmt(self, &hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		return FALSE;
	stmt = (StatementClass *) hstmt;

	stmt->internal = TRUE;		/* ensure no BEGIN/COMMIT/ABORT stuff */

	/* Set the Datestyle to the format the driver expects it to be in */
	result = PGAPI_ExecDirect(hstmt, "set DateStyle to 'ISO'", SQL_NTS, 0);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		status = FALSE;

	mylog("%s: result %d, status %d from set DateStyle\n", func, result, status);

	/* Disable genetic optimizer based on global flag */
	if (ci->drivers.disable_optimizer)
	{
		result = PGAPI_ExecDirect(hstmt, "set geqo to 'OFF'", SQL_NTS, 0);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
			status = FALSE;

		mylog("%s: result %d, status %d from set geqo\n", func, result, status);

	}

	/* KSQO (not applicable to 7.1+ - DJP 21/06/2002) */
	if (ci->drivers.ksqo && PG_VERSION_LT(self, 7.1))
	{
		result = PGAPI_ExecDirect(hstmt, "set ksqo to 'ON'", SQL_NTS, 0);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
			status = FALSE;

		mylog("%s: result %d, status %d from set ksqo\n", func, result, status);

	}

	/* extra_float_digits (applicable since 7.4) */
	if (PG_VERSION_GT(self, 7.3))
	{
		result = PGAPI_ExecDirect(hstmt, "set extra_float_digits to 2", SQL_NTS, 0);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
			status = FALSE;

		mylog("%s: result %d, status %d from set extra_float_digits\n", func, result, status);

	}

	/* Global settings */
	if (ci->drivers.conn_settings[0] != '\0')
	{
		cs = strdup(ci->drivers.conn_settings);
#ifdef	HAVE_STRTOK_R
		ptr = strtok_r(cs, ";", &last);
#else
		ptr = strtok(cs, ";");
#endif /* HAVE_STRTOK_R */
		while (ptr)
		{
			result = PGAPI_ExecDirect(hstmt, ptr, SQL_NTS, 0);
			if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
				status = FALSE;

			mylog("%s: result %d, status %d from '%s'\n", func, result, status, ptr);

#ifdef	HAVE_STRTOK_R
			ptr = strtok_r(NULL, ";", &last);
#else
			ptr = strtok(NULL, ";");
#endif /* HAVE_STRTOK_R */
		}

		free(cs);
	}

	/* Per Datasource settings */
	if (ci->conn_settings[0] != '\0')
	{
		cs = strdup(ci->conn_settings);
#ifdef	HAVE_STRTOK_R
		ptr = strtok_r(cs, ";", &last);
#else
		ptr = strtok(cs, ";");
#endif /* HAVE_STRTOK_R */
		while (ptr)
		{
			result = PGAPI_ExecDirect(hstmt, ptr, SQL_NTS, 0);
			if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
				status = FALSE;

			mylog("%s: result %d, status %d from '%s'\n", func, result, status, ptr);

#ifdef	HAVE_STRTOK_R
			ptr = strtok_r(NULL, ";", &last);
#else
			ptr = strtok(NULL, ";");
#endif /* HAVE_STRTOK_R */
		}

		free(cs);
	}


	PGAPI_FreeStmt(hstmt, SQL_DROP);

	return status;
}


/*
 *	This function is just a hack to get the oid of our Large Object oid type.
 *	If a real Large Object oid type is made part of Postgres, this function
 *	will go away and the define 'PG_TYPE_LO' will be updated.
 */
void
CC_lookup_lo(ConnectionClass *self)
{
	HSTMT		hstmt;
	StatementClass *stmt;
	RETCODE		result;
	CSTR func = "CC_lookup_lo";

	mylog("%s: entering...\n", func);

/*
 *	This function must use the local odbc API functions since the odbc state
 *	has not transitioned to "connected" yet.
 */
	result = PGAPI_AllocStmt(self, &hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		return;
	stmt = (StatementClass *) hstmt;

	result = PGAPI_ExecDirect(hstmt, "select oid from pg_type where typname='" PG_TYPE_LO_NAME "'", SQL_NTS, 0);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	result = PGAPI_Fetch(hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	result = PGAPI_GetData(hstmt, 1, SQL_C_SLONG, &self->lobj_type, sizeof(self->lobj_type), NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	mylog("Got the large object oid: %d\n", self->lobj_type);
	qlog("    [ Large Object oid = %d ]\n", self->lobj_type);

	result = PGAPI_FreeStmt(hstmt, SQL_DROP);
}


/*
 *	This function initializes the version of PostgreSQL from
 *	connInfo.protocol that we're connected to.
 *	h-inoue 01-2-2001
 */
void
CC_initialize_pg_version(ConnectionClass *self)
{
	strcpy(self->pg_version, self->connInfo.protocol);
	if (PROTOCOL_62(&self->connInfo))
	{
		self->pg_version_number = (float) 6.2;
		self->pg_version_major = 6;
		self->pg_version_minor = 2;
	}
	else if (PROTOCOL_63(&self->connInfo))
	{
		self->pg_version_number = (float) 6.3;
		self->pg_version_major = 6;
		self->pg_version_minor = 3;
	}
	else
	{
		self->pg_version_number = (float) 6.4;
		self->pg_version_major = 6;
		self->pg_version_minor = 4;
	}
}


/*
 *	This function gets the version of PostgreSQL that we're connected to.
 *	This is used to return the correct info in SQLGetInfo
 *	DJP - 25-1-2001
 */
void
CC_lookup_pg_version(ConnectionClass *self)
{
	HSTMT		hstmt;
	StatementClass *stmt;
	RETCODE		result;
	char		szVersion[32];
	int			major,
				minor;
	CSTR		func = "CC_lookup_pg_version";

	mylog("%s: entering...\n", func);

/*
 *	This function must use the local odbc API functions since the odbc state
 *	has not transitioned to "connected" yet.
 */
	result = PGAPI_AllocStmt(self, &hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		return;
	stmt = (StatementClass *) hstmt;

	/* get the server's version if possible	 */
	result = PGAPI_ExecDirect(hstmt, "select version()", SQL_NTS, 0);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	result = PGAPI_Fetch(hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	result = PGAPI_GetData(hstmt, 1, SQL_C_CHAR, self->pg_version, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	/*
	 * Extract the Major and Minor numbers from the string. This assumes
	 * the string starts 'Postgresql X.X'
	 */
	strcpy(szVersion, "0.0");
	if (sscanf(self->pg_version, "%*s %d.%d", &major, &minor) >= 2)
	{
		sprintf(szVersion, "%d.%d", major, minor);
		self->pg_version_major = major;
		self->pg_version_minor = minor;
	}
	self->pg_version_number = (float) atof(szVersion);
	if (PG_VERSION_GE(self, 7.3))
		self->schema_support = 1;

	mylog("Got the PostgreSQL version string: '%s'\n", self->pg_version);
	mylog("Extracted PostgreSQL version number: '%1.1f'\n", self->pg_version_number);
	qlog("    [ PostgreSQL version string = '%s' ]\n", self->pg_version);
	qlog("    [ PostgreSQL version number = '%1.1f' ]\n", self->pg_version_number);

	result = PGAPI_FreeStmt(hstmt, SQL_DROP);
}

int
CC_get_max_query_len(const ConnectionClass *conn)
{
	int			value;

	/* Long Queries in 7.0+ */
	if (PG_VERSION_GE(conn, 7.0))
		value = 0 /* MAX_STATEMENT_LEN */ ;
	/* Prior to 7.0 we used 2*BLCKSZ */
	else if (PG_VERSION_GE(conn, 6.5))
		value = (2 * BLCKSZ);
	else
		/* Prior to 6.5 we used BLCKSZ */
		value = BLCKSZ;
	return value;
}

/*
 *	This doesn't really return the CURRENT SCHEMA
 *	but there's no alternative.
 */
const char *
CC_get_current_schema(ConnectionClass *conn)
{
	if (!conn->current_schema && conn->schema_support)
	{
		QResultClass	*res;

		if (res = CC_send_query(conn, "select current_schema()", NULL, CLEAR_RESULT_ON_ABORT), res)
		{
			if (QR_get_num_total_tuples(res) == 1)
				conn->current_schema = strdup(QR_get_value_backend_row(res, 0, 0));
			QR_Destructor(res);
		}
	}
	return (const char *) conn->current_schema;
}

int	CC_mark_a_plan_to_discard(ConnectionClass *conn, const char *plan)
{
	int	cnt = conn->num_discardp + 1;
	char	*pname;

	CC_REALLOC_return_with_error(conn->discardp, char *,
		(cnt * sizeof(char *)), conn, "Couldn't alloc discardp.", -1) 
	CC_MALLOC_return_with_error(pname, char, (strlen(plan) + 1),
		conn, "Couldn't alloc discardp mem.", -1)
	strcpy(pname, plan);
	conn->discardp[conn->num_discardp++] = pname; 
	return 1;
}
int	CC_discard_marked_plans(ConnectionClass *conn)
{
	int	i, cnt;
	QResultClass *res;
	char	cmd[32];

	if ((cnt = conn->num_discardp) <= 0)
		return 0;
	for (i = cnt - 1; i >= 0; i--)
	{
		sprintf(cmd, "DEALLOCATE \"%s\"", conn->discardp[i]);
		res = CC_send_query(conn, cmd, NULL, CLEAR_RESULT_ON_ABORT);
		if (res)
		{
			QR_Destructor(res);
			free(conn->discardp[i]);
			conn->num_discardp--;
		}
		else
			return -1;
	}
	return 1;
}


#ifndef USE_LIBPQ
/*
 *		IMPLEMENTATION CONNECTION CLASS
 */
ConnectionClass *
CC_Constructor()
{
	ConnectionClass *rv;

	rv = (ConnectionClass *) malloc(sizeof(ConnectionClass));

	if (rv != NULL)
	{
		rv->henv = NULL;		/* not yet associated with an environment */

		rv->__error_message = NULL;
		rv->__error_number = 0;
		rv->errormsg_created = FALSE;

		rv->status = CONN_NOT_CONNECTED;
		rv->transact_status = CONN_IN_AUTOCOMMIT;		/* autocommit by default */

		CC_conninfo_init(&(rv->connInfo));
		rv->sock = SOCK_Constructor(rv);
		if (!rv->sock)
			return NULL;

		rv->stmts = (StatementClass **) malloc(sizeof(StatementClass *) * STMT_INCREMENT);
		if (!rv->stmts)
			return NULL;
		memset(rv->stmts, 0, sizeof(StatementClass *) * STMT_INCREMENT);

		rv->num_stmts = STMT_INCREMENT;
		rv->descs = (DescriptorClass **) malloc(sizeof(DescriptorClass *) * STMT_INCREMENT);
		if (!rv->descs)
			return NULL;
		memset(rv->descs, 0, sizeof(DescriptorClass *) * STMT_INCREMENT);

		rv->num_descs = STMT_INCREMENT;

		rv->lobj_type = PG_TYPE_LO_UNDEFINED;

		rv->ntables = 0;
		rv->col_info = NULL;

		rv->translation_option = 0;
		rv->translation_handle = NULL;
		rv->DataSourceToDriver = NULL;
		rv->DriverToDataSource = NULL;
		rv->driver_version = ODBCVER;
		memset(rv->pg_version, 0, sizeof(rv->pg_version));
		rv->pg_version_number = .0;
		rv->pg_version_major = 0;
		rv->pg_version_minor = 0;
		rv->ms_jet = 0;
		rv->unicode = 0;
		rv->result_uncommitted = 0;
		rv->schema_support = 0;
		rv->isolation = SQL_TXN_READ_COMMITTED;
		rv->client_encoding = NULL;
		rv->server_encoding = NULL;
		rv->current_schema = NULL;
		rv->num_discardp = 0;
		rv->discardp = NULL;

		/* Initialize statement options to defaults */
		/* Statements under this conn will inherit these options */

		InitializeStatementOptions(&rv->stmtOptions);
		InitializeARDFields(&rv->ardOptions);
		InitializeAPDFields(&rv->apdOptions);
		INIT_CONN_CS(rv);
	}
	return rv;
}


char
CC_Destructor(ConnectionClass *self)
{
	mylog("enter CC_Destructor, self=%u\n", self);

	if (self->status == CONN_EXECUTING)
		return 0;

	CC_cleanup(self);			/* cleanup socket and statements */

	mylog("after CC_Cleanup\n");

	/* Free up statement holders */
	if (self->stmts)
	{
		free(self->stmts);
		self->stmts = NULL;
	}

	if (self->descs)
	{
		free(self->descs);
		self->descs = NULL;
	}

	mylog("after free statement holders\n");

	if (self->__error_message)
		free(self->__error_message);
	DELETE_CONN_CS(self);
	free(self);

	mylog("exit CC_Destructor\n");

	return 1;
}


/* This is called by SQLDisconnect also */
char
CC_cleanup(ConnectionClass *self)
{
	int			i;
	StatementClass *stmt;
	DescriptorClass *desc;

	if (self->status == CONN_EXECUTING)
		return FALSE;

	mylog("in CC_Cleanup, self=%u\n", self);

	/* Cancel an ongoing transaction */
	/* We are always in the middle of a transaction, */
	/* even if we are in auto commit. */
	if (self->sock)
	{
		CC_abort(self);

		mylog("after CC_abort\n");

		/* This actually closes the connection to the dbase */
		SOCK_Destructor(self->sock);
		self->sock = NULL;
	}

	mylog("after SOCK destructor\n");

	/* Free all the stmts on this connection */
	for (i = 0; i < self->num_stmts; i++)
	{
		stmt = self->stmts[i];
		if (stmt)
		{
			stmt->hdbc = NULL;	/* prevent any more dbase interactions */

			SC_Destructor(stmt);

			self->stmts[i] = NULL;
		}
	}

	/* Free all the descs on this connection */
	for (i = 0; i < self->num_descs; i++)
	{
		desc = self->descs[i];
		if (desc)
		{
			DC_get_conn(desc) = NULL;	/* prevent any more dbase interactions */
			DC_Destructor(desc);
			free(desc);
			self->descs[i] = NULL;
		}
	}

	/* Check for translation dll */
#ifdef WIN32
	if (self->translation_handle)
	{
		FreeLibrary(self->translation_handle);
		self->translation_handle = NULL;
	}
#endif

	self->status = CONN_NOT_CONNECTED;
	self->transact_status = CONN_IN_AUTOCOMMIT;
	CC_conninfo_init(&(self->connInfo));
	if (self->client_encoding)
	{
		free(self->client_encoding);
		self->client_encoding = NULL;
	}
	if (self->server_encoding)
	{
		free(self->server_encoding);
		self->server_encoding = NULL;
	}
	if (self->current_schema)
	{
		free(self->current_schema);
		self->current_schema = NULL;
	}
	/* Free cached table info */
	if (self->col_info)
	{
		for (i = 0; i < self->ntables; i++)
		{
			if (self->col_info[i]->result)	/* Free the SQLColumns result structure */
				QR_Destructor(self->col_info[i]->result);

			if (self->col_info[i]->schema)
				free(self->col_info[i]->schema);
			free(self->col_info[i]);
		}
		free(self->col_info);
		self->col_info = NULL;
	}
	self->ntables = 0;
	if (self->num_discardp > 0 && self->discardp)
	{
		for (i = 0; i < self->num_discardp; i++)
			free(self->discardp[i]);
		self->num_discardp = 0;
	}
	if (self->discardp)
	{
		free(self->discardp);
		self->discardp = NULL;
	}

	mylog("exit CC_Cleanup\n");
	return TRUE;
}

static	int
md5_auth_send(ConnectionClass *self, const char *salt)
{
	char	*pwd1 = NULL, *pwd2 = NULL;
	ConnInfo   *ci = &(self->connInfo);
	SocketClass	*sock = self->sock;

	if (!(pwd1 = malloc(MD5_PASSWD_LEN + 1)))
		return 1;
	if (!EncryptMD5(ci->password, ci->username, strlen(ci->username), pwd1))
	{
		free(pwd1);
		return 1;
	} 
	if (!(pwd2 = malloc(MD5_PASSWD_LEN + 1)))
	{
		free(pwd1);
		return 1;
	} 
	if (!EncryptMD5(pwd1 + strlen("md5"), salt, 4, pwd2))
	{
		free(pwd2);
		free(pwd1);
		return 1;
	}
	free(pwd1);
	SOCK_put_int(sock, 4 + strlen(pwd2) + 1, 4);
	SOCK_put_n_char(sock, pwd2, strlen(pwd2) + 1);
	SOCK_flush_output(sock);
	free(pwd2);
	return 0; 
}


char
CC_connect(ConnectionClass *self, char password_req, char *salt_para)
{
	StartupPacket sp;
	StartupPacket6_2 sp62;
	QResultClass *res;
	SocketClass *sock;
	ConnInfo   *ci = &(self->connInfo);
	int			areq = -1;
	int			beresp;
	char		msgbuffer[ERROR_MSG_LENGTH];
	char		salt[5], notice[512];
	CSTR		func = "CC_connect";
	char	   *encoding;

	mylog("%s: entering...\n", func);

	if (password_req != AUTH_REQ_OK)

		sock = self->sock;		/* already connected, just authenticate */

	else
	{
		qlog("Global Options: Version='%s', fetch=%d, socket=%d, unknown_sizes=%d, max_varchar_size=%d, max_longvarchar_size=%d\n",
			 POSTGRESDRIVERVERSION,
			 ci->drivers.fetch_max,
			 ci->drivers.socket_buffersize,
			 ci->drivers.unknown_sizes,
			 ci->drivers.max_varchar_size,
			 ci->drivers.max_longvarchar_size);
		qlog("                disable_optimizer=%d, ksqo=%d, unique_index=%d, use_declarefetch=%d\n",
			 ci->drivers.disable_optimizer,
			 ci->drivers.ksqo,
			 ci->drivers.unique_index,
			 ci->drivers.use_declarefetch);
		qlog("                text_as_longvarchar=%d, unknowns_as_longvarchar=%d, bools_as_char=%d NAMEDATALEN=%d\n",
			 ci->drivers.text_as_longvarchar,
			 ci->drivers.unknowns_as_longvarchar,
			 ci->drivers.bools_as_char,
			 TABLE_NAME_STORAGE_LEN);

		encoding = check_client_encoding(ci->conn_settings);
		if (encoding && strcmp(encoding, "OTHER"))
			self->client_encoding = strdup(encoding);
		else
		{
			encoding = check_client_encoding(ci->drivers.conn_settings);
			if (encoding && strcmp(encoding, "OTHER"))
				self->client_encoding = strdup(encoding);
		}
		if (self->client_encoding)
			self->ccsc = pg_CS_code(self->client_encoding);
		qlog("                extra_systable_prefixes='%s', conn_settings='%s' conn_encoding='%s'\n",
			 ci->drivers.extra_systable_prefixes,
			 ci->drivers.conn_settings,
			 encoding ? encoding : "");

		if (self->status != CONN_NOT_CONNECTED)
		{
			CC_set_error(self, CONN_OPENDB_ERROR, "Already connected.");
			return 0;
		}

		if (ci->port[0] == '\0' ||
#ifdef	WIN32
			ci->server[0] == '\0' ||
#endif /* WIN32 */
			ci->database[0] == '\0')
		{
			CC_set_error(self, CONN_INIREAD_ERROR, "Missing server name, port, or database name in call to CC_connect.");
			return 0;
		}

		mylog("CC_connect(): DSN = '%s', server = '%s', port = '%s', sslmode = '%s'"
#ifdef HAVE_UNIX_SOCKETS
		      " uds = '%s',"
#endif
		      " database = '%s', username = '%s',"
		      " password='%s'\n", ci->dsn, ci->server, ci->port, ci->sslmode,
#ifdef HAVE_UNIX_SOCKETS
		      ci->uds,
#endif
		      ci->database, ci->username, ci->password ? "xxxxx" : "");

another_version_retry:

		/*
		 * If the socket was closed for some reason (like a SQLDisconnect,
		 * but no SQLFreeConnect then create a socket now.
		 */
		if (!self->sock)
		{
			self->sock = SOCK_Constructor(self);
			if (!self->sock)
			{
				CC_set_error(self, CONNECTION_SERVER_NOT_REACHED, "Could not open a socket to the server");
				return 0;
			}
		}

		sock = self->sock;

		mylog("connecting to the server socket...\n");

		SOCK_connect_to(sock, (short) atoi(ci->port), ci->server
#ifdef HAVE_UNIX_SOCKETS
				, ci->uds
#endif
				);
		if (SOCK_get_errcode(sock) != 0)
		{
			mylog("connection to the server socket failed.\n");
			CC_set_error(self, CONNECTION_SERVER_NOT_REACHED, "Could not connect to the server");
			return 0;
		}
		mylog("connection to the server socket succeeded.\n");

		if (PROTOCOL_62(ci))
		{
			sock->reverse = TRUE;		/* make put_int and get_int work
										 * for 6.2 */

			memset(&sp62, 0, sizeof(StartupPacket6_2));
			SOCK_put_int(sock, htonl(4 + sizeof(StartupPacket6_2)), 4);
			sp62.authtype = htonl(NO_AUTHENTICATION);
			strncpy(sp62.database, ci->database, PATH_SIZE);
			strncpy(sp62.user, ci->username, USRNAMEDATALEN);
			SOCK_put_n_char(sock, (char *) &sp62, sizeof(StartupPacket6_2));
			SOCK_flush_output(sock);
		}
		else
		{
			memset(&sp, 0, sizeof(StartupPacket));

			mylog("sizeof startup packet = %d\n", sizeof(StartupPacket));

			/* Send length of Authentication Block */
			SOCK_put_int(sock, 4 + sizeof(StartupPacket), 4);

			if (PROTOCOL_63(ci))
				sp.protoVersion = (ProtocolVersion) htonl(PG_PROTOCOL_63);
			else
				sp.protoVersion = (ProtocolVersion) htonl(PG_PROTOCOL_LATEST);

			strncpy(sp.database, ci->database, SM_DATABASE);
			strncpy(sp.user, ci->username, SM_USER);

			SOCK_put_n_char(sock, (char *) &sp, sizeof(StartupPacket));
			SOCK_flush_output(sock);
		}

		mylog("sent the authentication block.\n");

		if (sock->errornumber != 0)
		{
			mylog("couldn't send the authentication block properly.\n");
			CC_set_error(self, CONN_INVALID_AUTHENTICATION, "Sending the authentication packet failed");
			return 0;
		}
		mylog("sent the authentication block successfully.\n");
	}


	mylog("gonna do authentication\n");


	/*
	 * Now get the authentication request from backend
	 */

	if (!PROTOCOL_62(ci))
	{
		BOOL		before_64 = PG_VERSION_LT(self, 6.4),
					ReadyForQuery = FALSE;

		do
		{
			if (password_req != AUTH_REQ_OK)
				beresp = 'R';
			else
			{
				beresp = SOCK_get_char(sock);
				mylog("auth got '%c'\n", beresp);
			}

			switch (beresp)
			{
				case 'E':

					SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
					CC_set_error(self, CONN_INVALID_AUTHENTICATION, msgbuffer);
					qlog("ERROR from backend during authentication: '%s'\n", msgbuffer);
					if (strncmp(msgbuffer, "Unsupported frontend protocol", 29) == 0)
					{			/* retry older version */
						if (PROTOCOL_63(ci))
							strcpy(ci->protocol, PG62);
						else
							strcpy(ci->protocol, PG63);
						SOCK_Destructor(sock);
						self->sock = (SocketClass *) 0;
						CC_initialize_pg_version(self);
						goto another_version_retry;
					}

					return 0;
				case 'R':

					if (password_req != AUTH_REQ_OK)
					{
						mylog("in 'R' password_req=%s\n", ci->password);
						areq = password_req;
						if (salt_para)
							memcpy(salt, salt_para, sizeof(salt));
						password_req = AUTH_REQ_OK;
					}
					else
					{

						areq = SOCK_get_int(sock, 4);
						if (areq == AUTH_REQ_MD5)
							SOCK_get_n_char(sock, salt, 4);
						else if (areq == AUTH_REQ_CRYPT)
							SOCK_get_n_char(sock, salt, 2);

						mylog("areq = %d\n", areq);
					}
					switch (areq)
					{
						case AUTH_REQ_OK:
							break;

						case AUTH_REQ_KRB4:
							CC_set_error(self, CONN_AUTH_TYPE_UNSUPPORTED, "Kerberos 4 authentication not supported");
							return 0;

						case AUTH_REQ_KRB5:
							CC_set_error(self, CONN_AUTH_TYPE_UNSUPPORTED, "Kerberos 5 authentication not supported");
							return 0;

						case AUTH_REQ_PASSWORD:
							mylog("in AUTH_REQ_PASSWORD\n");

							if (ci->password[0] == '\0')
							{
								CC_set_error(self, CONNECTION_NEED_PASSWORD, "A password is required for this connection.");
								return -areq;		/* need password */
							}

							mylog("past need password\n");

							SOCK_put_int(sock, 4 + strlen(ci->password) + 1, 4);
							SOCK_put_n_char(sock, ci->password, strlen(ci->password) + 1);
							SOCK_flush_output(sock);

							mylog("past flush\n");
							break;

						case AUTH_REQ_CRYPT:
							CC_set_error(self, CONN_AUTH_TYPE_UNSUPPORTED, "Password crypt authentication not supported");
							return 0;
						case AUTH_REQ_MD5:
							mylog("in AUTH_REQ_MD5\n");
							if (ci->password[0] == '\0')
							{
								CC_set_error(self, CONNECTION_NEED_PASSWORD, "A password is required for this connection.");
								if (salt_para)
									memcpy(salt_para, salt, sizeof(salt));
								return -areq; /* need password */
							}
							if (md5_auth_send(self, salt))
							{
								CC_set_error(self, CONN_INVALID_AUTHENTICATION, "md5 hashing failed");
								return 0;
							}
							break;

						case AUTH_REQ_SCM_CREDS:
							CC_set_error(self, CONN_AUTH_TYPE_UNSUPPORTED, "Unix socket credential authentication not supported");
							return 0;

						default:
							CC_set_error(self, CONN_AUTH_TYPE_UNSUPPORTED, "Unknown authentication type");
							return 0;
					}
					break;
				case 'K':		/* Secret key (6.4 protocol) */
					self->be_pid = SOCK_get_int(sock, 4);		/* pid */
					self->be_key = SOCK_get_int(sock, 4);		/* key */

					break;
				case 'Z':		/* Backend is ready for new query (6.4) */
					ReadyForQuery = TRUE;
					break;
				case 'N':	/* Notices may come */
					while (SOCK_get_string(sock, notice, sizeof(notice) - 1)) ;
					break;
				default:
					CC_set_error(self, CONN_INVALID_AUTHENTICATION, "Unexpected protocol character during authentication");
					return 0;
			}

			/*
			 * There were no ReadyForQuery responce before 6.4.
			 */
			if (before_64 && areq == AUTH_REQ_OK)
				ReadyForQuery = TRUE;
		} while (!ReadyForQuery);
	}


	CC_clear_error(self);		/* clear any password error */

	/*
	 * send an empty query in order to find out whether the specified
	 * database really exists on the server machine
	 */
	mylog("sending an empty query...\n");

	res = CC_send_query(self, " ", NULL, CLEAR_RESULT_ON_ABORT);
	if (res == NULL ||
	    (QR_get_status(res) != PGRES_EMPTY_QUERY &&
	     QR_command_nonfatal(res)))
	{
		mylog("got no result from the empty query.  (probably database does not exist)\n");
		CC_set_error(self, CONNECTION_NO_SUCH_DATABASE, "The database does not exist on the server\nor user authentication failed.");
		if (res != NULL)
			QR_Destructor(res);
		return 0;
	}
	if (res)
		QR_Destructor(res);

	mylog("empty query seems to be OK.\n");

	CC_set_translation(self);

	/*
	 * Send any initial settings
	 */

	/* 
	 * Get the version number first so we can check it before sending options
	 * that are now obsolete. DJP 21/06/2002
	 */

	CC_lookup_pg_version(self);		/* Get PostgreSQL version for
						   SQLGetInfo use */
	/*
	 * Since these functions allocate statements, and since the connection
	 * is not established yet, it would violate odbc state transition
	 * rules.  Therefore, these functions call the corresponding local
	 * function instead.
	 */
	CC_send_settings(self);
	CC_clear_error(self);			/* clear any error */
	CC_lookup_lo(self);			/* a hack to get the oid of
						   our large object oid type */

	/*
	 *	Multibyte handling is available ?
	 */
	if (PG_VERSION_GE(self, 6.4))
	{
		CC_lookup_characterset(self);
		if (CC_get_errornumber(self) != 0)
			return 0;
		
		if (self->unicode)
		{
			if (!self->client_encoding ||
			    !stricmp(self->client_encoding, "UNICODE"))
			{
				QResultClass	*res;
				if (PG_VERSION_LT(self, 7.1))
				{
					CC_set_error(self, CONN_NOT_IMPLEMENTED_ERROR, "UTF-8 conversion isn't implemented before 7.1");
					return 0;
				}
				if (self->client_encoding)
					free(self->client_encoding);
				self->client_encoding = NULL;
				if (res = CC_send_query(self, "set client_encoding to 'UTF8'", NULL, CLEAR_RESULT_ON_ABORT), res)
				{
					self->client_encoding = strdup("UNICODE");
					self->ccsc = pg_CS_code(self->client_encoding);
					QR_Destructor(res);
					
				}
			}
		}
	}
	else if (self->unicode)
	{
		CC_set_error(self, CONN_NOT_IMPLEMENTED_ERROR, "Unicode isn't supported before 6.4");
		return 0;
	}

	ci->updatable_cursors = 0;
#ifdef	DRIVER_CURSOR_IMPLEMENT
	if (!ci->drivers.use_declarefetch &&
		PG_VERSION_GE(self, 7.0)) /* Tid scan since 7.0 */
		ci->updatable_cursors = ci->allow_keyset;
#endif /* DRIVER_CURSOR_IMPLEMENT */

	CC_clear_error(self);		/* clear any initial command errors */
	self->status = CONN_CONNECTED;

	mylog("%s: returning...\n", func);

	return 1;

}


/*
 *	Create a more informative error message by concatenating the connection
 *	error message with its socket error message.
 */
char *
CC_create_errormsg(ConnectionClass *self)
{
	SocketClass *sock = self->sock;
	int			pos;
	char	 msg[4096];

	mylog("enter CC_create_errormsg\n");

	msg[0] = '\0';

	if (CC_get_errormsg(self))
		strncpy(msg, CC_get_errormsg(self), sizeof(msg));

	mylog("msg = '%s'\n", msg);

	if (sock && sock->errormsg && sock->errormsg[0] != '\0')
	{
		pos = strlen(msg);
		sprintf(&msg[pos], ";\n%s", sock->errormsg);
	}

	mylog("exit CC_create_errormsg\n");
	return msg ? strdup(msg) : NULL;
}

void	CC_on_abort(ConnectionClass *conn, UDWORD opt)
{
	BOOL	set_no_trans = FALSE;

	if (0 != (opt & CONN_DEAD))
		opt |= NO_TRANS;
	if (CC_is_in_trans(conn))
	{
#ifdef	DRIVER_CURSOR_IMPLEMENT
		if (conn->result_uncommitted)
			ProcessRollback(conn, TRUE);
#endif /* DRIVER_CURSOR_IMPLEMENT */
		if (0 != (opt & NO_TRANS))
		{
			CC_set_no_trans(conn);
			CC_set_no_manual_trans(conn);
			set_no_trans = TRUE;
		}
	}
	CC_clear_cursors(conn, TRUE);
	if (0 != (opt & CONN_DEAD))
	{
		conn->status = CONN_DOWN;
		if (conn->sock)
		{
			SOCK_Destructor(conn->sock);
			conn->sock = NULL;
		}
	}
	else if (set_no_trans)
		CC_discard_marked_plans(conn);
	conn->result_uncommitted = 0;
}

/*
 *	The "result_in" is only used by QR_next_tuple() to fetch another group of rows into
 *	the same existing QResultClass (this occurs when the tuple cache is depleted and
 *	needs to be re-filled).
 *
 *	The "cursor" is used by SQLExecute to associate a statement handle as the cursor name
 *	(i.e., C3326857) for SQL select statements.  This cursor is then used in future
 *	'declare cursor C3326857 for ...' and 'fetch 100 in C3326857' statements.
 */
QResultClass *
CC_send_query(ConnectionClass *self, char *query, QueryInfo *qi, UDWORD flag)
{
	QResultClass *cmdres = NULL,
			   *retres = NULL,
			   *res = NULL;
	BOOL	clear_result_on_abort = ((flag & CLEAR_RESULT_ON_ABORT) != 0),
		create_keyset = ((flag & CREATE_KEYSET) != 0),
		issue_begin = ((flag & GO_INTO_TRANSACTION) != 0 && !CC_is_in_trans(self));
	char		swallow, *wq, *ptr;
	int			id;
	SocketClass *sock = self->sock;
	int			maxlen,
				empty_reqs;
	BOOL		msg_truncated,
				ReadyToReturn = FALSE,
				query_completed = FALSE,
				before_64 = PG_VERSION_LT(self, 6.4),
				aborted = FALSE,
				used_passed_result_object = FALSE;
	UDWORD		abort_opt;
	int		func_cs_count = 0;

	/* ERROR_MSG_LENGTH is suffcient */
	char msgbuffer[ERROR_MSG_LENGTH + 1];

	/* QR_set_command() dups this string so doesn't need static */
	char		cmdbuffer[ERROR_MSG_LENGTH + 1];

	mylog("send_query(): conn=%u, query='%s'\n", self, query);
	qlog("conn=%u, query='%s'\n", self, query);

	if (!self->sock)
	{
		CC_set_error(self, CONNECTION_COULD_NOT_SEND, "Could not send Query(connection dead)");
		CC_on_abort(self, NO_TRANS);
		return NULL;
	}

	/* Indicate that we are sending a query to the backend */
	maxlen = CC_get_max_query_len(self);
	if (maxlen > 0 && maxlen < (int) strlen(query) + 1)
	{
		CC_set_error(self, CONNECTION_MSG_TOO_LONG, "Query string is too long");
		return NULL;
	}

	if ((NULL == query) || (query[0] == '\0'))
		return NULL;

	if (SOCK_get_errcode(sock) != 0)
	{
		CC_set_error(self, CONNECTION_COULD_NOT_SEND, "Could not send Query to backend");
		CC_on_abort(self, NO_TRANS | CONN_DEAD);
		return NULL;
	}

#define	return DONT_CALL_RETURN_FROM_HERE???
	ENTER_INNER_CONN_CS(self, func_cs_count);
	SOCK_put_char(sock, 'Q');
	if (SOCK_get_errcode(sock) != 0)
	{
		CC_set_error(self, CONNECTION_COULD_NOT_SEND, "Could not send Query to backend");
		CC_on_abort(self, NO_TRANS | CONN_DEAD);
		goto cleanup;
	}

	if (issue_begin)
		SOCK_put_n_char(sock, "BEGIN;", 6);
	SOCK_put_string(sock, query);
	SOCK_flush_output(sock);

	if (SOCK_get_errcode(sock) != 0)
	{
		CC_set_error(self, CONNECTION_COULD_NOT_SEND, "Could not send Query to backend");
		CC_on_abort(self, NO_TRANS | CONN_DEAD);
		goto cleanup;
	}

	mylog("send_query: done sending query\n");

	empty_reqs = 0;
	for (wq = query; isspace((UCHAR) *wq); wq++)
		;
	if (*wq == '\0')
		empty_reqs = 1;
	cmdres = qi ? qi->result_in : NULL;
	if (cmdres)
		used_passed_result_object = TRUE;
	else
	{
		cmdres = QR_Constructor();
		if (!cmdres)
		{
			CC_set_error(self, CONNECTION_COULD_NOT_RECEIVE, "Could not create result info in send_query.");
			goto cleanup;
		}
	}
	res = cmdres;
	while (!ReadyToReturn)
	{
		/* what type of message is coming now ? */
		id = SOCK_get_char(sock);

		if ((SOCK_get_errcode(sock) != 0) || (id == EOF))
		{
			CC_set_error(self, CONNECTION_NO_RESPONSE, "No response from the backend");

			mylog("send_query: 'id' - %s\n", CC_get_errormsg(self));
			CC_on_abort(self, NO_TRANS | CONN_DEAD);
			ReadyToReturn = TRUE;
			retres = NULL;
			break;
		}

		mylog("send_query: got id = '%c'\n", id);

		switch (id)
		{
			case 'A':			/* Asynchronous Messages are ignored */
				(void) SOCK_get_int(sock, 4);	/* id of notification */
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				/* name of the relation the message comes from */
				break;
			case 'C':			/* portal query command, no tuples
								 * returned */
				/* read in the return message from the backend */
				SOCK_get_string(sock, cmdbuffer, ERROR_MSG_LENGTH);
				if (SOCK_get_errcode(sock) != 0)
				{
					CC_set_error(self, CONNECTION_NO_RESPONSE, "No response from backend while receiving a portal query command");
					mylog("send_query: 'C' - %s\n", CC_get_errormsg(self));
					CC_on_abort(self, NO_TRANS | CONN_DEAD);
					ReadyToReturn = TRUE;
					retres = NULL;
				}
				else
				{
					mylog("send_query: ok - 'C' - %s\n", cmdbuffer);

					if (query_completed)	/* allow for "show" style notices */
					{
						res->next = QR_Constructor();
						res = res->next;
					} 

					mylog("send_query: setting cmdbuffer = '%s'\n", cmdbuffer);

					if (strnicmp(cmdbuffer, "BEGIN", 5) == 0)
					{
						CC_set_in_trans(self);
						if (issue_begin)
						{
							issue_begin = FALSE;
							continue;
						}
					}
					else if (strnicmp(cmdbuffer, "COMMIT", 6) == 0)
						CC_on_commit(self);
					else if (strnicmp(cmdbuffer, "ROLLBACK", 8) == 0)
					{
						/* 
						   The method of ROLLBACK an original form ....
						   ROLLBACK [ WORK | TRANSACTION ] TO [ SAVEPOINT ] savepoint_name
						 */
						if (PG_VERSION_LT(self, 8.0) || !(contains_token(query, " TO ")))
							CC_on_abort(self, NO_TRANS);
					}
					else if (strnicmp(cmdbuffer, "END", 3) == 0)
						CC_on_commit(self);
					else if (strnicmp(cmdbuffer, "ABORT", 5) == 0)
						CC_on_abort(self, NO_TRANS);
					else
					{
						trim(cmdbuffer); /* get rid of trailing space */ 
						ptr = strrchr(cmdbuffer, ' ');
						if (ptr)
							res->recent_processed_row_count = atoi(ptr + 1);
						else
							res->recent_processed_row_count = -1;
					}

					if (QR_command_successful(res))
						QR_set_status(res, PGRES_COMMAND_OK);
					QR_set_command(res, cmdbuffer);
					query_completed = TRUE;
					mylog("send_query: returning res = %u\n", res);
					if (!before_64)
						break;

					/*
					 * (Quotation from the original comments) since
					 * backend may produce more than one result for some
					 * commands we need to poll until clear so we send an
					 * empty query, and keep reading out of the pipe until
					 * an 'I' is received
					 */

					if (empty_reqs == 0)
					{
						SOCK_put_string(sock, "Q ");
						SOCK_flush_output(sock);
						empty_reqs++;
					}
				}
				break;
			case 'Z':			/* Backend is ready for new query (6.4) */
				if (empty_reqs == 0)
				{
					ReadyToReturn = TRUE;
					if (aborted || query_completed)
						retres = cmdres;
					else
						ReadyToReturn = FALSE;
				}
				break;
			case 'N':			/* NOTICE: */
				msg_truncated = SOCK_get_string(sock, cmdbuffer, ERROR_MSG_LENGTH);
				if (QR_command_successful(res))
					QR_set_status(res, PGRES_NONFATAL_ERROR);
				QR_set_notice(res, cmdbuffer);	/* will dup this string */
				mylog("~~~ NOTICE: '%s'\n", cmdbuffer);
				qlog("NOTICE from backend during send_query: '%s'\n", cmdbuffer);
				while (msg_truncated)
					msg_truncated = SOCK_get_string(sock, cmdbuffer, ERROR_MSG_LENGTH);

				continue;		/* dont return a result -- continue
								 * reading */

			case 'I':			/* The server sends an empty query */
				/* There is a closing '\0' following the 'I', so we eat it */
				swallow = SOCK_get_char(sock);
				if ((swallow != '\0') || SOCK_get_errcode(sock) != 0)
				{
					CC_set_errornumber(self, CONNECTION_BACKEND_CRAZY);
					QR_set_message(res, "Unexpected protocol character from backend (send_query - I)");
					QR_set_status(res, PGRES_FATAL_ERROR);
					ReadyToReturn = TRUE;
					retres = cmdres;
					break;
				}
				else
				{
					/* We return the empty query */
					QR_set_status(res, PGRES_EMPTY_QUERY);
				}
				if (empty_reqs > 0)
				{
					if (--empty_reqs == 0)
						query_completed = TRUE;
				}
				break;
			case 'E':
				msg_truncated = SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);

				/* Remove a newline */
				if (msgbuffer[0] != '\0' && msgbuffer[strlen(msgbuffer) - 1] == '\n')
					msgbuffer[strlen(msgbuffer) - 1] = '\0';


				mylog("send_query: 'E' - %s\n", msgbuffer);
				qlog("ERROR from backend during send_query: '%s'\n", msgbuffer);

				/* We should report that an error occured. Zoltan */
				abort_opt = 0;
				if (!strncmp(msgbuffer, "FATAL", 5))
				{
					CC_set_errornumber(self, CONNECTION_SERVER_REPORTED_ERROR);
					abort_opt = NO_TRANS | CONN_DEAD;
                    ReadyToReturn = TRUE;
                    retres = NULL;
				}
				else
				{
					CC_set_errornumber(self, CONNECTION_SERVER_REPORTED_WARNING);
					if (CC_is_in_trans(self))
						CC_set_in_error_trans(self);
				}
				CC_on_abort(self, abort_opt);
				QR_set_status(res, PGRES_FATAL_ERROR);
				QR_set_message(res, msgbuffer);
				QR_set_aborted(res, TRUE);
				aborted = TRUE;
				while (msg_truncated)
					msg_truncated = SOCK_get_string(sock, cmdbuffer, ERROR_MSG_LENGTH);

				query_completed = TRUE;
				break;

			case 'P':			/* get the Portal name */
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				break;
			case 'T':			/* Tuple results start here */
				if (query_completed)
				{
					res->next = QR_Constructor();
					if (!res->next)
					{
						CC_set_error(self, CONNECTION_COULD_NOT_RECEIVE, "Could not create result info in send_query.");
						ReadyToReturn = TRUE;
						retres = NULL;
						break;
					}
					if (create_keyset)
						QR_set_haskeyset(res->next);
					mylog("send_query: 'T' no result_in: res = %u\n", res->next);
					res = res->next;

					if (qi)
						QR_set_cache_size(res, qi->row_size);
				}
				if (!used_passed_result_object)
				{
					if (create_keyset)
						QR_set_haskeyset(res);
					if (!QR_fetch_tuples(res, self, qi ? qi->cursor : NULL))
					{
						CC_set_error(self, CONNECTION_COULD_NOT_RECEIVE, QR_get_message(res));
						ReadyToReturn = TRUE;
						if (PGRES_FATAL_ERROR == QR_get_status(res))
							retres = cmdres;
						else
							retres = NULL;
						break;
					}
					query_completed = TRUE;
				}
				else
				{				/* next fetch, so reuse an existing result */

					/*
					 * called from QR_next_tuple and must return
					 * immediately.
					 */
					ReadyToReturn = TRUE;
					if (!QR_fetch_tuples(res, NULL, NULL))
					{
						CC_set_error(self, CONNECTION_COULD_NOT_RECEIVE, QR_get_message(res));
						retres = NULL;
						break;
					}
					retres = cmdres;
				}
				break;
			case 'D':			/* Copy in command began successfully */
				if (query_completed)
				{
					res->next = QR_Constructor();
					res = res->next;
				}
				QR_set_status(res, PGRES_COPY_IN);
				ReadyToReturn = TRUE;
				retres = cmdres;
				break;
			case 'B':			/* Copy out command began successfully */
				if (query_completed)
				{
					res->next = QR_Constructor();
					res = res->next;
				}
				QR_set_status(res, PGRES_COPY_OUT);
				ReadyToReturn = TRUE;
				retres = cmdres;
				break;
			default:
				CC_set_error(self, CONNECTION_BACKEND_CRAZY, "Unexpected protocol character from backend (send_query)");
				CC_on_abort(self, NO_TRANS | CONN_DEAD);

				mylog("send_query: error - %s\n", CC_get_errormsg(self));
				ReadyToReturn = TRUE;
				retres = NULL;
				break;
		}

		/*
		 * There were no ReadyForQuery response before 6.4.
		 */
		if (before_64)
		{
			if (empty_reqs == 0 && query_completed)
				break;
		}
	}

cleanup:
	CLEANUP_FUNC_CONN_CS(func_cs_count, self);
	/*
	 * Break before being ready to return.
	 */
	if (!ReadyToReturn)
		retres = cmdres;

	/*
	 * Cleanup garbage results before returning.
	 */
	if (cmdres && retres != cmdres && !used_passed_result_object)
		QR_Destructor(cmdres);
	/*
	 * Cleanup the aborted result if specified
	 */
	if (retres)
	{
		if (aborted)
		{
			if (clear_result_on_abort)
			{
	   			if (!used_passed_result_object)
				{
					QR_Destructor(retres);
					retres = NULL;
				}
			}
			if (retres)
			{
				/*
				 *	discard results other than errors.
				 */
				QResultClass	*qres;
				for (qres = retres; qres->next; qres = retres)
				{
					if (QR_get_aborted(qres))
						break;
					retres = qres->next;
					qres->next = NULL;
					QR_Destructor(qres);
				}
				/*
				 *	If error message isn't set
				 */
				if (retres && (!CC_get_errormsg(self) || !CC_get_errormsg(self)[0]))
					CC_set_errormsg(self, QR_get_message(retres));
			}
		}
	}
#undef	return
	return retres;
}


int
CC_send_function(ConnectionClass *self, int fnid, void *result_buf, int *actual_result_len, int result_is_int, LO_ARG *args, int nargs)
{
	char		id,
				c,
				done;
	SocketClass *sock = self->sock;

	/* ERROR_MSG_LENGTH is sufficient */
	char msgbuffer[ERROR_MSG_LENGTH + 1];
	int			i;

	mylog("send_function(): conn=%u, fnid=%d, result_is_int=%d, nargs=%d\n", self, fnid, result_is_int, nargs);

	if (!self->sock)
	{
		CC_set_error(self, CONNECTION_COULD_NOT_SEND, "Could not send function(connection dead)");
		CC_on_abort(self, NO_TRANS);
		return FALSE;
	}

	if (SOCK_get_errcode(sock) != 0)
	{
		CC_set_error(self, CONNECTION_COULD_NOT_SEND, "Could not send function to backend");
		CC_on_abort(self, NO_TRANS | CONN_DEAD);
		return FALSE;
	}

	SOCK_put_string(sock, "F ");
	if (SOCK_get_errcode(sock) != 0)
	{
		CC_set_error(self, CONNECTION_COULD_NOT_SEND, "Could not send function to backend");
		CC_on_abort(self, NO_TRANS | CONN_DEAD);
		return FALSE;
	}

	SOCK_put_int(sock, fnid, 4);
	SOCK_put_int(sock, nargs, 4);


	mylog("send_function: done sending function\n");

	for (i = 0; i < nargs; ++i)
	{
		mylog("  arg[%d]: len = %d, isint = %d, integer = %d, ptr = %u\n", i, args[i].len, args[i].isint, args[i].u.integer, args[i].u.ptr);

		SOCK_put_int(sock, args[i].len, 4);
		if (args[i].isint)
			SOCK_put_int(sock, args[i].u.integer, 4);
		else
			SOCK_put_n_char(sock, (char *) args[i].u.ptr, args[i].len);


	}

	mylog("    done sending args\n");

	SOCK_flush_output(sock);
	mylog("  after flush output\n");

	done = FALSE;
	while (!done)
	{
		id = SOCK_get_char(sock);
		mylog("   got id = %c\n", id);

		switch (id)
		{
			case 'V':
				done = TRUE;
				break;			/* ok */

			case 'N':
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				mylog("send_function(V): 'N' - %s\n", msgbuffer);
				/* continue reading */
				break;

			case 'E':
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				CC_set_errormsg(self, msgbuffer);
				if (CC_is_in_trans(self))
					CC_set_in_error_trans(self);
				CC_on_abort(self, 0);

				mylog("send_function(V): 'E' - %s\n", CC_get_errormsg(self));
				qlog("ERROR from backend during send_function: '%s'\n", CC_get_errormsg(self));

				return FALSE;

			case 'Z':
				break;

			default:
				CC_set_error(self, CONNECTION_BACKEND_CRAZY, "Unexpected protocol character from backend (send_function, args)");
				CC_on_abort(self, NO_TRANS | CONN_DEAD);

				mylog("send_function: error - %s\n", CC_get_errormsg(self));
				return FALSE;
		}
	}

	id = SOCK_get_char(sock);
	for (;;)
	{
		switch (id)
		{
			case 'G':			/* function returned properly */
				mylog("  got G!\n");

				*actual_result_len = SOCK_get_int(sock, 4);
				mylog("  actual_result_len = %d\n", *actual_result_len);

				if (result_is_int)
					*((int *) result_buf) = SOCK_get_int(sock, 4);
				else
					SOCK_get_n_char(sock, (char *) result_buf, *actual_result_len);

				mylog("  after get result\n");

				c = SOCK_get_char(sock);		/* get the last '0' */

				mylog("   after get 0\n");

				return TRUE;

			case 'E':
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				CC_set_errormsg(self, msgbuffer);
				CC_on_abort(self, 0);
				mylog("send_function(G): 'E' - %s\n", CC_get_errormsg(self));
				qlog("ERROR from backend during send_function: '%s'\n", CC_get_errormsg(self));

				return FALSE;

			case 'N':
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);

				mylog("send_function(G): 'N' - %s\n", msgbuffer);
				qlog("NOTICE from backend during send_function: '%s'\n", msgbuffer);

				continue;		/* dont return a result -- continue
								 * reading */

			case '0':			/* empty result */
				return TRUE;

			default:
				CC_set_error(self, CONNECTION_BACKEND_CRAZY, "Unexpected protocol character from backend (send_function, result)");
				CC_on_abort(self, NO_TRANS | CONN_DEAD);

				mylog("send_function: error - %s\n", CC_get_errormsg(self));
				return FALSE;
		}
	}
}

void
CC_log_error(const char *func, const char *desc, const ConnectionClass *self)
{
#ifdef PRN_NULLCHECK
#define nullcheck(a) (a ? a : "(NULL)")
#endif

	if (self)
	{
		qlog("CONN ERROR: func=%s, desc='%s', errnum=%d, errmsg='%s'\n", func, desc, self->__error_number, nullcheck(self->__error_message));
		mylog("CONN ERROR: func=%s, desc='%s', errnum=%d, errmsg='%s'\n", func, desc, self->__error_number, nullcheck(self->__error_message));
		qlog("            ------------------------------------------------------------\n");
		qlog("            henv=%u, conn=%u, status=%u, num_stmts=%d\n", self->henv, self, self->status, self->num_stmts);
		qlog("            sock=%u, stmts=%u, lobj_type=%d\n", self->sock, self->stmts, self->lobj_type);

		qlog("            ---------------- Socket Info -------------------------------\n");
		if (self->sock)
		{
			SocketClass *sock = self->sock;

			qlog("            socket=%d, reverse=%d, errornumber=%d, errormsg='%s'\n", sock->socket, sock->reverse, sock->errornumber, nullcheck(sock->errormsg));
			qlog("            buffer_in=%u, buffer_out=%u\n", sock->buffer_in, sock->buffer_out);
			qlog("            buffer_filled_in=%d, buffer_filled_out=%d, buffer_read_in=%d\n", sock->buffer_filled_in, sock->buffer_filled_out, sock->buffer_read_in);
		}
	}
	else
{
		qlog("INVALID CONNECTION HANDLE ERROR: func=%s, desc='%s'\n", func, desc);
		mylog("INVALID CONNECTION HANDLE ERROR: func=%s, desc='%s'\n", func, desc);
}
#undef PRN_NULLCHECK
}


int
CC_send_cancel_request(const ConnectionClass *conn)
{
	int			save_errno = SOCK_ERRNO;
	int			tmpsock = -1;
	struct
	{
		uint32		packetlen;
		CancelRequestPacket cp;
	}			crp;
	BOOL	ret = TRUE;

	/* Check we have an open connection */
	if (!conn || !conn->sock)
		return FALSE;

	/*
	 * We need to open a temporary connection to the postmaster. Use the
	 * information saved by connectDB to do this with only kernel calls.
	*/
	if ((tmpsock = socket(conn->sock->sadr->sa_family, SOCK_STREAM, 0)) < 0)
	{
		return FALSE;
	}
	if (connect(tmpsock, conn->sock->sadr, conn->sock->sadr_len) < 0)
	{
		closesocket(tmpsock);
		return FALSE;
	}

	/*
	 * We needn't set nonblocking I/O or NODELAY options here.
	 */
	crp.packetlen = htonl((uint32) sizeof(crp));
	crp.cp.cancelRequestCode = (MsgType) htonl(CANCEL_REQUEST_CODE);
	crp.cp.backendPID = htonl(conn->be_pid);
	crp.cp.cancelAuthCode = htonl(conn->be_key);

	if (send(tmpsock, (char *) &crp, sizeof(crp), 0) != (int) sizeof(crp))
	{
		save_errno = SOCK_ERRNO;
		ret = FALSE;
	}

	/* Sent it, done */
	closesocket(tmpsock);
	SOCK_ERRNO_SET(save_errno);

	return ret;
}

#else
/*
 *	Connection class implementation using libpq.
 *	Memory Allocation for PGconn is handled by libpq.
 */
ConnectionClass *
CC_Constructor()
{
	ConnectionClass *rv;

	rv = (ConnectionClass *) malloc(sizeof(ConnectionClass));

	if (rv != NULL)
	{
		rv->henv = NULL;		/* not yet associated with an environment */

		rv->__error_message = NULL;
		rv->__error_number = 0;
		rv->errormsg_created = FALSE;

		rv->status = CONN_NOT_CONNECTED;
		rv->transact_status = CONN_IN_AUTOCOMMIT;		/* autocommit by default */

		CC_conninfo_init(&(rv->connInfo));
		rv->stmts = (StatementClass **) malloc(sizeof(StatementClass *) * STMT_INCREMENT);
		if (!rv->stmts)
        {
            free(rv);
			return NULL;
        }
		memset(rv->stmts, 0, sizeof(StatementClass *) * STMT_INCREMENT);

		rv->num_stmts = STMT_INCREMENT;
		rv->descs = (DescriptorClass **) malloc(sizeof(DescriptorClass *) * STMT_INCREMENT);
		if (!rv->descs)
        {
            free(rv->stmts);
            free(rv);
			return NULL;
        }
		memset(rv->descs, 0, sizeof(DescriptorClass *) * STMT_INCREMENT);

		rv->num_descs = STMT_INCREMENT;

		rv->lobj_type = PG_TYPE_LO_UNDEFINED;

		rv->ntables = 0;
		rv->col_info = NULL;

		rv->translation_option = 0;
		rv->translation_handle = NULL;
		rv->DataSourceToDriver = NULL;
		rv->DriverToDataSource = NULL;
		rv->driver_version = ODBCVER;
		memset(rv->pg_version, 0, sizeof(rv->pg_version));
		rv->pg_version_number = .0;
		rv->pg_version_major = 0;
		rv->pg_version_minor = 0;
		rv->ms_jet = 0;
		rv->unicode = 0;
		rv->result_uncommitted = 0;
		rv->schema_support = 0;
		rv->isolation = SQL_TXN_READ_COMMITTED;
		rv->client_encoding = NULL;
		rv->server_encoding = NULL;
		rv->current_schema = NULL;
		rv->num_discardp = 0;
		rv->discardp = NULL;

		/* Initialize statement options to defaults */
		/* Statements under this conn will inherit these options */

		InitializeStatementOptions(&rv->stmtOptions);
		InitializeARDFields(&rv->ardOptions);
		InitializeAPDFields(&rv->apdOptions);
		INIT_CONN_CS(rv);
	}
	return rv;
}


char
CC_Destructor(ConnectionClass *self)
{
	mylog("enter CC_Destructor, self=%u\n", self);

	if (self->status == CONN_EXECUTING)
		return 0;

	CC_cleanup(self);			/* cleanup libpq connection class and statements */

	mylog("after CC_Cleanup\n");

	/* Free up statement holders */
	if (self->stmts)
	{
		free(self->stmts);
		self->stmts = NULL;
	}

	if (self->descs)
	{
		free(self->descs);
		self->descs = NULL;
	}

	mylog("after free statement holders\n");

	if (self->__error_message)
		free(self->__error_message);
	DELETE_CONN_CS(self);
	free(self);

	mylog("exit CC_Destructor\n");

	return 1;
}

/* This is called by SQLDisconnect also */
char
CC_cleanup(ConnectionClass *self)
{
	int			i;
	StatementClass *stmt;
	DescriptorClass *desc;

	if (self->status == CONN_EXECUTING)
		return FALSE;

	mylog("in CC_Cleanup, self=%u\n", self);

	/* Cancel an ongoing transaction */
	/* We are always in the middle of a transaction, */
	/* even if we are in auto commit. */
	if (self->pgconn)
	{
		CC_abort(self);

		mylog("after CC_abort\n");

		/* This closes the connection to the database */
		LIBPQ_Destructor(self->pgconn);
		self->pgconn = NULL;
	}

	mylog("after LIBPQ destructor\n");

	/* Free all the stmts on this connection */
	for (i = 0; i < self->num_stmts; i++)
	{
		stmt = self->stmts[i];
		if (stmt)
		{
			stmt->hdbc = NULL;	/* prevent any more dbase interactions */

			SC_Destructor(stmt);

			self->stmts[i] = NULL;
		}
	}

	/* Free all the descs on this connection */
	for (i = 0; i < self->num_descs; i++)
	{
		desc = self->descs[i];
		if (desc)
		{
			DC_get_conn(desc) = NULL;	/* prevent any more dbase interactions */
			DC_Destructor(desc);
			free(desc);
			self->descs[i] = NULL;
		}
	}

	/* Check for translation dll */
#ifdef WIN32
	if (self->translation_handle)
	{
		FreeLibrary(self->translation_handle);
		self->translation_handle = NULL;
	}
#endif

	self->status = CONN_NOT_CONNECTED;
	self->transact_status = CONN_IN_AUTOCOMMIT;
	CC_conninfo_init(&(self->connInfo));
	if (self->client_encoding)
	{
		free(self->client_encoding);
		self->client_encoding = NULL;
	}
	if (self->server_encoding)
	{
		free(self->server_encoding);
		self->server_encoding = NULL;
	}
	if (self->current_schema)
	{
		free(self->current_schema);
		self->current_schema = NULL;
	}
	/* Free cached table info */
	if (self->col_info)
	{
		for (i = 0; i < self->ntables; i++)
		{
			if (self->col_info[i]->result)	/* Free the SQLColumns result structure */
				QR_Destructor(self->col_info[i]->result);

			if (self->col_info[i]->schema)
				free(self->col_info[i]->schema);
			free(self->col_info[i]);
		}
		free(self->col_info);
		self->col_info = NULL;
	}
	self->ntables = 0;
	if (self->num_discardp > 0 && self->discardp)
	{
		for (i = 0; i < self->num_discardp; i++)
			free(self->discardp[i]);
		self->num_discardp = 0;
	}
	if (self->discardp)
	{
		free(self->discardp);
		self->discardp = NULL;
	}

	mylog("exit CC_Cleanup\n");
	return TRUE;
}


char
CC_connect(ConnectionClass *self, char password_req, char *salt_para)
{
	/* ignore salt_para for now */
	/* QResultClass *res; */
	PGconn *pgconn;
	ConnInfo   *ci = &(self->connInfo);
	int			areq = -1,connect_return;
	char	   *encoding;
	/* char	   *conninfo; */
	CSTR		func = "CC_connect";

	mylog("%s: entering...\n", func);

	if (password_req != AUTH_REQ_OK)

		/* already connected, just authenticate */
		pgconn = self->pgconn;

	else
	{
		qlog("Global Options: Version='%s', fetch=%d, socket=%d, unknown_sizes=%d, max_varchar_size=%d, max_longvarchar_size=%d\n",
			 POSTGRESDRIVERVERSION,
			 ci->drivers.fetch_max,
			 ci->drivers.socket_buffersize,
			 ci->drivers.unknown_sizes,
			 ci->drivers.max_varchar_size,
			 ci->drivers.max_longvarchar_size);
		qlog("                disable_optimizer=%d, ksqo=%d, unique_index=%d, use_declarefetch=%d\n",
			 ci->drivers.disable_optimizer,
			 ci->drivers.ksqo,
			 ci->drivers.unique_index,
			 ci->drivers.use_declarefetch);
		qlog("                text_as_longvarchar=%d, unknowns_as_longvarchar=%d, bools_as_char=%d NAMEDATALEN=%d\n",
			 ci->drivers.text_as_longvarchar,
			 ci->drivers.unknowns_as_longvarchar,
			 ci->drivers.bools_as_char,
			 TABLE_NAME_STORAGE_LEN);

		encoding = check_client_encoding(ci->conn_settings);
		if (encoding && strcmp(encoding, "OTHER"))
			self->client_encoding = strdup(encoding);
		else
		{
			encoding = check_client_encoding(ci->drivers.conn_settings);
			if (encoding && strcmp(encoding, "OTHER"))
				self->client_encoding = strdup(encoding);
		}
		if (self->client_encoding)
			self->ccsc = pg_CS_code(self->client_encoding);
		qlog("                extra_systable_prefixes='%s', conn_settings='%s' conn_encoding='%s'\n",
			 ci->drivers.extra_systable_prefixes,
			 ci->drivers.conn_settings,
			 encoding ? encoding : "");

		if (self->status != CONN_NOT_CONNECTED)
		{
			CC_set_error(self, CONN_OPENDB_ERROR, "Already connected.");
			return 0;
		}

		if (ci->port[0] == '\0' ||
#ifdef	WIN32
			ci->server[0] == '\0' ||
#endif /* WIN32 */
			ci->database[0] == '\0')
		{
			CC_set_error(self, CONN_INIREAD_ERROR, "Missing server name, port, or database name in call to CC_connect.");
			return 0;
		}

		mylog("CC_connect(): DSN = '%s', server = '%s', port = '%s', sslmode = '%s',"
		      " database = '%s', username = '%s',"
		      " password='%s'\n", ci->dsn, ci->server, ci->port, ci->sslmode,
		      ci->database, ci->username, ci->password ? "xxxxx" : "");


		mylog("connecting to the server \n");

		connect_return = LIBPQ_connect(self);
		if(0 == connect_return)
		{
			CC_set_error(self, CONNECTION_COULD_NOT_ESTABLISH, "Could not connect to the server");
			return 0;
		}

		mylog("connection to the database succeeded.\n");

	}

	CC_clear_error(self);		/* clear any password error */

	CC_set_translation(self);

	/*
	 * Send any initial settings
	 */

	/*
	 * Get the version number first so we can check it before sending options
	 * that are now obsolete. DJP 21/06/2002
	 */

	CC_lookup_pg_version(self);		/* Get PostgreSQL version for
						   SQLGetInfo use */
	/*
	 * Since these functions allocate statements, and since the connection
	 * is not established yet, it would violate odbc state transition
	 * rules.  Therefore, these functions call the corresponding local
	 * function instead.
	 */
	CC_send_settings(self);
	CC_clear_error(self);			/* clear any error */
	CC_lookup_lo(self);			/* a hack to get the oid of
						   our large object oid type */

	/*
	 *	Multibyte handling is available ?
	 */
	if (PG_VERSION_GE(self, 6.4))
	{
		CC_lookup_characterset(self);
		if (CC_get_errornumber(self) != 0)
			return 0;

		if (self->unicode)
		{
			if (!self->client_encoding ||
			    stricmp(self->client_encoding, "UNICODE"))
			{
				QResultClass	*res;
				if (PG_VERSION_LT(self, 7.1))
				{
					CC_set_error(self, CONN_NOT_IMPLEMENTED_ERROR, "UTF-8 conversion isn't implemented before 7.1");
					return 0;
				}
				if (self->client_encoding)
					free(self->client_encoding);
				self->client_encoding = NULL;
				res = LIBPQ_execute_query(self,"set client_encoding to 'UTF8'");
				if (res)
				{
					self->client_encoding = strdup("UNICODE");
					self->ccsc = pg_CS_code(self->client_encoding);
					QR_Destructor(res);

				}
			}
		}
	}
	else if (self->unicode)
	{
		CC_set_error(self, CONN_NOT_IMPLEMENTED_ERROR, "Unicode isn't supported before 6.4");
		return 0;
	}

	ci->updatable_cursors = 0;
#ifdef	DRIVER_CURSOR_IMPLEMENT
	if (!ci->drivers.use_declarefetch &&
		PG_VERSION_GE(self, 7.0)) /* Tid scan since 7.0 */
		ci->updatable_cursors = ci->allow_keyset;
#endif /* DRIVER_CURSOR_IMPLEMENT */

	CC_clear_error(self);		/* clear any initial command errors */
	self->status = CONN_CONNECTED;

	mylog("%s: returning...\n", func);

	return 1;

}


/*
 *	Create a more informative error message by concatenating the connection
 *	error message with its libpq error message.
 */
char *
CC_create_errormsg(ConnectionClass *self)
{
	PGconn *pgconn = self->pgconn;
	char	 msg[4096];

	mylog("enter CC_create_errormsg\n");

	msg[0] = '\0';

	if (CC_get_errormsg(self))
		strncpy(msg, CC_get_errormsg(self), sizeof(msg));

	mylog("msg = '%s'\n", msg);

	mylog("exit CC_create_errormsg\n");
	return msg ? strdup(msg) : NULL;
}


void	CC_on_abort(ConnectionClass *conn, UDWORD opt)
{
	BOOL	set_no_trans = FALSE;

	if (0 != (opt & CONN_DEAD))
		opt |= NO_TRANS;
	if (CC_is_in_trans(conn))
	{
#ifdef	DRIVER_CURSOR_IMPLEMENT
		if (conn->result_uncommitted)
			ProcessRollback(conn, TRUE);
#endif /* DRIVER_CURSOR_IMPLEMENT */
		if (0 != (opt & NO_TRANS))
		{
			CC_set_no_trans(conn);
			CC_set_no_manual_trans(conn);
			set_no_trans = TRUE;
		}
	}
	CC_clear_cursors(conn, TRUE);
	if (0 != (opt & CONN_DEAD))
	{
		conn->status = CONN_DOWN;
		if (conn->pgconn)
		{
			LIBPQ_Destructor(conn->pgconn);
			conn->pgconn = NULL;
		}
	}
	else if (set_no_trans)
		CC_discard_marked_plans(conn);
	conn->result_uncommitted = 0;
}

/*
 *	The "result_in" is only used by QR_next_tuple() to fetch another group of rows into
 *	the same existing QResultClass (this occurs when the tuple cache is depleted and
 *	needs to be re-filled).
 *
 *	The "cursor" is used by SQLExecute to associate a statement handle as the cursor name
 *	(i.e., C3326857) for SQL select statements.  This cursor is then used in future
 *	'declare cursor C3326857 for ...' and 'fetch 100 in C3326857' statements.
 */
QResultClass *
CC_send_query(ConnectionClass *self, char *query, QueryInfo *qi, UDWORD flag)
{
	QResultClass *cmdres = NULL,
			   *retres = NULL,
			   *res ;
	BOOL	clear_result_on_abort = ((flag & CLEAR_RESULT_ON_ABORT) != 0),
		create_keyset = ((flag & CREATE_KEYSET) != 0),
		issue_begin = ((flag & GO_INTO_TRANSACTION) != 0 && !CC_is_in_trans(self));
	char		 *wq;
	int			id=0;
	PGconn *pgconn = self->pgconn;
	int			maxlen,
				empty_reqs;
	BOOL		ReadyToReturn = FALSE,
				query_completed = FALSE,
				before_64 = PG_VERSION_LT(self, 6.4),
				aborted = FALSE,
				used_passed_result_object = FALSE;
	int		func_cs_count = 0;


	mylog("send_query(): conn=%u, query='%s'\n", self, query);
	qlog("conn=%u, query='%s'\n", self, query);

	if (!self->pgconn)
	{
		CC_set_error(self, CONNECTION_COULD_NOT_SEND, "Could not send Query(connection dead)");
		CC_on_abort(self, NO_TRANS);
		return NULL;
	}
	/* Indicate that we are sending a query to the backend */
	maxlen = CC_get_max_query_len(self);
	if (maxlen > 0 && maxlen < (int) strlen(query) + 1)
	{
		CC_set_error(self, CONNECTION_MSG_TOO_LONG, "Query string is too long");
		return NULL;
	}

	if ((NULL == query) || (query[0] == '\0'))
		return NULL;

#define	return DONT_CALL_RETURN_FROM_HERE???
	ENTER_INNER_CONN_CS(self, func_cs_count);

	if (issue_begin)
    {
		res = LIBPQ_execute_query(self,"BEGIN");
        QR_Destructor(res);
    }
    
	res = LIBPQ_execute_query(self,query);

	if((!res) || (res->status == PGRES_EMPTY_QUERY) )
	{
		QR_Destructor(res);
        res = NULL;
		goto cleanup;
	}
	else
	{
		mylog("send_query: done sending query\n");

		empty_reqs = 0;
		for (wq = query; isspace((UCHAR) *wq); wq++)
		;
		if (*wq == '\0')
			empty_reqs = 1;
		cmdres = qi ? qi->result_in : NULL;
		if (cmdres)
			used_passed_result_object = TRUE;
		if (!used_passed_result_object)
		{
			if (create_keyset)
				QR_set_haskeyset(res->next);
			if (!QR_fetch_tuples(res, self, qi ? qi->cursor : NULL))
			{
				CC_set_error(self, CONNECTION_COULD_NOT_RECEIVE, QR_get_message(res));
				if (PGRES_FATAL_ERROR == QR_get_status(res))
					retres = cmdres;
				else
					retres = NULL;
			}
		}
		else
		{				/* next fetch, so reuse an existing result */
			/*
			* called from QR_next_tuple and must return
			* immediately.
			*/
			if (!QR_fetch_tuples(res, NULL, NULL))
			{
				CC_set_error(self, CONNECTION_COULD_NOT_RECEIVE, QR_get_message(res));
				retres = NULL;
			}
			retres = cmdres;
		}
	}

cleanup:
	CLEANUP_FUNC_CONN_CS(func_cs_count, self);
	/*
	 * Cleanup garbage results before returning.
	 */
	if (cmdres && retres != cmdres && !used_passed_result_object)
	{
		QR_Destructor(cmdres);
	}
	/*
	 * Cleanup the aborted result if specified
	 */
	if (retres)
	{
		if (aborted)
		{
			if (clear_result_on_abort)
			{
	   			if (!used_passed_result_object)
				{
					QR_Destructor(retres);
					retres = NULL;
				}
			}
			if (retres)
			{
				/*
				 *	discard results other than errors.
				 */
				QResultClass	*qres;
				for (qres = retres; qres->next; qres = retres)
				{
					if (QR_get_aborted(qres))
						break;
					retres = qres->next;
					qres->next = NULL;
					QR_Destructor(qres);
				}
				/*
				 *	If error message isn't set
				 */
				if (retres && (!CC_get_errormsg(self) || !CC_get_errormsg(self)[0]))
					CC_set_errormsg(self, QR_get_message(retres));
			}
		}
	}
#undef	return
	return res;
}

int
CC_send_function(ConnectionClass *self, int fnid, void *result_buf, int *actual_result_len, int result_is_int, LO_ARG *args, int nargs)
{
	char			done;
	PGconn *pgconn=self->pgconn;


	mylog("send_function(): conn=%u, fnid=%d, result_is_int=%d, nargs=%d\n", self, fnid, result_is_int, nargs);

	if (!self->pgconn)
	{
		CC_set_error(self, CONNECTION_COULD_NOT_SEND, "Could not send function(connection dead)");
		CC_on_abort(self, NO_TRANS);
		return FALSE;
	}

	mylog("send_function: done sending function\n");

	/* Need to implement this */

	mylog("    done sending args\n");

	mylog("  after flush output\n");

	done = FALSE;
	return TRUE;
}


void
CC_log_error(const char *func, const char *desc, const ConnectionClass *self)
{
#ifdef PRN_NULLCHECK
#define nullcheck(a) (a ? a : "(NULL)")
#endif

	if (self)
	{
		qlog("CONN ERROR: func=%s, desc='%s', errnum=%d, errmsg='%s'\n", func, desc, self->__error_number, nullcheck(self->__error_message));
		mylog("CONN ERROR: func=%s, desc='%s', errnum=%d, errmsg='%s'\n", func, desc, self->__error_number, nullcheck(self->__error_message));
		qlog("            ------------------------------------------------------------\n");
		qlog("            henv=%u, conn=%u, status=%u, num_stmts=%d\n", self->henv, self, self->status, self->num_stmts);
	}
	else
	{
		qlog("INVALID CONNECTION HANDLE ERROR: func=%s, desc='%s'\n", func, desc);
		mylog("INVALID CONNECTION HANDLE ERROR: func=%s, desc='%s'\n", func, desc);
	}
#undef PRN_NULLCHECK
}


int
CC_send_cancel_request(const ConnectionClass *conn)
{
	int			ret = 0,errbufsize=256;
	char		errbuf[256];
	PGcancel	*cancel;


	cancel = PQgetCancel(conn->pgconn);
	if(!cancel)
	{
		PQfreeCancel(cancel);
		return FALSE;
	}
	ret=PQcancel(cancel, errbuf,errbufsize);
	if(1 == ret)
		return TRUE;
	else
	{
		PQfreeCancel(cancel);
		return FALSE;
	}
	return ret;
}


void
LIBPQ_Destructor(PGconn *pgconn)
{
	mylog("entering PGCONN_Destructor \n");
	PQfinish(pgconn);
	mylog("exiting PGCONN_Destructor \n");
}


int
LIBPQ_connect(ConnectionClass *self)
{
	char *conninfo;
	mylog("connecting to the database  using %s as the server\n",self->connInfo.server);
	if(self->connInfo.server != '\0')
	{
		conninfo = (char *)malloc((sizeof(char) * strlen(" host=") + strlen(self->connInfo.server) + 1));
		if(!conninfo)
		{
			CC_set_error(self, CONN_MEMORY_ALLOCATION_FAILED,"Could not allocate memory for connection string(server)");
			mylog("could not allocate memory for server \n");
		}
		conninfo = strcpy(conninfo," host=");
		conninfo = strcat(conninfo,self->connInfo.server);

	}
	mylog("the size is %d \n",strlen(conninfo));
	if(self->connInfo.port[0] != '\0')
	{
		size_t size=(sizeof(char) * (strlen(" port=") + strlen(self->connInfo.port) + 1));
		conninfo = (char *)realloc(conninfo,size+strlen(conninfo));
		if(!conninfo)
		{
			CC_set_error(self, CONN_MEMORY_ALLOCATION_FAILED,"Could not allocate memory for connection string(port)");
			mylog("could not allocate memory for port \n");
		}
		conninfo = strcat(conninfo," port=");
		conninfo = strcat(conninfo,self->connInfo.port);
	}


	if(self->connInfo.database[0] != '\0')
	{
		size_t size= (sizeof(char) * (strlen(" dbname=") + strlen(self->connInfo.database) + 1));
		conninfo = (char *)realloc(conninfo,size+strlen(conninfo));
		if(!conninfo)
		{
			CC_set_error(self, CONN_MEMORY_ALLOCATION_FAILED,"Could not allocate memory for connection string(database)");
			mylog("i could not allocate memory for dbname \n");
		}
		conninfo = strcat(conninfo," dbname=");
		conninfo = strcat(conninfo,self->connInfo.database);
	}


	if(self->connInfo.username[0] != '\0')
	{
		size_t size = (sizeof(char) * (strlen(" user=") + strlen(self->connInfo.username) + 1));
		conninfo = (char *)realloc(conninfo,size+strlen(conninfo));
		if(!conninfo)
		{
			CC_set_error(self, CONN_MEMORY_ALLOCATION_FAILED,"Could not allocate memory for connection string(username)");
			mylog("i could not allocate memory for username \n");
		}
		conninfo = strcat(conninfo," user=");
		conninfo = strcat(conninfo,self->connInfo.username);
	}


	if(self->connInfo.sslmode[0] != '\0')
	{
		size_t size = (sizeof(char) * (strlen(" sslmode=") + strlen(self->connInfo.sslmode) + 1));
		conninfo = (char *)realloc(conninfo,size+strlen(conninfo));
		if(!conninfo)
		{
			CC_set_error(self, CONN_MEMORY_ALLOCATION_FAILED,"Could not allocate memory for connection string(sslmode)");
			mylog("i could not allocate memory for sslmode \n");
		}
		conninfo = strcat(conninfo," sslmode=");
		conninfo = strcat(conninfo,self->connInfo.sslmode);
	}
    
	if(self->connInfo.password[0] != '\0')
	{
		size_t size = (sizeof(char) * (strlen(" password=") + strlen(self->connInfo.password) + 1));
		conninfo = (char *)realloc(conninfo,size+strlen(conninfo));
		if(!conninfo)
		{
			CC_set_error(self, CONN_MEMORY_ALLOCATION_FAILED,"Could not allocate memory for connection string(password)");
			mylog("i could not allocate memory for password \n");
		}
		conninfo = strcat(conninfo," password=");
		conninfo = strcat(conninfo,self->connInfo.password);
	}

	self->pgconn = PQconnectdb(conninfo);
	if (PQstatus(self->pgconn) != CONNECTION_OK)
	{
		CC_set_error(self,CONNECTION_COULD_NOT_ESTABLISH,PQerrorMessage(self->pgconn));
		mylog("could not establish connection to the database %s \n",PQerrorMessage(self->pgconn));
		PQfinish(self->pgconn);
        self->pgconn = NULL;
		free(conninfo);
		return 0;
	}
	/* free the conninfo structure */
	free(conninfo);
	mylog("connection to the database succeeded.\n");
	return 1;
}


QResultClass *
LIBPQ_execute_query(ConnectionClass *self,char *query)
{
	QResultClass	*qres;
	PGresult	*pgres;
	char		errbuffer[ERROR_MSG_LENGTH + 1];
	int		pos=0;
    
	pgres = PQexec(self->pgconn,query);

	qres=QR_Constructor();
	if(!qres)
	{
		CC_set_error(self, CONNECTION_COULD_NOT_RECEIVE, "Could not allocate memory for result set");
		QR_Destructor(qres);
        	return NULL;
	}
	qres->status = PQresultStatus(pgres);
	
	/* Check the connection status */
	if(PQstatus(self->pgconn) == CONNECTION_BAD)
	{
		snprintf(errbuffer, ERROR_MSG_LENGTH, "%s", PQerrorMessage(self->pgconn));
		/* Remove the training CR that libpq adds to the message */
		pos = strlen(errbuffer);
		if (pos)
			errbuffer[pos - 1] = '\0';
		
		mylog("The server could be dead: %s\n", errbuffer);
		CC_set_error(self, CONNECTION_COULD_NOT_SEND, errbuffer);
		CC_on_abort(self,CONN_DEAD);
		PQclear(pgres);
		return qres;
	}
    
	if (strnicmp(query, "BEGIN", 5) == 0)
	{
		CC_set_in_trans(self);
	}
	else if (strnicmp(query, "COMMIT", 6) == 0)
		CC_on_commit(self);
	else if (strnicmp(query, "ROLLBACK", 8) == 0)
	{
		/* 
		   The method of ROLLBACK an original form ....
		   ROLLBACK [ WORK | TRANSACTION ] TO [ SAVEPOINT ] savepoint_name
		 */
		if (PG_VERSION_LT(self, 8.0) || !(contains_token(query, " TO ")))
			CC_on_abort(self, NO_TRANS);
	}
	else if (strnicmp(query, "END", 3) == 0)
		CC_on_commit(self);
	else if (strnicmp(query, "ABORT", 5) == 0)
		CC_on_abort(self, NO_TRANS);
	else
		qres->recent_processed_row_count = atoi(PQcmdTuples(pgres));

	if( (PQresultStatus(pgres) == PGRES_COMMAND_OK) )
	{
		mylog("The query was executed successfully and the query did not return any result \n");
		PQclear(pgres);
		return qres;
	}

	if ( (PQresultStatus(pgres) != PGRES_EMPTY_QUERY) && (PQresultStatus(pgres) != PGRES_TUPLES_OK) )
	{
	        snprintf(errbuffer, ERROR_MSG_LENGTH, "%s", PQerrorMessage(self->pgconn));

	        /* Remove the training CR that libpq adds to the message */
	        pos = strlen(errbuffer);
	        if (pos)
	            errbuffer[pos - 1] = '\0';

		mylog("the server returned the error: %s\n", errbuffer);
		CC_set_error(self, CONNECTION_SERVER_REPORTED_ERROR, errbuffer);
	        PQclear(pgres);
		return qres;
	}

	qres=CC_mapping(self,pgres,qres);
	QR_set_command(qres, query);
	PQclear(pgres);
	return qres;
}

/*
 *	This function populates the manual_tuples of QResultClass using PGresult class.
 */

QResultClass *
CC_mapping(ConnectionClass *self, PGresult *pgres,QResultClass *qres)
{
	int i=0,j=0;
	TupleNode *node, *temp;
	Oid typid;
	int atttypmod,typlen;
	int num_attributes = PQnfields(pgres);
	int num_tuples = PQntuples(pgres);
    ConnInfo   *ci = &(self->connInfo);

	CI_set_num_fields(qres->fields, num_attributes);
	for(i = 0 ; i < num_attributes ; i++)
	{
		typid = PQftype(pgres,i);
		atttypmod = PQfmod(pgres,i);
        
        /* Setup the typmod */
		switch (typid)
		{
			case PG_TYPE_DATETIME:
			case PG_TYPE_TIMESTAMP_NO_TMZONE:
			case PG_TYPE_TIME:
			case PG_TYPE_TIME_WITH_TMZONE:
				break;
			default:
				atttypmod -= 4;
		}
		if (atttypmod < 0)
			atttypmod = -1;
        
        /* Setup the typlen */
        switch (typid)
        {
            case PG_TYPE_NUMERIC:
                typlen = (atttypmod >> 16) & 0xffff;
                break;
            
            case PG_TYPE_BPCHAR:
            case PG_TYPE_VARCHAR:
                typlen = atttypmod;
                break;
        
            default:
                typlen = PQfsize(pgres,i);
        }
        
        /* 
         * FIXME - The following is somewhat borked with regard to driver.unknownsizes
         *         It works, but basically is not aware of different variable length types
         *         (UNKNOWNS_AS_MAX), and UNKNOWNS_AS_LONGEST won't work because we don't
         *         have data at this point 
         */
		if(typlen == -1)
        {
            switch (ci->drivers.unknown_sizes)
            {
                case UNKNOWNS_AS_DONTKNOW:
				    typlen = SQL_NO_TOTAL;
                    break;
                
			    default:
				    typlen = ci->drivers.max_longvarchar_size;
            }
		}
        
		CI_set_field_info(qres->fields, i, PQfname(pgres,i),
			  typid, (Int2)typlen, atttypmod);
	}
	qres->manual_tuples = TL_Constructor(num_attributes);
	qres->manual_tuples->num_tuples = (Int4)num_tuples;
	for(i=0;i < num_tuples;i++)
	{
			node = (TupleNode *)malloc(sizeof(TupleNode) + (num_attributes ) * sizeof(TupleField));
			if(!node)
			{
				QR_set_status(qres, PGRES_FATAL_ERROR);
				QR_set_message(qres, "Error could not allocate memory for row.");
			}
			if (i==0)
			{
				qres->manual_tuples->list_start = qres->manual_tuples->list_end = node;
				qres->manual_tuples->lastref = node;
				qres->manual_tuples->last_indexed = 0;
				qres->manual_tuples->list_end->next = NULL;
			}
			else
			{
				temp = qres->manual_tuples->list_end;
				qres->manual_tuples->list_end->next = node;
				qres->manual_tuples->list_end = node;
				qres->manual_tuples->list_end->prev = temp;
				qres->manual_tuples->list_end->next = NULL;
			}
			for(j=0;j < num_attributes ;j++)
			{
				/* PQgetvalue returns an empty string even the data value is null. 
				  * An additional checking is provided to set the null value */
				if (PQgetisnull(pgres,i,j)) 
					set_tuplefield_null(&qres->manual_tuples->list_end->tuple[j]);
				else
					set_tuplefield_string(&qres->manual_tuples->list_end->tuple[j], PQgetvalue(pgres,i,j));
			}

	}
		return qres;
}

void
CC_is_server_alive(ConnectionClass *conn)
{
	PGresult *res;
	if((PQstatus(conn->pgconn)) != CONNECTION_OK)
		conn->status = CONN_NOT_CONNECTED;
	res = PQexec(conn->pgconn,"SELECT 1");
	if(PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		conn->status = CONN_DOWN;
	}
	else
	{
		PQclear(res);
		conn->status = CONN_CONNECTED;
	}

}


#endif /* USE_LIBPQ */



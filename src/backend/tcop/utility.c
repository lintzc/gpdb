/*-------------------------------------------------------------------------
 *
 * utility.c
 *	  Contains functions which control the execution of the POSTGRES utility
 *	  commands.  At one time acted as an interface between the Lisp and C
 *	  systems.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tcop/utility.c,v 1.289.2.3 2009/12/09 21:58:16 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/twophase.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/toasting.h"
#include "catalog/aoseg.h"
#include "catalog/aoblkdir.h"
#include "catalog/aovisimap.h"
#include "commands/alter.h"
#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/comment.h"
#include "commands/conversioncmds.h"
#include "commands/copy.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/discard.h"
#include "commands/explain.h"
#include "commands/extension.h"
#include "commands/extprotocolcmds.h"
#include "commands/filespace.h"
#include "commands/lockcmds.h"
#include "commands/portalcmds.h"
#include "commands/prepare.h"
#include "commands/queue.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "optimizer/planmain.h"
#include "parser/parse_utilcmd.h"
#include "postmaster/bgwriter.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteRemove.h"
#include "storage/fd.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/syscache.h"
#include "lib/stringinfo.h"

#include "cdb/cdbdisp_query.h"
#include "cdb/cdbpartition.h"
#include "cdb/cdbvars.h"

/*
 * Error-checking support for DROP commands
 */

struct msgstrings
{
	char		kind;
	int			nonexistent_code;
	const char *nonexistent_msg;
	const char *skipping_msg;
	const char *nota_msg;
	const char *drophint_msg;
};

static const struct msgstrings msgstringarray[] = {
	{RELKIND_RELATION,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("table \"%s\" does not exist"),
		gettext_noop("table \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a base table"),
	gettext_noop("Use DROP TABLE to remove a table, DROP EXTERNAL TABLE if external, or DROP FOREIGN TABLE if foreign.")},
	{RELKIND_SEQUENCE,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("sequence \"%s\" does not exist"),
		gettext_noop("sequence \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a sequence"),
	gettext_noop("Use DROP SEQUENCE to remove a sequence.")},
	{RELKIND_VIEW,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("view \"%s\" does not exist"),
		gettext_noop("view \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a view"),
	gettext_noop("Use DROP VIEW to remove a view.")},
	{RELKIND_INDEX,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("index \"%s\" does not exist"),
		gettext_noop("index \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not an index"),
	gettext_noop("Use DROP INDEX to remove an index.")},
	{RELKIND_COMPOSITE_TYPE,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("type \"%s\" does not exist"),
		gettext_noop("type \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a type"),
	gettext_noop("Use DROP TYPE to remove a type.")},
	{'\0', 0, NULL, NULL, NULL}
};


/*
 * Emit the right error message for a "DROP" command issued on a
 * relation of the wrong type
 */
static void
DropErrorMsgWrongType(char *relname, char wrongkind, char rightkind)
{
	const struct msgstrings *rentry;
	const struct msgstrings *wentry;

	for (rentry = msgstringarray; rentry->kind != '\0'; rentry++)
		if (rentry->kind == rightkind)
			break;
	Assert(rentry->kind != '\0');

	for (wentry = msgstringarray; wentry->kind != '\0'; wentry++)
		if (wentry->kind == wrongkind)
			break;
	/* wrongkind could be something we don't have in our table... */

	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg(rentry->nota_msg, relname),
			 (wentry->kind != '\0') ? errhint("%s", wentry->drophint_msg) : 0));
}

/*
 * Emit the right error message for a "DROP" command issued on a
 * non-existent relation
 */
void
DropErrorMsgNonExistent(const RangeVar *rel, char rightkind, bool missing_ok)
{
	const struct msgstrings *rentry;

	for (rentry = msgstringarray; rentry->kind != '\0'; rentry++)
	{
		if (rentry->kind == rightkind)
		{
			if (!missing_ok)
			{
				ereport(ERROR,
						(errcode(rentry->nonexistent_code),
						 errmsg(rentry->nonexistent_msg, rel->relname)));
			}
			else
			{
				if (Gp_role != GP_ROLE_EXECUTE)
					ereport(NOTICE, (errmsg(rentry->skipping_msg, rel->relname)));
				break;
			}
		}
	}

	Assert(rentry->kind != '\0');		/* Should be impossible */
}

/*
 * returns false if missing_ok is true and the object does not exist,
 * true if object exists and permissions are OK,
 * errors otherwise
 *
 */

static bool
CheckDropPermissions(RangeVar *rel, char rightkind, bool missing_ok)
{
	Oid			relOid;
	HeapTuple	tuple;
	Form_pg_class classform;

	relOid = RangeVarGetRelid(rel, true);
	if (!OidIsValid(relOid))
	{
		DropErrorMsgNonExistent(rel, rightkind, missing_ok);
		return false;
	}

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relOid);

	classform = (Form_pg_class) GETSTRUCT(tuple);

	if (classform->relkind != rightkind)
		DropErrorMsgWrongType(rel->relname, classform->relkind,
							  rightkind);

	/* Allow DROP to either table owner or schema owner */
	if (!pg_class_ownercheck(relOid, GetUserId()) &&
		!pg_namespace_ownercheck(classform->relnamespace, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   rel->relname);

	if (!allowSystemTableModsDDL && IsSystemClass(classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rel->relname)));

	ReleaseSysCache(tuple);

	return true;
}

/*
 * CheckDropRelStorage
 * 
 * Catch a mismatch between the DROP object type requested and the
 * actual object in the catalog. For example, if DROP EXTERNAL TABLE t
 * was issue, verify that t is indeed an external table, error if not.
 */
static bool
CheckDropRelStorage(RangeVar *rel, ObjectType removeType)
{
	Oid			relOid;
	HeapTuple	tuple;
	char		relstorage;

	relOid = RangeVarGetRelid(rel, true);
	
	if (!OidIsValid(relOid))
		return false;

	/* Find out the relstorage */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	relstorage = ((Form_pg_class) GETSTRUCT(tuple))->relstorage;
	ReleaseSysCache(tuple);

	/* 
	 * skip the check if it's external partition. 
	 * 1.remember rel_is_child_partition is only working on QD. 
	 * 2.we do the check on QD, no need to do it again on QE.
	 */
	if (relstorage == RELSTORAGE_EXTERNAL && (Gp_segment != -1 || rel_is_child_partition(relOid)))
		return true;

	if ((removeType == OBJECT_EXTTABLE && relstorage != RELSTORAGE_EXTERNAL) ||
		(removeType == OBJECT_TABLE && (relstorage == RELSTORAGE_EXTERNAL ||
										relstorage == RELSTORAGE_FOREIGN)))
	{
		/* we have a mismatch. format an error string and shoot */
		
		char *want_type;
		char *hint;
		
		if (removeType == OBJECT_EXTTABLE)
			want_type = pstrdup("an external");
		else
			want_type = pstrdup("a base");

		if (relstorage == RELSTORAGE_EXTERNAL)
			hint = pstrdup("Use DROP EXTERNAL TABLE to remove an external table");
		else if (relstorage == RELSTORAGE_FOREIGN)
			hint = pstrdup("Use DROP FOREIGN TABLE to remove a foreign table");
		else
			hint = pstrdup("Use DROP TABLE to remove a base table");
		
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not %s table", rel->relname, want_type),
				 errhint("%s", hint)));
	}
	

	return true;
}

/*
 * Verify user has ownership of specified relation, else ereport.
 *
 * If noCatalogs is true then we also deny access to system catalogs,
 * except when allowSystemTableModsDDL is true.
 */
void
CheckRelationOwnership(RangeVar *rel, bool noCatalogs)
{
	Oid			relOid;
	HeapTuple	tuple;

	relOid = RangeVarGetRelid(rel, false);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))		/* should not happen */
		elog(ERROR, "cache lookup failed for relation %u", relOid);

	if (!pg_class_ownercheck(relOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   rel->relname);

	if (noCatalogs)
	{
		if (!allowSystemTableModsDDL &&
			IsSystemClass((Form_pg_class) GETSTRUCT(tuple)))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied: \"%s\" is a system catalog",
							rel->relname)));
	}

	ReleaseSysCache(tuple);
}


/*
 * CommandIsReadOnly: is an executable query read-only?
 *
 * This is a much stricter test than we apply for XactReadOnly mode;
 * the query must be *in truth* read-only, because the caller wishes
 * not to do CommandCounterIncrement for it.
 *
 * Note: currently no need to support Query nodes here
 */
bool
CommandIsReadOnly(Node *node)
{
	if (IsA(node, PlannedStmt))
	{
		PlannedStmt *stmt = (PlannedStmt *) node;

		switch (stmt->commandType)
		{
			case CMD_SELECT:
				if (stmt->intoClause != NULL)
					return false;	/* SELECT INTO */
				else if (stmt->rowMarks != NIL)
					return false;	/* SELECT FOR UPDATE/SHARE */
				else
					return true;
			case CMD_UPDATE:
			case CMD_INSERT:
			case CMD_DELETE:
				return false;
			default:
				elog(WARNING, "unrecognized commandType: %d",
					 (int) stmt->commandType);
				break;
		}
	}
	/* For now, treat all utility commands as read/write */
	return false;
}

/*
 * check_xact_readonly: is a utility command read-only?
 *
 * Here we use the loose rules of XactReadOnly mode: no permanent effects
 * on the database are allowed.
 */
static void
check_xact_readonly(Node *parsetree)
{
	if (!XactReadOnly)
		return;

	/*
	 * Note: Commands that need to do more complicated checking are handled
	 * elsewhere, in particular COPY and plannable statements do their own
	 * checking.
	 */

	switch (nodeTag(parsetree))
	{
		case T_CreateStmt:
			{
				CreateStmt *createStmt;

				createStmt = (CreateStmt *) parsetree;

				if (createStmt->relation->istemp)
					return;		// Permit creation of TEMPORARY tables in read-only mode.

				ereport(ERROR,
						(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
						 errmsg("transaction is read-only")));
			}
			break;

		case T_DropStmt:
			{
				DropStmt   *dropStmt = (DropStmt *) parsetree;
				ListCell   *arg;

				/*
				 * So, if DROP TABLE is used, all objects must be
				 * temporary tables.
				 */
				foreach(arg, dropStmt->objects)
				{
					List	   *names = (List *) lfirst(arg);
					RangeVar   *rel;

					rel = makeRangeVarFromNameList(names);

					if (dropStmt->removeType != OBJECT_TABLE ||
					    !RelationToRemoveIsTemp(rel, dropStmt->missing_ok))
						ereport(ERROR,
								(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
								 errmsg("transaction is read-only")));

				}
			}
			return;		// All objects are TEMPORARY tables.

		case T_AlterDatabaseStmt:
		case T_AlterDatabaseSetStmt:
		case T_AlterDomainStmt:
		case T_AlterFunctionStmt:
		case T_AlterQueueStmt:
		case T_AlterRoleStmt:
		case T_AlterRoleSetStmt:
		case T_AlterObjectSchemaStmt:
		case T_AlterOwnerStmt:
		case T_AlterSeqStmt:
		case T_AlterTableStmt:
		case T_RenameStmt:
		case T_CommentStmt:
		case T_DefineStmt:
		case T_CreateCastStmt:
		case T_CreateConversionStmt:
		case T_CreatedbStmt:
		case T_CreateDomainStmt:
		case T_CreateFunctionStmt:
		case T_CreateQueueStmt:
		case T_CreateRoleStmt:
		case T_IndexStmt:
		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
		case T_CreatePLangStmt:
		case T_CreateOpClassStmt:
		case T_CreateOpFamilyStmt:
		case T_AlterOpFamilyStmt:
		case T_RuleStmt:
		case T_CreateSchemaStmt:
		case T_CreateSeqStmt:
		case T_CreateExternalStmt:
		case T_CreateFileSpaceStmt:
		case T_CreateTableSpaceStmt:
		case T_CreateTrigStmt:
		case T_CompositeTypeStmt:
		case T_CreateEnumStmt:
		case T_ViewStmt:
		case T_DropCastStmt:
		case T_DropdbStmt:
		case T_RemoveFuncStmt:
		case T_DropQueueStmt:
		case T_DropRoleStmt:
		case T_DropPLangStmt:
		case T_RemoveOpClassStmt:
		case T_RemoveOpFamilyStmt:
		case T_DropPropertyStmt:
		case T_GrantStmt:
		case T_GrantRoleStmt:
		case T_TruncateStmt:
		case T_DropOwnedStmt:
		case T_ReassignOwnedStmt:
		case T_AlterTSDictionaryStmt:
		case T_AlterTSConfigurationStmt:
			ereport(ERROR,
					(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
					 errmsg("transaction is read-only")));
			break;
		default:
			/* do nothing */
			break;
	}
}


/*
 * Process one relation in a drop statement.
 */
static bool
ProcessDropStatement(DropStmt *stmt)
{
	ListCell   *arg;
	bool		dispatchDrop = true;

	Assert(list_length(stmt->objects) == 1);

	foreach(arg, stmt->objects)
	{
		List	   *names = (List *) lfirst(arg);
		RangeVar   *rel;

		/*
		 * MPP-2879: We don't yet have locks, if we
		 * noticed that we don't have permission to drop
		 * on the QD, we *must* not dispatch -- we may be
		 * racing other DDL. Multiple creates/drops racing
		 * each other will produce very bad problems.
		 */
		switch (stmt->removeType)
		{
			case OBJECT_TABLE:
			case OBJECT_EXTTABLE:
				rel = makeRangeVarFromNameList(names);
				if (CheckDropPermissions(rel, RELKIND_RELATION,
										 stmt->missing_ok) &&
						CheckDropRelStorage(rel, stmt->removeType))
				{
					/*
					 * If RemoveRelation fails to find the relation on QD, it
					 * will return false and we should not dispatch the drop
					 * to segments as not holding Exclusive Lock.
					 */
					dispatchDrop = RemoveRelation(rel, stmt->behavior, stmt,
												  RELKIND_RELATION);
				}
				else
					dispatchDrop = false;
				break;

			case OBJECT_SEQUENCE:
				rel = makeRangeVarFromNameList(names);
				if (CheckDropPermissions(rel, RELKIND_SEQUENCE,
										 stmt->missing_ok))
					dispatchDrop = RemoveRelation(rel, stmt->behavior, stmt,
												  RELKIND_SEQUENCE);
				else
					dispatchDrop = false;
				break;

			case OBJECT_VIEW:
				rel = makeRangeVarFromNameList(names);
				if (CheckDropPermissions(rel, RELKIND_VIEW, stmt->missing_ok))
					RemoveView(rel, stmt->behavior);
				else
					dispatchDrop = false;
				break;

			case OBJECT_INDEX:
				rel = makeRangeVarFromNameList(names);
				if (CheckDropPermissions(rel, RELKIND_INDEX, stmt->missing_ok))
					RemoveIndex(rel, stmt->behavior);
				else
					dispatchDrop = false;
				break;

			case OBJECT_TYPE:
				/*
				 * RemoveType does its own permissions checks
				 */
				RemoveType(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_DOMAIN:
				/*
				 * RemoveDomain does its own permissions checks
				 */
				RemoveDomain(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_CONVERSION:
				DropConversionCommand(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_SCHEMA:
				/*
				 * RemoveSchema does its own permissions checks
				 */
				RemoveSchema(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_FILESPACE:
				/*
				 * RemoveFileSpace does its own permissions checks
				 */
				RemoveFileSpace(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_TABLESPACE:
				/*
				 * RemoveTableSpace does its own permissions checks
				 */
				RemoveTableSpace(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_EXTPROTOCOL:
				/*
				 * RemoveExtProtocol does its own permissions checks
				 */
				RemoveExtProtocol(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_TSPARSER:
				/*
				 * RemoveTSParser does its own permission checks
				 */
				RemoveTSParser(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_TSDICTIONARY:
				/*
				 * RemoveTSDictionary does its own permission checks
				 */
				RemoveTSDictionary(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_TSTEMPLATE:
				/*
				 * RemoveTSTemplate does its own permission checks
				 */
				RemoveTSTemplate(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_TSCONFIGURATION:
				/*
				 * RemoveTSConfiguration does its own permission checks
				 */
				RemoveTSConfiguration(names, stmt->behavior, stmt->missing_ok);
				break;

			case OBJECT_EXTENSION:
				/*
				 * RemoveExtension does its own permissions checks
				 */
				RemoveExtension(names, stmt->behavior, stmt->missing_ok);
				break;

			default:
				elog(ERROR, "unrecognized drop object type: %d",
					 (int) stmt->removeType);
				break;
		}

		/*
		 * We used to need to do CommandCounterIncrement() here,
		 * but now it's done inside performDeletion().
		 */
	}
	return dispatchDrop;
}

/*
 * CheckRestrictedOperation: throw error for hazardous command if we're
 * inside a security restriction context.
 *
 * This is needed to protect session-local state for which there is not any
 * better-defined protection mechanism, such as ownership.
 */
static void
CheckRestrictedOperation(const char *cmdname)
{
	if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 /* translator: %s is name of a SQL command, eg PREPARE */
				 errmsg("cannot execute %s within security-restricted operation",
						cmdname)));
}


/*
 * ProcessUtility
 *		general utility function invoker
 *
 *	parsetree: the parse tree for the utility statement
 *	queryString: original source text of command (NULL if not available)
 *	params: parameters to use during execution
 *	isTopLevel: true if executing a "top level" (interactively issued) command
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * Notes: as of PG 8.4, caller MUST supply a queryString; it is not
 * allowed anymore to pass NULL.  (If you really don't have source text,
 * you can pass a constant string, perhaps "(query not available)".)
 *
 * completionTag is only set nonempty if we want to return a nondefault status.
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
ProcessUtility(Node *parsetree,
			   const char *queryString,
			   ParamListInfo params,
			   bool isTopLevel,
			   DestReceiver *dest,
			   char *completionTag)
{
	Assert(queryString != NULL);	/* required as of 8.4 */

	check_xact_readonly(parsetree);

	if (completionTag)
		completionTag[0] = '\0';

	switch (nodeTag(parsetree))
	{
			/*
			 * ******************** transactions ********************
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
						/*
						 * START TRANSACTION, as defined by SQL99: Identical
						 * to BEGIN.  Same code for both.
						 */
					case TRANS_STMT_BEGIN:
					case TRANS_STMT_START:
						{
							ListCell   *lc;

							BeginTransactionBlock();
							foreach(lc, stmt->options)
							{
								DefElem    *item = (DefElem *) lfirst(lc);

								if (strcmp(item->defname, "transaction_isolation") == 0)
									SetPGVariableOptDispatch("transaction_isolation",
												  list_make1(item->arg),
												  true,
												  /* gp_dispatch */ false);
								else if (strcmp(item->defname, "transaction_read_only") == 0)
									SetPGVariableOptDispatch("transaction_read_only",
												  list_make1(item->arg),
												  true,
												  /* gp_dispatch */ false);
							}

							sendDtxExplicitBegin();
						}
						break;

					case TRANS_STMT_COMMIT:
						if (!EndTransactionBlock())
						{
							/* report unsuccessful commit in completionTag */
							if (completionTag)
								strcpy(completionTag, "ROLLBACK");
						}
						break;

					case TRANS_STMT_PREPARE:
						if (Gp_role == GP_ROLE_DISPATCH)
						{
							ereport(ERROR, (
											errcode(ERRCODE_GP_COMMAND_ERROR),
									 errmsg("PREPARE TRANSACTION is not yet supported in Greenplum Database.")
											));

						}
						if (!PrepareTransactionBlock(stmt->gid))
						{
							/* report unsuccessful commit in completionTag */
							if (completionTag)
								strcpy(completionTag, "ROLLBACK");
						}
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						if (Gp_role == GP_ROLE_DISPATCH)
						{
							ereport(ERROR, (
											errcode(ERRCODE_GP_COMMAND_ERROR),
									 errmsg("COMMIT PREPARED is not yet supported in Greenplum Database.")
											));

						}
						PreventTransactionChain(isTopLevel, "COMMIT PREPARED");
						FinishPreparedTransaction(stmt->gid, /* isCommit */ true, /* raiseErrorIfNotFound */ true);
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						if (Gp_role == GP_ROLE_DISPATCH)
						{
							ereport(ERROR, (
											errcode(ERRCODE_GP_COMMAND_ERROR),
									 errmsg("ROLLBACK PREPARED is not yet supported in Greenplum Database.")
											));

						}
						PreventTransactionChain(isTopLevel, "ROLLBACK PREPARED");
						FinishPreparedTransaction(stmt->gid, /* isCommit */ false, /* raiseErrorIfNotFound */ true);
						break;

					case TRANS_STMT_ROLLBACK:
						UserAbortTransactionBlock();
						break;

					case TRANS_STMT_SAVEPOINT:
						{
							ListCell   *cell;
							char	   *name = NULL;

							RequireTransactionChain(isTopLevel, "SAVEPOINT");

							foreach(cell, stmt->options)
							{
								DefElem    *elem = lfirst(cell);

								if (strcmp(elem->defname, "savepoint_name") == 0)
									name = strVal(elem->arg);
							}

							Assert(PointerIsValid(name));

							if (Gp_role == GP_ROLE_DISPATCH)
							{
								/* We already checked that we're in a
								 * transaction; need to make certain
								 * that the BEGIN has been dispatched
								 * before we start dispatching our savepoint.
								 */
								sendDtxExplicitBegin();
							}

							DefineDispatchSavepoint(
									name);
						}
						break;

					case TRANS_STMT_RELEASE:
						RequireTransactionChain(isTopLevel, "RELEASE SAVEPOINT");
						ReleaseSavepoint(stmt->options);
						break;

					case TRANS_STMT_ROLLBACK_TO:
						RequireTransactionChain(isTopLevel, "ROLLBACK TO SAVEPOINT");
						RollbackToSavepoint(stmt->options);

						/*
						 * CommitTransactionCommand is in charge of
						 * re-defining the savepoint again
						 */
						break;
				}
			}
			break;

			/*
			 * Portal (cursor) manipulation
			 *
			 * Note: DECLARE CURSOR is processed mostly as a SELECT, and
			 * therefore what we will get here is a PlannedStmt not a bare
			 * DeclareCursorStmt.
			 */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				if (stmt->utilityStmt == NULL ||
					!IsA(stmt->utilityStmt, DeclareCursorStmt))
					elog(ERROR, "non-DECLARE CURSOR PlannedStmt passed to ProcessUtility");
				PerformCursorOpen(stmt, params, queryString, isTopLevel);
			}
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				CheckRestrictedOperation("CLOSE");
				PerformPortalClose(stmt->portalname);
			}
			break;

		case T_FetchStmt:
			PerformPortalFetch((FetchStmt *) parsetree, dest,
							   completionTag);
			break;

			/*
			 * relation and attribute manipulation
			 */
		case T_CreateSchemaStmt:
			CreateSchemaCommand((CreateSchemaStmt *) parsetree,
								queryString);
			break;

		case T_CreateStmt:
			{
				List	   *stmts;
				ListCell   *l;
				Oid			relOid;

				/* Run parse analysis ... */
				/*
				 * GPDB: Only do parse analysis in the Query Dispatcher. The Executor
				 * nodes receive an already-transformed statement from the QD. We only
				 * want to process the main CreateStmt here, not any auxiliary IndexStmts
				 * or other such statements that would be created from the main
				 * CreateStmt by parse analysis. The QD will dispatch those other statements
				 * separately.
				 *
				 * Also, when processing an ALTER TABLE ADD PARTITION, atpxPartAddList()
				 * passes us an already-transformed statement.
				 */
				if (Gp_role == GP_ROLE_EXECUTE || ((CreateStmt *) parsetree)->is_add_part)
					stmts = list_make1(parsetree);
				else
					stmts = transformCreateStmt((CreateStmt *) parsetree,
												queryString, false);

				/* ... and do it */
				foreach(l, stmts)
				{
					Node	   *stmt = (Node *) lfirst(l);

					if (IsA(stmt, CreateStmt))
					{
						CreateStmt *cstmt = (CreateStmt *) stmt;
						char		relKind = RELKIND_RELATION;
						char		relStorage = RELSTORAGE_HEAP;

						/*
						 * If this T_CreateStmt was dispatched and we're a QE
						 * receiving it, extract the relkind and relstorage from
						 * it
						 */
						if (Gp_role == GP_ROLE_EXECUTE)
						{
							if (cstmt->relKind != 0)
								relKind = cstmt->relKind;

							if (cstmt->relStorage != 0)
								relStorage = cstmt->relStorage;

							/* sanity check */
							switch(relKind)
							{
								case RELKIND_VIEW:
								case RELKIND_COMPOSITE_TYPE:
									Assert(relStorage == RELSTORAGE_VIRTUAL);
									break;
								default:
									Assert(relStorage == RELSTORAGE_HEAP ||
										   relStorage == RELSTORAGE_AOROWS ||
										   relStorage == RELSTORAGE_AOCOLS ||
										   relStorage == RELSTORAGE_EXTERNAL ||
										   relStorage == RELSTORAGE_FOREIGN);
							}
						}

						/*
						 * Create the table itself. Don't dispatch it yet, as we haven't
						 * created the toast and other auxiliary tables yet.
						 */
						relOid = DefineRelation((CreateStmt *) stmt,
												relKind, relStorage, false);

						/*
						 * Let AlterTableCreateToastTable decide if this one
						 * needs a secondary relation too.
						 */
						CommandCounterIncrement();

						DefinePartitionedRelation((CreateStmt *) parsetree, relOid);

						if (relKind != RELKIND_COMPOSITE_TYPE)
						{
							AlterTableCreateToastTable(relOid,
													   cstmt->is_part_child);
							AlterTableCreateAoSegTable(relOid,
													   cstmt->is_part_child);

							if (cstmt->buildAoBlkdir)
								AlterTableCreateAoBlkdirTable(relOid, cstmt->is_part_child);

							AlterTableCreateAoVisimapTable(relOid,
														   cstmt->is_part_child);
						}

						if (Gp_role == GP_ROLE_DISPATCH)
							CdbDispatchUtilityStatement((Node *) stmt,
														DF_CANCEL_ON_ERROR |
														DF_NEED_TWO_PHASE |
														DF_WITH_SNAPSHOT,
														GetAssignedOidsForDispatch(),
														NULL);

						CommandCounterIncrement();
						/*
						 * Deferred statements should be evaluated *after* AO tables
						 * are updated correctly.  Otherwise, they may not have
						 * segment information yet and operations like create_index
						 * in the deferred statements cannot see the relfile.
						 */
						EvaluateDeferredStatements(cstmt->deferredStmts);
					}
					else
					{
						/* Recurse for anything else */
						ProcessUtility(stmt,
									   queryString,
									   params,
									   false,
									   None_Receiver,
									   NULL);
					}

					/* Need CCI between commands */
					if (lnext(l) != NULL)
						CommandCounterIncrement();
				}
			}
			break;

		case T_CreateExternalStmt:
			{
				List *stmts;
				ListCell   *l;

				/* Run parse analysis ... */
				/*
				 * GPDB: Only do parse analysis in the Query Dispatcher. The Executor
				 * nodes receive an already-transformed statement from the QD. We only
				 * want to process the main CreateExternalStmt here, other such
				 * statements that would be created from the main
				 * CreateExternalStmt by parse analysis. The QD will dispatch
				 * those other statements separately.
				 */
				if (Gp_role == GP_ROLE_EXECUTE)
					stmts = list_make1(parsetree);
				else
					stmts = transformCreateExternalStmt((CreateExternalStmt *) parsetree, queryString);

				/* ... and do it */
				foreach(l, stmts)
				{
					Node	   *stmt = (Node *) lfirst(l);

					if (IsA(stmt, CreateExternalStmt))
						DefineExternalRelation((CreateExternalStmt *) stmt);
					else
					{
						/* Recurse for anything else */
						ProcessUtility(stmt,
									   queryString,
									   params,
									   false,
									   None_Receiver,
									   NULL);
					}
				}
			}
			break;

		case T_CreateFileSpaceStmt:
			CreateFileSpace((CreateFileSpaceStmt *) parsetree);
			break;

		case T_CreateTableSpaceStmt:
			CreateTableSpace((CreateTableSpaceStmt *) parsetree);
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;
				ListCell   *arg;
				List       *objects;
				bool       if_exists;

				if_exists = stmt->missing_ok;
				objects = stmt->objects;

				/* we modify the object in the loop below, so make a copy */
				stmt = copyObject(stmt);

				foreach(arg, objects)
				{
					List	   *names = (List *) lfirst(arg);

					stmt->objects = NIL;
					stmt->objects = lappend(stmt->objects, list_copy(names));
					stmt->missing_ok = if_exists;

					if (ProcessDropStatement(stmt))
					{
						/*
						 * If we are the QD, dispatch this DROP command to all the QEs
						 */
						if (Gp_role == GP_ROLE_DISPATCH)
						{
							CdbDispatchUtilityStatement((Node *) stmt,
														DF_CANCEL_ON_ERROR|
														DF_WITH_SNAPSHOT|
														DF_NEED_TWO_PHASE,
														NIL, /* FIXME */
														NULL);
						}
					}
				}
			}
			break;

		case T_TruncateStmt:
			ExecuteTruncate((TruncateStmt *) parsetree);
			break;

		case T_CommentStmt:
			/* NOTE: Not currently dispatched to QEs */
			CommentObject((CommentStmt *) parsetree);
			break;

		case T_CopyStmt:
			{
				uint64		processed;

				processed = DoCopy((CopyStmt *) parsetree, queryString);
				if (completionTag)
					snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
							 "COPY " UINT64_FORMAT, processed);
			}
			break;

		case T_PrepareStmt:
			CheckRestrictedOperation("PREPARE");
			PrepareQuery((PrepareStmt *) parsetree, queryString);
			break;

		case T_ExecuteStmt:
			ExecuteQuery((ExecuteStmt *) parsetree, queryString, params,
						 dest, completionTag);
			break;

		case T_DeallocateStmt:
			CheckRestrictedOperation("DEALLOCATE");
			DeallocateQuery((DeallocateStmt *) parsetree);
			break;


			/*
			 * schema
			 */
		case T_RenameStmt:
			ExecRenameStmt((RenameStmt *) parsetree);
			break;

		case T_AlterObjectSchemaStmt:
			ExecAlterObjectSchemaStmt((AlterObjectSchemaStmt *) parsetree);
			break;

		case T_AlterOwnerStmt:
			ExecAlterOwnerStmt((AlterOwnerStmt *) parsetree);
			break;

		case T_AlterTableStmt:
			{
				List	   *stmts;
				ListCell   *l;

				/* Run parse analysis ... */
				/*
				 * GPDB: Like for CREATE TABLE, only do parse analysis in the Query Dispatcher.
				 */
				if (Gp_role == GP_ROLE_EXECUTE)
					stmts = list_make1(parsetree);
				else
					stmts = transformAlterTableStmt((AlterTableStmt *) parsetree,
													queryString);

				/* ... and do it */
				foreach(l, stmts)
				{
					Node	   *stmt = (Node *) lfirst(l);

					if (IsA(stmt, AlterTableStmt))
					{
						/* Do the table alteration proper */
						AlterTable((AlterTableStmt *) stmt);
					}
					else
					{
						/* Recurse for anything else */
						ProcessUtility(stmt,
									   queryString,
									   params,
									   false,
									   None_Receiver,
									   NULL);
					}

					/* Need CCI between commands */
					if (lnext(l) != NULL)
						CommandCounterIncrement();
				}
			}
			break;

		case T_AlterDomainStmt:
			{
				AlterDomainStmt *stmt = (AlterDomainStmt *) parsetree;

				/*
				 * Some or all of these functions are recursive to cover
				 * inherited things, so permission checks are done there.
				 */
				switch (stmt->subtype)
				{
					case 'T':	/* ALTER DOMAIN DEFAULT */

						/*
						 * Recursively alter column default for table and, if
						 * requested, for descendants
						 */
						AlterDomainDefault(stmt->typname,
										   stmt->def);
						break;
					case 'N':	/* ALTER DOMAIN DROP NOT NULL */
						AlterDomainNotNull(stmt->typname,
										   false);
						break;
					case 'O':	/* ALTER DOMAIN SET NOT NULL */
						AlterDomainNotNull(stmt->typname,
										   true);
						break;
					case 'C':	/* ADD CONSTRAINT */
						AlterDomainAddConstraint(stmt->typname,
												 stmt->def);
						break;
					case 'X':	/* DROP CONSTRAINT */
						AlterDomainDropConstraint(stmt->typname,
												  stmt->name,
												  stmt->behavior);
						break;
					default:	/* oops */
						elog(ERROR, "unrecognized alter domain type: %d",
							 (int) stmt->subtype);
						break;
				}

				if (Gp_role == GP_ROLE_DISPATCH)
				{
					/* ADD CONSTRAINT will assign a new OID for the constraint */
					CdbDispatchUtilityStatement((Node *) stmt,
												DF_CANCEL_ON_ERROR|
												DF_WITH_SNAPSHOT|
												DF_NEED_TWO_PHASE,
												GetAssignedOidsForDispatch(),
												NULL);
				}
			}
			break;

		case T_GrantStmt:
			ExecuteGrantStmt((GrantStmt *) parsetree);
			break;

		case T_GrantRoleStmt:
			GrantRole((GrantRoleStmt *) parsetree);
			break;

			/*
			 * **************** object creation / destruction *****************
			 */
		case T_DefineStmt:
			{
				DefineStmt *stmt = (DefineStmt *) parsetree;

				switch (stmt->kind)
				{
					case OBJECT_AGGREGATE:
						DefineAggregate(stmt->defnames, stmt->args,
										stmt->oldstyle, stmt->definition,
										stmt->ordered);
						break;
					case OBJECT_OPERATOR:
						Assert(stmt->args == NIL);
						DefineOperator(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TYPE:
						Assert(stmt->args == NIL);
						DefineType(stmt->defnames, stmt->definition);
						break;
					case OBJECT_EXTPROTOCOL:
						Assert(stmt->args == NIL);
						DefineExtProtocol(stmt->defnames, stmt->definition, stmt->trusted);
						break;						
					case OBJECT_TSPARSER:
						Assert(stmt->args == NIL);
						DefineTSParser(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSDICTIONARY:
						Assert(stmt->args == NIL);
						DefineTSDictionary(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSTEMPLATE:
						Assert(stmt->args == NIL);
						DefineTSTemplate(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSCONFIGURATION:
						Assert(stmt->args == NIL);
						DefineTSConfiguration(stmt->defnames, stmt->definition);
						break;
					default:
						elog(ERROR, "unrecognized define stmt type: %d",
							 (int) stmt->kind);
						break;
				}
			}
			break;

		case T_CompositeTypeStmt:		/* CREATE TYPE (composite) */
			{
				CompositeTypeStmt *stmt = (CompositeTypeStmt *) parsetree;

				DefineCompositeType(stmt->typevar, stmt->coldeflist);
			}
			break;

		case T_CreateEnumStmt:	/* CREATE TYPE (enum) */
			DefineEnum((CreateEnumStmt *) parsetree);
			break;

		case T_ViewStmt:		/* CREATE VIEW */
			DefineView((ViewStmt *) parsetree, queryString);
			break;

		case T_CreateFunctionStmt:		/* CREATE FUNCTION */
			CreateFunction((CreateFunctionStmt *) parsetree, queryString);
			break;

		case T_AlterFunctionStmt:		/* ALTER FUNCTION */
			AlterFunction((AlterFunctionStmt *) parsetree);
			break;

		case T_IndexStmt:		/* CREATE INDEX */
		{
			IndexStmt  *stmt = (IndexStmt *) parsetree;
			ListCell   *lc;
			List	   *stmts;

			/* Run parse analysis ... */
			stmts = transformIndexStmt(stmt, queryString);
			foreach(lc, stmts)
			{
				IndexStmt  *stmt = (IndexStmt *) lfirst(lc);

				if (stmt->concurrent)
					PreventTransactionChain(isTopLevel,
											"CREATE INDEX CONCURRENTLY");

				CheckRelationOwnership(stmt->relation, true);

				/* ... and do it */
				DefineIndex(stmt->relation,		/* relation */
							stmt->idxname,		/* index name */
							InvalidOid, /* no predefined OID */
							stmt->accessMethod, /* am name */
							stmt->tableSpace,
							stmt->indexParams,	/* parameters */
							(Expr *) stmt->whereClause,
							stmt->options,
							stmt->unique,
							stmt->primary,
							stmt->isconstraint,
							false,		/* is_alter_table */
							true,		/* check_rights */
							false,		/* skip_build */
							stmt->is_split_part,		/* quiet */
							stmt->concurrent,	/* concurrent */
							stmt);
			}
			break;
		}

		case T_CreateExtensionStmt:
			CreateExtension((CreateExtensionStmt *) parsetree);
			break;

		case T_AlterExtensionStmt:
			ExecAlterExtensionStmt((AlterExtensionStmt *) parsetree);
			break;

		case T_AlterExtensionContentsStmt:
			ExecAlterExtensionContentsStmt((AlterExtensionContentsStmt *) parsetree);
			break;

		case T_RuleStmt:		/* CREATE RULE */
			DefineRule((RuleStmt *) parsetree, queryString);
			if (Gp_role == GP_ROLE_DISPATCH)
			{
				CdbDispatchUtilityStatement((Node *) parsetree,
											DF_CANCEL_ON_ERROR|
											DF_WITH_SNAPSHOT|
											DF_NEED_TWO_PHASE,
											GetAssignedOidsForDispatch(),
											NULL);
			}
			break;

		case T_CreateSeqStmt:
			DefineSequence((CreateSeqStmt *) parsetree);
			break;

		case T_AlterSeqStmt:
			AlterSequence((AlterSeqStmt *) parsetree);
			break;

		case T_RemoveFuncStmt:
			{
				RemoveFuncStmt *stmt = (RemoveFuncStmt *) parsetree;

				switch (stmt->kind)
				{
					case OBJECT_FUNCTION:
						RemoveFunction(stmt);
						break;
					case OBJECT_AGGREGATE:
						RemoveAggregate(stmt);
						break;
					case OBJECT_OPERATOR:
						RemoveOperator(stmt);
						break;
					default:
						elog(ERROR, "unrecognized object type: %d",
							 (int) stmt->kind);
						break;
				}
			}
			break;

		case T_DoStmt:
			ExecuteDoStmt((DoStmt *) parsetree);
			break;

		case T_CreatedbStmt:
			if (Gp_role != GP_ROLE_EXECUTE)
			{
				/*
				 * Don't allow master to call this in a transaction block. Segments
				 * are ok as distributed transaction participants. 
				 */
				PreventTransactionChain(isTopLevel, "CREATE DATABASE");
			}
			createdb((CreatedbStmt *) parsetree);
			break;

		case T_AlterDatabaseStmt:
			AlterDatabase((AlterDatabaseStmt *) parsetree);
			break;

		case T_AlterDatabaseSetStmt:
			AlterDatabaseSet((AlterDatabaseSetStmt *) parsetree);
			break;

		case T_DropdbStmt:
			{
				DropdbStmt *stmt = (DropdbStmt *) parsetree;

				if (Gp_role != GP_ROLE_EXECUTE)
				{
					/*
					 * Don't allow master tp call this in a transaction block.  Segments are ok as
					 * distributed transaction participants. 
					 */
					PreventTransactionChain(isTopLevel, "DROP DATABASE");
				}
				dropdb(stmt->dbname, stmt->missing_ok);
			}
			break;

			/* Query-level asynchronous notification */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				if (Gp_role == GP_ROLE_EXECUTE)
					ereport(ERROR, (
								errcode(ERRCODE_GP_COMMAND_ERROR),
						 errmsg("Notify command cannot run in a function running on a segDB.")
								));

				Async_Notify(stmt->relation->relname);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				if (Gp_role == GP_ROLE_EXECUTE)
					ereport(ERROR,(errcode(ERRCODE_GP_COMMAND_ERROR),
							errmsg("Listen command cannot run in a function running on a segDB.")));

				CheckRestrictedOperation("LISTEN");
				Async_Listen(stmt->relation->relname);
			}
			break;

		case T_UnlistenStmt:
			{
				UnlistenStmt *stmt = (UnlistenStmt *) parsetree;

				if (Gp_role == GP_ROLE_EXECUTE)
					ereport(ERROR, (errcode(ERRCODE_GP_COMMAND_ERROR),
							errmsg("Unlisten command cannot run in a function running on a segDB.")));

				CheckRestrictedOperation("UNLISTEN");
				Async_Unlisten(stmt->relation->relname);
			}
			break;

		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;

				closeAllVfds(); /* probably not necessary... */
				/* Allowed names are restricted if you're not superuser */
				load_file(stmt->filename, !superuser());

				if (Gp_role == GP_ROLE_DISPATCH)
				{
					StringInfoData buffer;

					initStringInfo(&buffer);

					appendStringInfo(&buffer, "LOAD '%s'", stmt->filename);

					CdbDispatchCommand(buffer.data,
										DF_WITH_SNAPSHOT,
										NULL);
					pfree(buffer.data);
				}
			}
			break;

		case T_ClusterStmt:
			cluster((ClusterStmt *) parsetree, isTopLevel);
			break;

		case T_VacuumStmt:
			vacuum((VacuumStmt *) parsetree, NIL, NULL, false, isTopLevel);
			break;

		case T_ExplainStmt:
			ExplainQuery((ExplainStmt *) parsetree, queryString, params, dest);
			break;

		case T_VariableSetStmt:
			{
				VariableSetStmt *n = (VariableSetStmt *) parsetree;
				ExecSetVariableStmt(n);
					
				if (n->kind == VAR_RESET || n->kind == VAR_RESET_ALL)
				{
					if (Gp_role == GP_ROLE_DISPATCH)
					{
						/*
						 * RESET must be dispatched different, because it can't
						 * be in a user transaction
						 */
						StringInfoData buffer;

						initStringInfo(&buffer);

						if (n->kind == VAR_RESET_ALL)
							appendStringInfo(&buffer, "RESET ALL");
						else
							appendStringInfo(&buffer, "RESET %s", n->name);

						CdbDispatchCommand(buffer.data, DF_WITH_SNAPSHOT, NULL);
					}
				}
				else
				{
					/*
					 * Special cases for special SQL syntax that effectively sets
					 * more than one variable per statement.
					 */
					if (strcmp(n->name, "TRANSACTION") == 0)
					{
						ListCell   *head;

						foreach(head, n->args)
						{
							DefElem    *item = (DefElem *) lfirst(head);

							if (strcmp(item->defname, "transaction_isolation") == 0)
								SetPGVariableOptDispatch("transaction_isolation",
											  list_make1(item->arg), n->is_local,
											  /* gp_dispatch */ true);
							else if (strcmp(item->defname, "transaction_read_only") == 0)
								SetPGVariableOptDispatch("transaction_read_only",
											  list_make1(item->arg), n->is_local,
										      /* gp_dispatch */ true);
						}
					}
					else if (strcmp(n->name, "SESSION CHARACTERISTICS") == 0)
					{
						ListCell   *head;

						foreach(head, n->args)
						{
							DefElem    *item = (DefElem *) lfirst(head);

							if (strcmp(item->defname, "transaction_isolation") == 0)
								SetPGVariableOptDispatch("default_transaction_isolation",
											  list_make1(item->arg), n->is_local,
										      /* gp_dispatch */ true);
							else if (strcmp(item->defname, "transaction_read_only") == 0)
								SetPGVariableOptDispatch("default_transaction_read_only",
											  list_make1(item->arg), n->is_local,
											  /* gp_dispatch */ true);
						}
					}
					else
						SetPGVariableOptDispatch(n->name, n->args, n->is_local, /* gp_dispatch */ true);
				}
			}
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name, dest);
			}
			break;

		case T_DiscardStmt:
			/* should we allow DISCARD PLANS? */
			CheckRestrictedOperation("DISCARD");
			DiscardCommand((DiscardStmt *) parsetree, isTopLevel);
			break;

		case T_CreateTrigStmt:
			{
				Oid trigOid = CreateTrigger((CreateTrigStmt *) parsetree, InvalidOid);
				if (Gp_role == GP_ROLE_DISPATCH)
				{
					((CreateTrigStmt *) parsetree)->trigOid = trigOid;
					CdbDispatchUtilityStatement((Node *) parsetree,
												DF_CANCEL_ON_ERROR|
												DF_WITH_SNAPSHOT|
												DF_NEED_TWO_PHASE,
												GetAssignedOidsForDispatch(),
												NULL);
				}
			}
			break;

		case T_DropPropertyStmt:
			{
				DropPropertyStmt *stmt = (DropPropertyStmt *) parsetree;
				Oid			relId;

				relId = RangeVarGetRelid(stmt->relation, false);

				switch (stmt->removeType)
				{
					case OBJECT_RULE:
						/* RemoveRewriteRule checks permissions */
						RemoveRewriteRule(relId, stmt->property,
										  stmt->behavior, stmt->missing_ok);
						break;
					case OBJECT_TRIGGER:
						/* DropTrigger checks permissions */
						DropTrigger(relId, stmt->property,
									stmt->behavior, stmt->missing_ok);
						break;
					default:
						elog(ERROR, "unrecognized object type: %d",
							 (int) stmt->removeType);
						break;
				}
				if (Gp_role == GP_ROLE_DISPATCH)
				{
					CdbDispatchUtilityStatement((Node *) parsetree,
												DF_CANCEL_ON_ERROR|
												DF_WITH_SNAPSHOT|
												DF_NEED_TWO_PHASE,
												NIL, /* FIXME */
												NULL);
				}
			}
			break;

		case T_CreatePLangStmt:
			CreateProceduralLanguage((CreatePLangStmt *) parsetree);
			break;

		case T_DropPLangStmt:
			DropProceduralLanguage((DropPLangStmt *) parsetree);
			break;

			/*
			 * ******************************** DOMAIN statements ****
			 */
		case T_CreateDomainStmt:
			DefineDomain((CreateDomainStmt *) parsetree);
			break;

			/*
			 * ********************* RESOOURCE QUEUE statements ****
			 */
		case T_CreateQueueStmt:

			/*
			 * MPP-7960: We cannot run CREATE RESOURCE QUEUE inside a user
			 * transaction block because the shared memory structures are not
			 * cleaned up on abort, resulting in "leaked", unreachable queues.
			 */

			if (Gp_role == GP_ROLE_DISPATCH)
				PreventTransactionChain(isTopLevel, "CREATE RESOURCE QUEUE");

			CreateQueue((CreateQueueStmt *) parsetree);
			break;

		case T_AlterQueueStmt:
			AlterQueue((AlterQueueStmt *) parsetree);
			break;

		case T_DropQueueStmt:
			DropQueue((DropQueueStmt *) parsetree);
			break;

			/*
			 * ******************************** ROLE statements ****
			 */
		case T_CreateRoleStmt:
			CreateRole((CreateRoleStmt *) parsetree);
			break;

		case T_AlterRoleStmt:
			AlterRole((AlterRoleStmt *) parsetree);
			break;

		case T_AlterRoleSetStmt:
			AlterRoleSet((AlterRoleSetStmt *) parsetree);
			break;

		case T_DropRoleStmt:
			DropRole((DropRoleStmt *) parsetree);
			break;

		case T_DropOwnedStmt:
			DropOwnedObjects((DropOwnedStmt *) parsetree);
			break;

		case T_ReassignOwnedStmt:
			ReassignOwnedObjects((ReassignOwnedStmt *) parsetree);
			break;

		case T_LockStmt:
			LockTableCommand((LockStmt *) parsetree);
			break;

		case T_ConstraintsSetStmt:
			AfterTriggerSetState((ConstraintsSetStmt *) parsetree);
			break;

		case T_CheckPointStmt:
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("must be superuser to do CHECKPOINT")));

			if (Gp_role == GP_ROLE_DISPATCH)
			{
				CdbDispatchCommand("CHECKPOINT", DF_WITH_SNAPSHOT, NULL);
			}
			RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);
			break;

		case T_ReindexStmt:
			{
				ReindexStmt *stmt = (ReindexStmt *) parsetree;

				switch (stmt->kind)
				{
					case OBJECT_INDEX:
						ReindexIndex(stmt);
						break;
					case OBJECT_TABLE:
						ReindexTable(stmt);
						break;
					case OBJECT_DATABASE:

						/*
						 * This cannot run inside a user transaction block; if
						 * we were inside a transaction, then its commit- and
						 * start-transaction-command calls would not have the
						 * intended effect!
						 */
							if (Gp_role == GP_ROLE_DISPATCH)
								PreventTransactionChain(isTopLevel,
														"REINDEX DATABASE");
						ReindexDatabase(stmt);
						break;
					default:
						elog(ERROR, "unrecognized object type: %d",
							 (int) stmt->kind);
						break;
				}
			}
			break;

		case T_CreateConversionStmt:
			CreateConversionCommand((CreateConversionStmt *) parsetree);
			break;

		case T_CreateCastStmt:
			CreateCast((CreateCastStmt *) parsetree);
			break;

		case T_DropCastStmt:
			DropCast((DropCastStmt *) parsetree);
			break;

		case T_CreateOpClassStmt:
			DefineOpClass((CreateOpClassStmt *) parsetree);
			break;

		case T_CreateOpFamilyStmt:
			DefineOpFamily((CreateOpFamilyStmt *) parsetree);
			break;

		case T_AlterOpFamilyStmt:
			AlterOpFamily((AlterOpFamilyStmt *) parsetree);
			break;

		case T_RemoveOpClassStmt:
			RemoveOpClass((RemoveOpClassStmt *) parsetree);
			break;

		case T_RemoveOpFamilyStmt:
			RemoveOpFamily((RemoveOpFamilyStmt *) parsetree);
			break;

		case T_AlterTypeStmt:
			AlterType((AlterTypeStmt *) parsetree);
			break;

		case T_AlterTSDictionaryStmt:
			AlterTSDictionary((AlterTSDictionaryStmt *) parsetree);
			break;

		case T_AlterTSConfigurationStmt:
			AlterTSConfiguration((AlterTSConfigurationStmt *) parsetree);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			break;
	}
}

/*
 * UtilityReturnsTuples
 *		Return "true" if this utility statement will send output to the
 *		destination.
 *
 * Generally, there should be a case here for each case in ProcessUtility
 * where "dest" is passed on.
 */
bool
UtilityReturnsTuples(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return false;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return false;		/* not our business to raise error */
				return portal->tupDesc ? true : false;
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				if (stmt->into)
					return false;
				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return false;		/* not our business to raise error */
				if (entry->plansource->resultDesc)
					return true;
				return false;
			}

		case T_ExplainStmt:
			return true;

		case T_VariableShowStmt:
			return true;

		default:
			return false;
	}
}

/*
 * UtilityTupleDescriptor
 *		Fetch the actual output tuple descriptor for a utility statement
 *		for which UtilityReturnsTuples() previously returned "true".
 *
 * The returned descriptor is created in (or copied into) the current memory
 * context.
 */
TupleDesc
UtilityTupleDescriptor(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return NULL;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return NULL;	/* not our business to raise error */
				return CreateTupleDescCopy(portal->tupDesc);
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				if (stmt->into)
					return NULL;
				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return NULL;	/* not our business to raise error */
				return FetchPreparedStatementResultDesc(entry);
			}

		case T_ExplainStmt:
			return ExplainResultDesc((ExplainStmt *) parsetree);

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				return GetPGVariableResultDesc(n->name);
			}

		default:
			return NULL;
	}
}


/*
 * CreateCommandTag
 *		utility to get a string representation of the command operation,
 *		given either a raw (un-analyzed) parsetree or a planned query.
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 *
 * NB: all result strings must be shorter than COMPLETION_TAG_BUFSIZE.
 * Also, the result must point at a true constant (permanent storage).
 */
const char *
CreateCommandTag(Node *parsetree)
{
	const char *tag;

	switch (nodeTag(parsetree))
	{
			/* raw plannable queries */
		case T_InsertStmt:
			tag = "INSERT";
			break;

		case T_DeleteStmt:
			tag = "DELETE";
			break;

		case T_UpdateStmt:
			tag = "UPDATE";
			break;

		case T_SelectStmt:
			tag = "SELECT";
			break;

			/* utility statements --- same whether raw or cooked */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
						tag = "BEGIN";
						break;

					case TRANS_STMT_START:
						tag = "START TRANSACTION";
						break;

					case TRANS_STMT_COMMIT:
						tag = "COMMIT";
						break;

					case TRANS_STMT_ROLLBACK:
					case TRANS_STMT_ROLLBACK_TO:
						tag = "ROLLBACK";
						break;

					case TRANS_STMT_SAVEPOINT:
						tag = "SAVEPOINT";
						break;

					case TRANS_STMT_RELEASE:
						tag = "RELEASE";
						break;

					case TRANS_STMT_PREPARE:
						tag = "PREPARE TRANSACTION";
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						tag = "COMMIT PREPARED";
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						tag = "ROLLBACK PREPARED";
						break;

					default:
						tag = "???";
						break;
				}
			}
			break;

		case T_DeclareCursorStmt:
			tag = "DECLARE CURSOR";
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				if (stmt->portalname == NULL)
					tag = "CLOSE CURSOR ALL";
				else
					tag = "CLOSE CURSOR";
			}
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;

				tag = (stmt->ismove) ? "MOVE" : "FETCH";
			}
			break;

		case T_CreateDomainStmt:
			tag = "CREATE DOMAIN";
			break;

		case T_CreateSchemaStmt:
			tag = "CREATE SCHEMA";
			break;

		case T_CreateStmt:
			tag = "CREATE TABLE";
			break;

		case T_CreateExternalStmt:
			tag = "CREATE EXTERNAL TABLE";
			break;

		case T_CreateFileSpaceStmt:
			tag = "CREATE FILESPACE";
			break;

		case T_CreateTableSpaceStmt:
			tag = "CREATE TABLESPACE";
			break;

		case T_DropStmt:
			switch (((DropStmt *) parsetree)->removeType)
			{
				case OBJECT_TABLE:
					tag = "DROP TABLE";
					break;
				case OBJECT_EXTTABLE:
					tag = "DROP EXTERNAL TABLE";
					break;
				case OBJECT_SEQUENCE:
					tag = "DROP SEQUENCE";
					break;
				case OBJECT_VIEW:
					tag = "DROP VIEW";
					break;
				case OBJECT_INDEX:
					tag = "DROP INDEX";
					break;
				case OBJECT_TYPE:
					tag = "DROP TYPE";
					break;
				case OBJECT_DOMAIN:
					tag = "DROP DOMAIN";
					break;
				case OBJECT_CONVERSION:
					tag = "DROP CONVERSION";
					break;
				case OBJECT_SCHEMA:
					tag = "DROP SCHEMA";
					break;
				case OBJECT_FILESPACE:
					tag = "DROP FILESPACE";
					break;
				case OBJECT_TABLESPACE:
					tag = "DROP TABLESPACE";
					break;
				case OBJECT_EXTPROTOCOL:
					tag = "DROP PROTOCOL";
					break;					
				case OBJECT_TSPARSER:
					tag = "DROP TEXT SEARCH PARSER";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "DROP TEXT SEARCH DICTIONARY";
					break;
				case OBJECT_TSTEMPLATE:
					tag = "DROP TEXT SEARCH TEMPLATE";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "DROP TEXT SEARCH CONFIGURATION";
					break;
				case OBJECT_EXTENSION:
					tag = "DROP EXTENSION";
					break;
				default:
					tag = "???";
			}
			break;

		case T_TruncateStmt:
			tag = "TRUNCATE TABLE";
			break;

		case T_CommentStmt:
			tag = "COMMENT";
			break;

		case T_CopyStmt:
			tag = "COPY";
			break;

		case T_RenameStmt:
			switch (((RenameStmt *) parsetree)->renameType)
			{
				case OBJECT_AGGREGATE:
					tag = "ALTER AGGREGATE";
					break;
				case OBJECT_CONVERSION:
					tag = "ALTER CONVERSION";
					break;
				case OBJECT_DATABASE:
					tag = "ALTER DATABASE";
					break;
				case OBJECT_EXTPROTOCOL:
					tag = "ALTER PROTOCOL";
					break;
				case OBJECT_FUNCTION:
					tag = "ALTER FUNCTION";
					break;
				case OBJECT_INDEX:
					tag = "ALTER INDEX";
					break;
				case OBJECT_LANGUAGE:
					tag = "ALTER LANGUAGE";
					break;
				case OBJECT_OPCLASS:
					tag = "ALTER OPERATOR CLASS";
					break;
				case OBJECT_OPFAMILY:
					tag = "ALTER OPERATOR FAMILY";
					break;
				case OBJECT_ROLE:
					tag = "ALTER ROLE";
					break;
				case OBJECT_SCHEMA:
					tag = "ALTER SCHEMA";
					break;
				case OBJECT_SEQUENCE:
					tag = "ALTER SEQUENCE";
					break;
				case OBJECT_COLUMN:
				case OBJECT_TABLE:
					tag = "ALTER TABLE";
					break;
				case OBJECT_FILESPACE:
					tag = "ALTER FILESPACE";
					break;
				case OBJECT_TABLESPACE:
					tag = "ALTER TABLESPACE";
					break;
				case OBJECT_TRIGGER:
					tag = "ALTER TRIGGER";
					break;
				case OBJECT_VIEW:
					tag = "ALTER VIEW";
					break;
				case OBJECT_TSPARSER:
					tag = "ALTER TEXT SEARCH PARSER";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "ALTER TEXT SEARCH DICTIONARY";
					break;
				case OBJECT_TSTEMPLATE:
					tag = "ALTER TEXT SEARCH TEMPLATE";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "ALTER TEXT SEARCH CONFIGURATION";
					break;
				default:
					tag = "???";
					break;
			}
			break;

		case T_AlterObjectSchemaStmt:
			switch (((AlterObjectSchemaStmt *) parsetree)->objectType)
			{
				case OBJECT_AGGREGATE:
					tag = "ALTER AGGREGATE";
					break;
				case OBJECT_DOMAIN:
					tag = "ALTER DOMAIN";
					break;
				case OBJECT_EXTENSION:
					tag = "ALTER EXTENSION";
					break;
				case OBJECT_FUNCTION:
					tag = "ALTER FUNCTION";
					break;
				case OBJECT_SEQUENCE:
					tag = "ALTER SEQUENCE";
					break;
				case OBJECT_TABLE:
					tag = "ALTER TABLE";
					break;
				case OBJECT_TYPE:
					tag = "ALTER TYPE";
					break;
				case OBJECT_TSPARSER:
					tag = "ALTER TEXT SEARCH PARSER";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "ALTER TEXT SEARCH DICTIONARY";
					break;
				case OBJECT_TSTEMPLATE:
					tag = "ALTER TEXT SEARCH TEMPLATE";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "ALTER TEXT SEARCH CONFIGURATION";
					break;
				default:
					tag = "???";
					break;
			}
			break;

		case T_AlterOwnerStmt:
			switch (((AlterOwnerStmt *) parsetree)->objectType)
			{
				case OBJECT_AGGREGATE:
					tag = "ALTER AGGREGATE";
					break;
				case OBJECT_CONVERSION:
					tag = "ALTER CONVERSION";
					break;
				case OBJECT_DATABASE:
					tag = "ALTER DATABASE";
					break;
				case OBJECT_DOMAIN:
					tag = "ALTER DOMAIN";
					break;
				case OBJECT_EXTENSION:
					tag = "ALTER EXTENSION";
					break;
				case OBJECT_FUNCTION:
					tag = "ALTER FUNCTION";
					break;
				case OBJECT_LANGUAGE:
					tag = "ALTER LANGUAGE";
					break;
				case OBJECT_OPERATOR:
					tag = "ALTER OPERATOR";
					break;
				case OBJECT_OPCLASS:
					tag = "ALTER OPERATOR CLASS";
					break;
				case OBJECT_OPFAMILY:
					tag = "ALTER OPERATOR FAMILY";
					break;
				case OBJECT_SCHEMA:
					tag = "ALTER SCHEMA";
					break;
				case OBJECT_FILESPACE:
					tag = "ALTER FILESPACE";
					break;
				case OBJECT_TABLESPACE:
					tag = "ALTER TABLESPACE";
					break;
				case OBJECT_TYPE:
					tag = "ALTER TYPE";
					break;
				case OBJECT_EXTPROTOCOL:
					tag = "ALTER PROTOCOL";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "ALTER TEXT SEARCH CONFIGURATION";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "ALTER TEXT SEARCH DICTIONARY";
					break;
				default:
					tag = "???";
					break;
			}
			break;

		case T_AlterTableStmt:
			{
				AlterTableStmt *stmt = (AlterTableStmt *) parsetree;

				/*
				 * We might be supporting ALTER INDEX here, so set the
				 * completion tag appropriately. Catch all other possibilities
				 * with ALTER TABLE
				 */

				if (stmt->relkind == OBJECT_INDEX)
					tag = "ALTER INDEX";
				else if (stmt->relkind == OBJECT_EXTTABLE)
					tag = "ALTER EXTERNAL TABLE";
				else
					tag = "ALTER TABLE";
			}
			break;

		case T_AlterDomainStmt:
			tag = "ALTER DOMAIN";
			break;

		case T_AlterFunctionStmt:
			tag = "ALTER FUNCTION";
			break;

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				tag = (stmt->is_grant) ? "GRANT" : "REVOKE";
			}
			break;

		case T_GrantRoleStmt:
			{
				GrantRoleStmt *stmt = (GrantRoleStmt *) parsetree;

				tag = (stmt->is_grant) ? "GRANT ROLE" : "REVOKE ROLE";
			}
			break;

		case T_DefineStmt:
			switch (((DefineStmt *) parsetree)->kind)
			{
				case OBJECT_AGGREGATE:
					tag = "CREATE AGGREGATE";
					break;
				case OBJECT_OPERATOR:
					tag = "CREATE OPERATOR";
					break;
				case OBJECT_TYPE:
					tag = "CREATE TYPE";
					break;
				case OBJECT_EXTPROTOCOL:
					tag = "CREATE PROTOCOL";
					break;
				case OBJECT_TSPARSER:
					tag = "CREATE TEXT SEARCH PARSER";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "CREATE TEXT SEARCH DICTIONARY";
					break;
				case OBJECT_TSTEMPLATE:
					tag = "CREATE TEXT SEARCH TEMPLATE";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "CREATE TEXT SEARCH CONFIGURATION";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CompositeTypeStmt:
			tag = "CREATE TYPE";
			break;

		case T_CreateEnumStmt:
			tag = "CREATE TYPE";
			break;

		case T_ViewStmt:
			tag = "CREATE VIEW";
			break;

		case T_CreateFunctionStmt:
			tag = "CREATE FUNCTION";
			break;

		case T_IndexStmt:
			tag = "CREATE INDEX";
			break;

		case T_CreateExtensionStmt:
			tag = "CREATE EXTENSION";
			break;

		case T_AlterExtensionStmt:
			tag = "ALTER EXTENSION";
			break;

		case T_AlterExtensionContentsStmt:
			tag = "ALTER EXTENSION";
			break;

		case T_RuleStmt:
			tag = "CREATE RULE";
			break;

		case T_CreateSeqStmt:
			tag = "CREATE SEQUENCE";
			break;

		case T_AlterSeqStmt:
			tag = "ALTER SEQUENCE";
			break;

		case T_RemoveFuncStmt:
			switch (((RemoveFuncStmt *) parsetree)->kind)
			{
				case OBJECT_FUNCTION:
					tag = "DROP FUNCTION";
					break;
				case OBJECT_AGGREGATE:
					tag = "DROP AGGREGATE";
					break;
				case OBJECT_OPERATOR:
					tag = "DROP OPERATOR";
					break;
				default:
					tag = "???";
			}
			break;

		case T_DoStmt:
			tag = "DO";
			break;

		case T_CreatedbStmt:
			tag = "CREATE DATABASE";
			break;

		case T_AlterDatabaseStmt:
			tag = "ALTER DATABASE";
			break;

		case T_AlterDatabaseSetStmt:
			tag = "ALTER DATABASE";
			break;

		case T_DropdbStmt:
			tag = "DROP DATABASE";
			break;

		case T_NotifyStmt:
			tag = "NOTIFY";
			break;

		case T_ListenStmt:
			tag = "LISTEN";
			break;

		case T_UnlistenStmt:
			tag = "UNLISTEN";
			break;

		case T_LoadStmt:
			tag = "LOAD";
			break;

		case T_ClusterStmt:
			tag = "CLUSTER";
			break;

		case T_VacuumStmt:
			if (((VacuumStmt *) parsetree)->vacuum)
				tag = "VACUUM";
			else
				tag = "ANALYZE";
			break;

		case T_ExplainStmt:
			tag = "EXPLAIN";
			break;

		case T_VariableSetStmt:
			switch (((VariableSetStmt *) parsetree)->kind)
			{
				case VAR_SET_VALUE:
				case VAR_SET_CURRENT:
				case VAR_SET_DEFAULT:
				case VAR_SET_MULTI:
					tag = "SET";
					break;
				case VAR_RESET:
				case VAR_RESET_ALL:
					tag = "RESET";
					break;
				default:
					tag = "???";
			}
			break;

		case T_VariableShowStmt:
			tag = "SHOW";
			break;

		case T_DiscardStmt:
			switch (((DiscardStmt *) parsetree)->target)
			{
				case DISCARD_ALL:
					tag = "DISCARD ALL";
					break;
				case DISCARD_PLANS:
					tag = "DISCARD PLANS";
					break;
				case DISCARD_TEMP:
					tag = "DISCARD TEMP";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CreateTrigStmt:
			tag = "CREATE TRIGGER";
			break;

		case T_DropPropertyStmt:
			switch (((DropPropertyStmt *) parsetree)->removeType)
			{
				case OBJECT_TRIGGER:
					tag = "DROP TRIGGER";
					break;
				case OBJECT_RULE:
					tag = "DROP RULE";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CreatePLangStmt:
			tag = "CREATE LANGUAGE";
			break;

		case T_DropPLangStmt:
			tag = "DROP LANGUAGE";
			break;

		case T_CreateQueueStmt:
			tag = "CREATE QUEUE";
			break;

		case T_AlterQueueStmt:
			tag = "ALTER QUEUE";
			break;

		case T_DropQueueStmt:
			tag = "DROP QUEUE";
			break;

		case T_CreateRoleStmt:
			tag = "CREATE ROLE";
			break;

		case T_AlterRoleStmt:
			tag = "ALTER ROLE";
			break;

		case T_AlterRoleSetStmt:
			tag = "ALTER ROLE";
			break;

		case T_DropRoleStmt:
			tag = "DROP ROLE";
			break;

		case T_DropOwnedStmt:
			tag = "DROP OWNED";
			break;

		case T_ReassignOwnedStmt:
			tag = "REASSIGN OWNED";
			break;

		case T_LockStmt:
			tag = "LOCK TABLE";
			break;

		case T_ConstraintsSetStmt:
			tag = "SET CONSTRAINTS";
			break;

		case T_CheckPointStmt:
			tag = "CHECKPOINT";
			break;

		case T_ReindexStmt:
			tag = "REINDEX";
			break;

		case T_CreateConversionStmt:
			tag = "CREATE CONVERSION";
			break;

		case T_CreateCastStmt:
			tag = "CREATE CAST";
			break;

		case T_DropCastStmt:
			tag = "DROP CAST";
			break;

		case T_CreateOpClassStmt:
			tag = "CREATE OPERATOR CLASS";
			break;

		case T_CreateOpFamilyStmt:
			tag = "CREATE OPERATOR FAMILY";
			break;

		case T_AlterOpFamilyStmt:
			tag = "ALTER OPERATOR FAMILY";
			break;

		case T_RemoveOpClassStmt:
			tag = "DROP OPERATOR CLASS";
			break;

		case T_RemoveOpFamilyStmt:
			tag = "DROP OPERATOR FAMILY";
			break;

		case T_AlterTSDictionaryStmt:
			tag = "ALTER TEXT SEARCH DICTIONARY";
			break;

		case T_AlterTSConfigurationStmt:
			tag = "ALTER TEXT SEARCH CONFIGURATION";
			break;

		case T_PrepareStmt:
			tag = "PREPARE";
			break;

		case T_ExecuteStmt:
			tag = "EXECUTE";
			break;

		case T_DeallocateStmt:
			{
				DeallocateStmt *stmt = (DeallocateStmt *) parsetree;

				if (stmt->name == NULL)
					tag = "DEALLOCATE ALL";
				else
					tag = "DEALLOCATE";
			}
			break;

			/* already-planned queries */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 */
						if (stmt->utilityStmt != NULL)
						{
							Assert(IsA(stmt->utilityStmt, DeclareCursorStmt));
							tag = "DECLARE CURSOR";
						}
						else if (stmt->intoClause != NULL)
							tag = "SELECT INTO";
						else if (stmt->rowMarks != NIL)
						{
							if (((RowMarkClause *) linitial(stmt->rowMarks))->forUpdate)
								tag = "SELECT FOR UPDATE";
							else
								tag = "SELECT FOR SHARE";
						}
						else
							tag = "SELECT";
						break;
					case CMD_UPDATE:
						tag = "UPDATE";
						break;
					case CMD_INSERT:
						tag = "INSERT";
						break;
					case CMD_DELETE:
						tag = "DELETE";
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = "???";
						break;
				}
			}
			break;

		case T_Query: /* used to be function CreateQueryTag */
			{
				Query *query = (Query*)parsetree;
				
				switch (query->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 */
						if (query->intoClause != NULL)
							tag = "SELECT INTO";
						else if (query->rowMarks != NIL)
						{
							if (((RowMarkClause *) linitial(query->rowMarks))->forUpdate)
								tag = "SELECT FOR UPDATE";
							else
								tag = "SELECT FOR SHARE";
						}
						else
							tag = "SELECT";
						break;
					case CMD_UPDATE:
						tag = "UPDATE";
						break;
					case CMD_INSERT:
						tag = "INSERT";
						break;
					case CMD_DELETE:
						tag = "DELETE";
						break;
					case CMD_UTILITY:
						tag = CreateCommandTag(query->utilityStmt);
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) query->commandType);
						tag = "???";
						break;
				}
			}
			break;

		case T_AlterTypeStmt:
			tag = "ALTER TYPE";
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			Assert(false);
			tag = "???";
			break;
	}

	return tag;
}


/*
 * GetCommandLogLevel
 *		utility to get the minimum log_statement level for a command,
 *		given either a raw (un-analyzed) parsetree or a planned query.
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 */
LogStmtLevel
GetCommandLogLevel(Node *parsetree)
{
	LogStmtLevel lev;

	switch (nodeTag(parsetree))
	{
			/* raw plannable queries */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_SelectStmt:
			if (((SelectStmt *) parsetree)->intoClause)
				lev = LOGSTMT_DDL;		/* CREATE AS, SELECT INTO */
			else
				lev = LOGSTMT_ALL;
			break;

			/* utility statements --- same whether raw or cooked */
		case T_TransactionStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DeclareCursorStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClosePortalStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_FetchStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateExternalStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFileSpaceStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTableSpaceStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_TruncateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_CommentStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CopyStmt:
			if (((CopyStmt *) parsetree)->is_from)
				lev = LOGSTMT_MOD;
			else
				lev = LOGSTMT_ALL;
			break;

		case T_RenameStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOwnerStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DefineStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CompositeTypeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_IndexStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RuleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RemoveFuncStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DoStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreatedbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropdbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_NotifyStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ListenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_UnlistenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_LoadStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClusterStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VacuumStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ExplainStmt:
			{
				ExplainStmt *stmt = (ExplainStmt *) parsetree;

				/* Look through an EXPLAIN ANALYZE to the contained stmt */
				if (stmt->analyze)
					return GetCommandLogLevel(stmt->query);
				/* Plain EXPLAIN isn't so interesting */
				lev = LOGSTMT_ALL;
			}
			break;

		case T_VariableSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_VariableShowStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DiscardStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropPropertyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePLangStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropPLangStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ReassignOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_LockStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ConstraintsSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CheckPointStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ReindexStmt:
			lev = LOGSTMT_ALL;	/* should this be DDL? */
			break;

		case T_CreateConversionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateCastStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropCastStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpClassStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RemoveOpClassStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RemoveOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSDictionaryStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSConfigurationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_PrepareStmt:
			{
				PrepareStmt *stmt = (PrepareStmt *) parsetree;

				/* Look through a PREPARE to the contained stmt */
				lev = GetCommandLogLevel(stmt->query);
			}
			break;

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *ps;

				/* Look through an EXECUTE to the referenced stmt */
				ps = FetchPreparedStatement(stmt->name, false);
				if (ps)
					lev = GetCommandLogLevel(ps->plansource->raw_parse_tree);
				else
					lev = LOGSTMT_ALL;
			}
			break;

		case T_DeallocateStmt:
			lev = LOGSTMT_ALL;
			break;

			/* already-planned queries */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						if (stmt->intoClause != NULL)
							lev = LOGSTMT_DDL;	/* CREATE AS, SELECT INTO */
						else
							lev = LOGSTMT_ALL;	/* SELECT or DECLARE CURSOR */
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
						lev = LOGSTMT_MOD;
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}
			}
			break;

			/* parsed-and-rewritten-but-not-planned queries */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						if (stmt->intoClause != NULL)
							lev = LOGSTMT_DDL;	/* CREATE AS, SELECT INTO */
						else
							lev = LOGSTMT_ALL;	/* SELECT or DECLARE CURSOR */
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
						lev = LOGSTMT_MOD;
						break;

					case CMD_UTILITY:
						lev = GetCommandLogLevel(stmt->utilityStmt);
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}

			}
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			lev = LOGSTMT_ALL;
			break;
	}

	return lev;
}

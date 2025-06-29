/*-------------------------------------------------------------------------
 *
 * collationcmds.c
 *	  collation-related commands support code
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/collationcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "catalog/oid_dispatch.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp_query.h"
#include "storage/fd.h"


typedef struct
{
	char	   *localename;		/* name of locale, as per "locale -a" */
	char	   *alias;			/* shortened alias for same */
	int			enc;			/* encoding */
} CollAliasData;


/*
 * CREATE COLLATION
 */
ObjectAddress
DefineCollation(ParseState *pstate, List *names, List *parameters, bool if_not_exists)
{
	char	   *collName;
	Oid			collNamespace;
	AclResult	aclresult;
	ListCell   *pl;
	DefElem    *fromEl = NULL;
	DefElem    *localeEl = NULL;
	DefElem    *lccollateEl = NULL;
	DefElem    *lcctypeEl = NULL;
	DefElem    *providerEl = NULL;
	DefElem    *deterministicEl = NULL;
	DefElem    *versionEl = NULL;
	DefElem    *commentEl = NULL;
	char	   *collcollate = NULL;
	char	   *collctype = NULL;
	char	   *collproviderstr = NULL;
	bool		collisdeterministic = true;
	int			collencoding = 0;
	char		collprovider = 0;
	char	   *collversion = NULL;
	char	   *collcomment = NULL;
	Oid			newoid;
	ObjectAddress address;

	collNamespace = QualifiedNameGetCreationNamespace(names, &collName);

	aclresult = pg_namespace_aclcheck(collNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(collNamespace));

	foreach(pl, parameters)
	{
		DefElem    *defel = lfirst_node(DefElem, pl);
		DefElem   **defelp;

		if (strcmp(defel->defname, "from") == 0)
			defelp = &fromEl;
		else if (strcmp(defel->defname, "locale") == 0)
			defelp = &localeEl;
		else if (strcmp(defel->defname, "lc_collate") == 0)
			defelp = &lccollateEl;
		else if (strcmp(defel->defname, "lc_ctype") == 0)
			defelp = &lcctypeEl;
		else if (strcmp(defel->defname, "provider") == 0)
			defelp = &providerEl;
		else if (strcmp(defel->defname, "deterministic") == 0)
			defelp = &deterministicEl;
		else if (strcmp(defel->defname, "version") == 0)
			defelp = &versionEl;
		else if (strcmp(defel->defname, "comment") == 0)
			defelp = &commentEl;
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("collation attribute \"%s\" not recognized",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
			break;
		}

		*defelp = defel;
	}

	if ((localeEl && (lccollateEl || lcctypeEl))
		|| (fromEl && list_length(parameters) != 1))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("conflicting or redundant options")));

	if (fromEl)
	{
		Oid			collid;
		HeapTuple	tp;

		collid = get_collation_oid(defGetQualifiedName(fromEl), false);
		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);

		collcollate = pstrdup(NameStr(((Form_pg_collation) GETSTRUCT(tp))->collcollate));
		collctype = pstrdup(NameStr(((Form_pg_collation) GETSTRUCT(tp))->collctype));
		collprovider = ((Form_pg_collation) GETSTRUCT(tp))->collprovider;
		collisdeterministic = ((Form_pg_collation) GETSTRUCT(tp))->collisdeterministic;
		collencoding = ((Form_pg_collation) GETSTRUCT(tp))->collencoding;

		ReleaseSysCache(tp);

		/*
		 * Copying the "default" collation is not allowed because most code
		 * checks for DEFAULT_COLLATION_OID instead of COLLPROVIDER_DEFAULT,
		 * and so having a second collation with COLLPROVIDER_DEFAULT would
		 * not work and potentially confuse or crash some code.  This could be
		 * fixed with some legwork.
		 */
		if (collprovider == COLLPROVIDER_DEFAULT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("collation \"default\" cannot be copied")));
	}

	if (localeEl)
	{
		collcollate = defGetString(localeEl);
		collctype = defGetString(localeEl);
	}

	if (lccollateEl)
		collcollate = defGetString(lccollateEl);

	if (lcctypeEl)
		collctype = defGetString(lcctypeEl);

	if (providerEl)
		collproviderstr = defGetString(providerEl);

	if (commentEl)
		collcomment = defGetString(commentEl);

	if (deterministicEl)
		collisdeterministic = defGetBoolean(deterministicEl);

	if (versionEl)
		collversion = defGetString(versionEl);

	if (collproviderstr)
	{
		if (pg_strcasecmp(collproviderstr, "icu") == 0)
			collprovider = COLLPROVIDER_ICU;
		else if (pg_strcasecmp(collproviderstr, "libc") == 0)
			collprovider = COLLPROVIDER_LIBC;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("unrecognized collation provider: %s",
							collproviderstr)));
	}
	else if (!fromEl)
		collprovider = COLLPROVIDER_LIBC;

	if (!collcollate)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("parameter \"lc_collate\" must be specified")));

	if (!collctype)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("parameter \"lc_ctype\" must be specified")));

	/*
	 * Nondeterministic collations are currently only supported with ICU
	 * because that's the only case where it can actually make a difference.
	 * So we can save writing the code for the other providers.
	 */
	if (!collisdeterministic && collprovider != COLLPROVIDER_ICU)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nondeterministic collations not supported with this provider")));

	if (!fromEl)
	{
		if (collprovider == COLLPROVIDER_ICU)
		{
#ifdef USE_ICU
			/*
			 * We could create ICU collations with collencoding == database
			 * encoding, but it seems better to use -1 so that it matches the
			 * way initdb would create ICU collations.  However, only allow
			 * one to be created when the current database's encoding is
			 * supported.  Otherwise the collation is useless, plus we get
			 * surprising behaviors like not being able to drop the collation.
			 *
			 * Skip this test when !USE_ICU, because the error we want to
			 * throw for that isn't thrown till later.
			 */
			if (!is_encoding_supported_by_icu(GetDatabaseEncoding()))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("current database's encoding is not supported with this provider")));
#endif
			collencoding = -1;
		}
		else
		{
			collencoding = GetDatabaseEncoding();
			check_encoding_locale_matches(collencoding, collcollate, collctype);
		}
	}

	if (!collversion)
		collversion = get_collation_actual_version(collprovider, collcollate);

	newoid = CollationCreate(collName,
							 collNamespace,
							 GetUserId(),
							 collprovider,
							 collisdeterministic,
							 collencoding,
							 collcollate,
							 collctype,
							 collversion,
							 if_not_exists,
							 false);	/* not quiet */

	if (!OidIsValid(newoid))
		return InvalidObjectAddress;

	/*
	 * Check that the locales can be loaded.  NB: pg_newlocale_from_collation
	 * is only supposed to be called on non-C-equivalent locales.
	 */
	CommandCounterIncrement();
	if (!lc_collate_is_c(newoid) || !lc_ctype_is_c(newoid))
		(void) pg_newlocale_from_collation(newoid);

	if (collcomment)
		CreateComments(newoid, CollationRelationId, 0, collcomment);

	ObjectAddressSet(address, CollationRelationId, newoid);

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		DefineStmt * stmt = makeNode(DefineStmt);
		stmt->kind = OBJECT_COLLATION;
		stmt->oldstyle = false;
		stmt->defnames = names;
		stmt->args = NIL;
		stmt->definition = parameters;
		stmt->trusted = false;
		CdbDispatchUtilityStatement((Node *) stmt,
		                            DF_CANCEL_ON_ERROR|
			                        DF_WITH_SNAPSHOT|
			                        DF_NEED_TWO_PHASE,
		                            GetAssignedOidsForDispatch(),
		                            NULL);
	}

	return address;
}

/*
 * Subroutine for ALTER COLLATION SET SCHEMA and RENAME
 *
 * Is there a collation with the same name of the given collation already in
 * the given namespace?  If so, raise an appropriate error message.
 */
void
IsThereCollationInNamespace(const char *collname, Oid nspOid)
{
	/* make sure the name doesn't already exist in new schema */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collname),
							  Int32GetDatum(GetDatabaseEncoding()),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" already exists in schema \"%s\"",
						collname, GetDatabaseEncodingName(),
						get_namespace_name(nspOid))));

	/* mustn't match an any-encoding entry, either */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" already exists in schema \"%s\"",
						collname, get_namespace_name(nspOid))));
}

/*
 * ALTER COLLATION
 */
ObjectAddress
AlterCollation(AlterCollationStmt *stmt)
{
	Relation	rel;
	Oid			collOid;
	HeapTuple	tup;
	Form_pg_collation collForm;
	Datum		collversion;
	bool		isnull;
	char	   *oldversion;
	char	   *newversion;
	ObjectAddress address;

	rel = table_open(CollationRelationId, RowExclusiveLock);
	collOid = get_collation_oid(stmt->collname, false);

	if (!pg_collation_ownercheck(collOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_COLLATION,
					   NameListToString(stmt->collname));

	tup = SearchSysCacheCopy1(COLLOID, ObjectIdGetDatum(collOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for collation %u", collOid);

	collForm = (Form_pg_collation) GETSTRUCT(tup);
	collversion = SysCacheGetAttr(COLLOID, tup, Anum_pg_collation_collversion,
								  &isnull);
	oldversion = isnull ? NULL : TextDatumGetCString(collversion);

	newversion = get_collation_actual_version(collForm->collprovider, NameStr(collForm->collcollate));

	/* cannot change from NULL to non-NULL or vice versa */
	if ((!oldversion && newversion) || (oldversion && !newversion))
		elog(ERROR, "invalid collation version change");
	else if (oldversion && newversion && strcmp(newversion, oldversion) != 0)
	{
		bool		nulls[Natts_pg_collation];
		bool		replaces[Natts_pg_collation];
		Datum		values[Natts_pg_collation];

		ereport(NOTICE,
				(errmsg("changing version from %s to %s",
						oldversion, newversion)));

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replaces, false, sizeof(replaces));

		values[Anum_pg_collation_collversion - 1] = CStringGetTextDatum(newversion);
		replaces[Anum_pg_collation_collversion - 1] = true;

		tup = heap_modify_tuple(tup, RelationGetDescr(rel),
								values, nulls, replaces);
	}
	else
		ereport(NOTICE,
				(errmsg("version has not changed")));

	CatalogTupleUpdate(rel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(CollationRelationId, collOid, 0);

	ObjectAddressSet(address, CollationRelationId, collOid);

	heap_freetuple(tup);
	table_close(rel, NoLock);

	return address;
}


Datum
pg_collation_actual_version(PG_FUNCTION_ARGS)
{
	Oid			collid = PG_GETARG_OID(0);
	HeapTuple	tp;
	char	   *collcollate;
	char		collprovider;
	char	   *version;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("collation with OID %u does not exist", collid)));

	collcollate = pstrdup(NameStr(((Form_pg_collation) GETSTRUCT(tp))->collcollate));
	collprovider = ((Form_pg_collation) GETSTRUCT(tp))->collprovider;

	ReleaseSysCache(tp);

	version = get_collation_actual_version(collprovider, collcollate);

	if (version)
		PG_RETURN_TEXT_P(cstring_to_text(version));
	else
		PG_RETURN_NULL();
}


/* will we use "locale -a" in pg_import_system_collations? */
#if defined(HAVE_LOCALE_T) && !defined(WIN32)
#define READ_LOCALE_A_OUTPUT
#endif

#if defined(READ_LOCALE_A_OUTPUT) || defined(USE_ICU)
/*
 * Check a string to see if it is pure ASCII
 */
static bool
is_all_ascii(const char *str)
{
	while (*str)
	{
		if (IS_HIGHBIT_SET(*str))
			return false;
		str++;
	}
	return true;
}
#endif							/* READ_LOCALE_A_OUTPUT || USE_ICU */

#ifdef READ_LOCALE_A_OUTPUT
/*
 * "Normalize" a libc locale name, stripping off encoding tags such as
 * ".utf8" (e.g., "en_US.utf8" -> "en_US", but "br_FR.iso885915@euro"
 * -> "br_FR@euro").  Return true if a new, different name was
 * generated.
 */
static bool
normalize_libc_locale_name(char *new, const char *old)
{
	char	   *n = new;
	const char *o = old;
	bool		changed = false;

	while (*o)
	{
		if (*o == '.')
		{
			/* skip over encoding tag such as ".utf8" or ".UTF-8" */
			o++;
			while ((*o >= 'A' && *o <= 'Z')
				   || (*o >= 'a' && *o <= 'z')
				   || (*o >= '0' && *o <= '9')
				   || (*o == '-'))
				o++;
			changed = true;
		}
		else
			*n++ = *o++;
	}
	*n = '\0';

	return changed;
}

/*
 * qsort comparator for CollAliasData items
 */
static int
cmpaliases(const void *a, const void *b)
{
	const CollAliasData *ca = (const CollAliasData *) a;
	const CollAliasData *cb = (const CollAliasData *) b;

	/* comparing localename is enough because other fields are derived */
	return strcmp(ca->localename, cb->localename);
}
#endif							/* READ_LOCALE_A_OUTPUT */

/*
 * Dispatch collation create command to segments.
 *
 * @param alias:	collation name in collname field
 * @param locale:	locale name, usually include lang and region
 * @param nspid:	namespace pid
 * @param provider:	"icu" or "libc", NULL means decided by callee (DefineCollation in QE, default "libc")
 */
static void
DispatchCollationCreate(char *alias, char *locale, Oid nspid, char* provider, char* comment)
{
	Assert(Gp_role == GP_ROLE_DISPATCH);

	List *names = NIL;
	Value *schemaname = makeString(get_namespace_name(nspid));
	Value *relname = makeString(alias);

	names = lappend(names, schemaname);
	names = lappend(names, relname);

	List *parameters = lappend(NIL, makeDefElem("locale", (Node*) makeString(locale), -1));
	if (provider)
	{
		parameters = lappend(parameters, makeDefElem("provider", (Node*) makeString(provider), -1));
	}
	if (comment)
	{
		parameters = lappend(parameters, makeDefElem("comment", (Node*) makeString(comment), -1));
	}

	DefineStmt * stmt = makeNode(DefineStmt);
	stmt->kind = OBJECT_COLLATION;
	stmt->oldstyle = false;
	stmt->defnames = names;
	stmt->args = NIL;
	stmt->definition = parameters;
	stmt->trusted = false;
	CdbDispatchUtilityStatement((Node *) stmt,
	                            DF_CANCEL_ON_ERROR|
		                        DF_WITH_SNAPSHOT|
		                        DF_NEED_TWO_PHASE,
	                            GetAssignedOidsForDispatch(),
	                            NULL);
}

#ifdef USE_ICU
/*
 * Get the ICU language tag for a locale name.
 * The result is a palloc'd string.
 */
static char *
get_icu_language_tag(const char *localename)
{
	char		buf[ULOC_FULLNAME_CAPACITY];
	UErrorCode	status;

	status = U_ZERO_ERROR;
	uloc_toLanguageTag(localename, buf, sizeof(buf), true, &status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("could not convert locale name \"%s\" to language tag: %s",
						localename, u_errorName(status))));

	return pstrdup(buf);
}

/*
 * Get a comment (specifically, the display name) for an ICU locale.
 * The result is a palloc'd string, or NULL if we can't get a comment
 * or find that it's not all ASCII.  (We can *not* accept non-ASCII
 * comments, because the contents of template0 must be encoding-agnostic.)
 */
static char *
get_icu_locale_comment(const char *localename)
{
	UErrorCode	status;
	UChar		displayname[128];
	int32		len_uchar;
	int32		i;
	char	   *result;

	status = U_ZERO_ERROR;
	len_uchar = uloc_getDisplayName(localename, "en",
									displayname, lengthof(displayname),
									&status);
	if (U_FAILURE(status))
		return NULL;			/* no good reason to raise an error */

	/* Check for non-ASCII comment (can't use is_all_ascii for this) */
	for (i = 0; i < len_uchar; i++)
	{
		if (displayname[i] > 127)
			return NULL;
	}

	/* OK, transcribe */
	result = palloc(len_uchar + 1);
	for (i = 0; i < len_uchar; i++)
		result[i] = displayname[i];
	result[len_uchar] = '\0';

	return result;
}
#endif							/* USE_ICU */


/*
 * pg_import_system_collations: add known system collations to pg_collation
 */
Datum
pg_import_system_collations(PG_FUNCTION_ARGS)
{
	Oid			nspid = PG_GETARG_OID(0);
	int			ncreated = 0;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to import system collations"))));

	if (Gp_role != GP_ROLE_DISPATCH)
		ereport(ERROR,
					(errmsg("must be dispatcher to import system collations")));

	if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(nspid)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema with OID %u does not exist", nspid)));

	/* Load collations known to libc, using "locale -a" to enumerate them */
#ifdef READ_LOCALE_A_OUTPUT
	{
		FILE	   *locale_a_handle;
		char		localebuf[NAMEDATALEN]; /* we assume ASCII so this is fine */
		int			nvalid = 0;
		Oid			collid;
		CollAliasData *aliases;
		int			naliases,
					maxaliases,
					i;

		/* expansible array of aliases */
		maxaliases = 100;
		aliases = (CollAliasData *) palloc(maxaliases * sizeof(CollAliasData));
		naliases = 0;

		locale_a_handle = OpenPipeStream("locale -a", "r");
		if (locale_a_handle == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not execute command \"%s\": %m",
							"locale -a")));

		while (fgets(localebuf, sizeof(localebuf), locale_a_handle))
		{
			size_t		len;
			int			enc;
			char		alias[NAMEDATALEN];

			len = strlen(localebuf);

			if (len == 0 || localebuf[len - 1] != '\n')
			{
				elog(DEBUG1, "locale name too long, skipped: \"%s\"", localebuf);
				continue;
			}
			localebuf[len - 1] = '\0';

			/*
			 * Some systems have locale names that don't consist entirely of
			 * ASCII letters (such as "bokm&aring;l" or "fran&ccedil;ais").
			 * This is pretty silly, since we need the locale itself to
			 * interpret the non-ASCII characters. We can't do much with
			 * those, so we filter them out.
			 */
			if (!is_all_ascii(localebuf))
			{
				elog(DEBUG1, "locale name has non-ASCII characters, skipped: \"%s\"", localebuf);
				continue;
			}

			enc = pg_get_encoding_from_locale(localebuf, false);
			if (enc < 0)
			{
				/* error message printed by pg_get_encoding_from_locale() */
				continue;
			}
			if (!PG_VALID_BE_ENCODING(enc))
				continue;		/* ignore locales for client-only encodings */
			if (enc == PG_SQL_ASCII)
				continue;		/* C/POSIX are already in the catalog */
			/*
			 * Greenplum specific behavior: this function in Greenplum can only be called after a full cluster is
			 * built, this is different from Postgres which might call this function during initdb. When reaching
			 * here, it must be in a database session, we can just ignore the collations not match current database's
			 * encoding because they cannot be used in this database.
			 */
			if (enc != GetDatabaseEncoding())
				continue;       /* Ignore collations incompatible with database encoding */ 

			/* count valid locales found in operating system */
			nvalid++;

			/*
			 * Create a collation named the same as the locale, but quietly
			 * doing nothing if it already exists.  This is the behavior we
			 * need even at initdb time, because some versions of "locale -a"
			 * can report the same locale name more than once.  And it's
			 * convenient for later import runs, too, since you just about
			 * always want to add on new locales without a lot of chatter
			 * about existing ones.
			 */
			collid = CollationCreate(localebuf, nspid, GetUserId(),
									 COLLPROVIDER_LIBC, true, enc,
									 localebuf, localebuf,
									 get_collation_actual_version(COLLPROVIDER_LIBC, localebuf),
									 true, true);
			if (OidIsValid(collid))
			{
				DispatchCollationCreate(localebuf, localebuf, nspid, "libc", NULL);
				ncreated++;

				/* Must do CCI between inserts to handle duplicates correctly */
				CommandCounterIncrement();
			}

			/*
			 * Generate aliases such as "en_US" in addition to "en_US.utf8"
			 * for ease of use.  Note that collation names are unique per
			 * encoding only, so this doesn't clash with "en_US" for LATIN1,
			 * say.
			 *
			 * However, it might conflict with a name we'll see later in the
			 * "locale -a" output.  So save up the aliases and try to add them
			 * after we've read all the output.
			 */
			if (normalize_libc_locale_name(alias, localebuf))
			{
				if (naliases >= maxaliases)
				{
					maxaliases *= 2;
					aliases = (CollAliasData *)
						repalloc(aliases, maxaliases * sizeof(CollAliasData));
				}
				aliases[naliases].localename = pstrdup(localebuf);
				aliases[naliases].alias = pstrdup(alias);
				aliases[naliases].enc = enc;
				naliases++;
			}
		}

		ClosePipeStream(locale_a_handle);

		/*
		 * Before processing the aliases, sort them by locale name.  The point
		 * here is that if "locale -a" gives us multiple locale names with the
		 * same encoding and base name, say "en_US.utf8" and "en_US.utf-8", we
		 * want to pick a deterministic one of them.  First in ASCII sort
		 * order is a good enough rule.  (Before PG 10, the code corresponding
		 * to this logic in initdb.c had an additional ordering rule, to
		 * prefer the locale name exactly matching the alias, if any.  We
		 * don't need to consider that here, because we would have already
		 * created such a pg_collation entry above, and that one will win.)
		 */
		if (naliases > 1)
			qsort((void *) aliases, naliases, sizeof(CollAliasData), cmpaliases);

		/* Now add aliases, ignoring any that match pre-existing entries */
		for (i = 0; i < naliases; i++)
		{
			char	   *locale = aliases[i].localename;
			char	   *alias = aliases[i].alias;
			int			enc = aliases[i].enc;

			collid = CollationCreate(alias, nspid, GetUserId(),
									 COLLPROVIDER_LIBC, true, enc,
									 locale, locale,
									 get_collation_actual_version(COLLPROVIDER_LIBC, locale),
									 true, true);
			if (OidIsValid(collid))
			{
				DispatchCollationCreate(alias, locale, nspid, "libc", NULL);
				ncreated++;

				CommandCounterIncrement();
			}
		}

		/* Give a warning if "locale -a" seems to be malfunctioning */
		if (nvalid == 0)
			ereport(WARNING,
					(errmsg("no usable system locales were found")));
	}
#endif							/* READ_LOCALE_A_OUTPUT */

	/*
	 * Load collations known to ICU
	 *
	 * We use uloc_countAvailable()/uloc_getAvailable() rather than
	 * ucol_countAvailable()/ucol_getAvailable().  The former returns a full
	 * set of language+region combinations, whereas the latter only returns
	 * language+region combinations of they are distinct from the language's
	 * base collation.  So there might not be a de-DE or en-GB, which would be
	 * confusing.
	 */
#ifdef USE_ICU
	{
		int			i;

		/*
		 * Start the loop at -1 to sneak in the root locale without too much
		 * code duplication.
		 */
		for (i = -1; i < uloc_countAvailable(); i++)
		{
			const char *name;
			char	   *langtag;
			char	   *icucomment = NULL;
			const char *collcollate;
			char	   *collname;
			Oid			collid;

			if (i == -1)
				name = "";		/* ICU root locale */
			else
				name = uloc_getAvailable(i);

			langtag = get_icu_language_tag(name);
			collcollate = U_ICU_VERSION_MAJOR_NUM >= 54 ? langtag : name;

			/*
			 * Be paranoid about not allowing any non-ASCII strings into
			 * pg_collation
			 */
			if (!is_all_ascii(langtag) || !is_all_ascii(collcollate))
				continue;

			collname = psprintf("%s-x-icu", langtag);
			collid = CollationCreate(collname,
									 nspid, GetUserId(),
									 COLLPROVIDER_ICU, true, -1,
									 collcollate, collcollate,
									 get_collation_actual_version(COLLPROVIDER_ICU, collcollate),
									 true, true);
			if (OidIsValid(collid))
			{
				icucomment = get_icu_locale_comment(name);

				DispatchCollationCreate(collname, unconstify(char *, collcollate), nspid, "icu", icucomment);
				ncreated++;

				CommandCounterIncrement();

				if (icucomment)
					CreateComments(collid, CollationRelationId, 0,
								   icucomment);
			}
		}
	}
#endif							/* USE_ICU */

	PG_RETURN_INT32(ncreated);
}

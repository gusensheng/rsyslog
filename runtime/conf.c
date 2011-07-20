/* The config file handler (not yet a real object)
 *
 * This file is based on an excerpt from syslogd.c, which dates back
 * much later. I began the file on 2008-02-19 as part of the modularization
 * effort. Over time, a clean abstration will become even more important
 * because the config file handler will by dynamically be loaded and be
 * kept in memory only as long as the config file is actually being 
 * processed. Thereafter, it shall be unloaded. -- rgerhards
 *
 * TODO: the license MUST be changed to LGPL. However, we can not
 * currently do that, because we use some sysklogd code to crunch
 * the selector lines (e.g. *.info). That code is scheduled for removal
 * as part of RainerScript. After this is done, we can change licenses.
 *
 * Copyright 2008-2011 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#define CFGLNSIZ 4096 /* the maximum size of a configuraton file line, after re-combination */
#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <assert.h>
#include <dirent.h>
#include <glob.h>
#include <sys/types.h>
#ifdef HAVE_LIBGEN_H
#	ifndef OS_SOLARIS
#		include <libgen.h>
#	endif
#endif

#include "rsyslog.h"
#include "dirty.h"
#include "parse.h"
#include "action.h"
#include "template.h"
#include "cfsysline.h"
#include "modules.h"
#include "outchannel.h"
#include "stringbuf.h"
#include "conf.h"
#include "stringbuf.h"
#include "srUtils.h"
#include "errmsg.h"
#include "net.h"
#include "rule.h"
#include "ruleset.h"
#include "rsconf.h"
#include "unicode-helper.h"

#ifdef OS_SOLARIS
#	define NAME_MAX MAXNAMELEN
#endif

/* forward definitions */
//static rsRetVal cfline(rsconf_t *conf, uchar *line, rule_t **pfCurr);


/* static data */
DEFobjStaticHelpers
DEFobjCurrIf(module)
DEFobjCurrIf(errmsg)
DEFobjCurrIf(net)
DEFobjCurrIf(rule)
DEFobjCurrIf(ruleset)

ecslConfObjType currConfObj = eConfObjGlobal; /* to support scoping - which config object is currently active? */
int bConfStrictScoping = 0;	/* force strict scoping during config processing? */


/* The following module-global variables are used for building
 * tag and host selector lines during startup and config reload.
 * This is stored as a global variable pool because of its ease. It is
 * also fairly compatible with multi-threading as the stratup code must
 * be run in a single thread anyways. So there can be no race conditions.
 * rgerhards 2005-10-18
 */
EHostnameCmpMode eDfltHostnameCmpMode = HN_NO_COMP;
cstr_t *pDfltHostnameCmp = NULL;
cstr_t *pDfltProgNameCmp = NULL;


/* process a $ModLoad config line.  */
rsRetVal
doModLoad(uchar **pp, __attribute__((unused)) void* pVal)
{
	DEFiRet;
	uchar szName[512];
	uchar *pModName;

	ASSERT(pp != NULL);
	ASSERT(*pp != NULL);

	skipWhiteSpace(pp); /* skip over any whitespace */
	if(getSubString(pp, (char*) szName, sizeof(szName) / sizeof(uchar), ' ')  != 0) {
		errmsg.LogError(0, RS_RET_NOT_FOUND, "could not extract module name");
		ABORT_FINALIZE(RS_RET_NOT_FOUND);
	}
	skipWhiteSpace(pp); /* skip over any whitespace */

	/* this below is a quick and dirty hack to provide compatibility with the
	 * $ModLoad MySQL forward compatibility statement. TODO: clean this up
	 * For the time being, it is clean enough, it just needs to be done
	 * differently when we have a full design for loadable plug-ins. For the
	 * time being, we just mangle the names a bit.
	 * rgerhards, 2007-08-14
	 */
	if(!strcmp((char*) szName, "MySQL"))
		pModName = (uchar*) "ommysql.so";
	else
		pModName = szName;

	CHKiRet(module.Load(pModName, 1));

finalize_it:
	RETiRet;
}


/* parse and interpret a $-config line that starts with
 * a name (this is common code). It is parsed to the name
 * and then the proper sub-function is called to handle
 * the actual directive.
 * rgerhards 2004-11-17
 * rgerhards 2005-06-21: previously only for templates, now 
 *    generalized.
 */
rsRetVal
doNameLine(uchar **pp, void* pVal)
{
	DEFiRet;
	uchar *p;
	enum eDirective eDir;
	char szName[128];

	ASSERT(pp != NULL);
	p = *pp;
	ASSERT(p != NULL);

	eDir = (enum eDirective) pVal;	/* this time, it actually is NOT a pointer! */

	if(getSubString(&p, szName, sizeof(szName) / sizeof(char), ',')  != 0) {
		errmsg.LogError(0, RS_RET_NOT_FOUND, "Invalid config line: could not extract name - line ignored");
		ABORT_FINALIZE(RS_RET_NOT_FOUND);
	}
	if(*p == ',')
		++p; /* comma was eaten */
	
	/* we got the name - now we pass name & the rest of the string
	 * to the subfunction. It makes no sense to do further
	 * parsing here, as this is in close interaction with the
	 * respective subsystem. rgerhards 2004-11-17
	 */
	
	switch(eDir) {
		case DIR_TEMPLATE: 
			tplAddLine(loadConf, szName, &p);
			break;
		case DIR_OUTCHANNEL: 
			ochAddLine(szName, &p);
			break;
		case DIR_ALLOWEDSENDER: 
			net.addAllowedSenderLine(szName, &p);
			break;
		default:/* we do this to avoid compiler warning - not all
			 * enum values call this function, so an incomplete list
			 * is quite ok (but then we should not run into this code,
			 * so at least we log a debug warning).
			 */
			dbgprintf("INTERNAL ERROR: doNameLine() called with invalid eDir %d.\n",
				eDir);
			break;
	}

	*pp = p;

finalize_it:
	RETiRet;
}


/* Parse and interpret a system-directive in the config line
 * A system directive is one that starts with a "$" sign. It offers
 * extended configuration parameters.
 * 2004-11-17 rgerhards
 */
rsRetVal
cfsysline(uchar *p)
{
	DEFiRet;
	uchar szCmd[64];

	ASSERT(p != NULL);
	errno = 0;
	if(getSubString(&p, (char*) szCmd, sizeof(szCmd) / sizeof(uchar), ' ')  != 0) {
		errmsg.LogError(0, RS_RET_NOT_FOUND, "Invalid $-configline - could not extract command - line ignored\n");
		ABORT_FINALIZE(RS_RET_NOT_FOUND);
	}

	/* we now try and see if we can find the command in the registered
	 * list of cfsysline handlers. -- rgerhards, 2007-07-31
	 */
	CHKiRet(processCfSysLineCommand(szCmd, &p));

	/* now check if we have some extra characters left on the line - that
	 * should not be the case. Whitespace is OK, but everything else should
	 * trigger a warning (that may be an indication of undesired behaviour).
	 * An exception, of course, are comments (starting with '#').
	 * rgerhards, 2007-07-04
	 */
	skipWhiteSpace(&p);

	if(*p && *p != '#') { /* we have a non-whitespace, so let's complain */
		errmsg.LogError(0, NO_ERRCODE, 
		         "error: extra characters in config line ignored: '%s'", p);
	}

finalize_it:
	RETiRet;
}


/* Helper to cfline() and its helpers. Parses a template name
 * from an "action" line. Must be called with the Line pointer
 * pointing to the first character after the semicolon.
 * rgerhards 2004-11-19
 * changed function to work with OMSR. -- rgerhards, 2007-07-27
 * the default template is to be used when no template is specified.
 */
rsRetVal cflineParseTemplateName(uchar** pp, omodStringRequest_t *pOMSR, int iEntry, int iTplOpts, uchar *dfltTplName)
{
	uchar *p;
	uchar *tplName = NULL;
	cstr_t *pStrB;
	DEFiRet;

	ASSERT(pp != NULL);
	ASSERT(*pp != NULL);
	ASSERT(pOMSR != NULL);

	p =*pp;
	/* a template must follow - search it and complain, if not found */
	skipWhiteSpace(&p);
	if(*p == ';')
		++p; /* eat it */
	else if(*p != '\0' && *p != '#') {
		errmsg.LogError(0, RS_RET_ERR, "invalid character in selector line - ';template' expected");
		ABORT_FINALIZE(RS_RET_ERR);
	}

	skipWhiteSpace(&p); /* go to begin of template name */

	if(*p == '\0' || *p == '#') {
		/* no template specified, use the default */
		/* TODO: check NULL ptr */
		tplName = (uchar*) strdup((char*)dfltTplName);
	} else {
		/* template specified, pick it up */
		CHKiRet(cstrConstruct(&pStrB));

		/* now copy the string */
		while(*p && *p != '#' && !isspace((int) *p)) {
			CHKiRet(cstrAppendChar(pStrB, *p));
			++p;
		}
		CHKiRet(cstrFinalize(pStrB));
		CHKiRet(cstrConvSzStrAndDestruct(pStrB, &tplName, 0));
	}

	CHKiRet(OMSRsetEntry(pOMSR, iEntry, tplName, iTplOpts));

finalize_it:
	if(iRet != RS_RET_OK)
		free(tplName);

	*pp = p;

	RETiRet;
}

/* Helper to cfline(). Parses a file name up until the first
 * comma and then looks for the template specifier. Tries
 * to find that template.
 * rgerhards 2004-11-18
 * parameter pFileName must point to a buffer large enough
 * to hold the largest possible filename.
 * rgerhards, 2007-07-25
 * updated to include OMSR pointer -- rgerhards, 2007-07-27
 * updated to include template name -- rgerhards, 2008-03-28
 * rgerhards, 2010-01-19: file names end at the first space
 */
rsRetVal
cflineParseFileName(uchar* p, uchar *pFileName, omodStringRequest_t *pOMSR, int iEntry, int iTplOpts, uchar *pszTpl)
{
	register uchar *pName;
	int i;
	DEFiRet;

	ASSERT(pOMSR != NULL);

	pName = pFileName;
	i = 1; /* we start at 1 so that we reseve space for the '\0'! */
	while(*p && *p != ';' && *p != ' ' && i < MAXFNAME) {
		*pName++ = *p++;
		++i;
	}
	*pName = '\0';

	iRet = cflineParseTemplateName(&p, pOMSR, iEntry, iTplOpts, pszTpl);

	RETiRet;
}


/* Helper to cfline(). This function takes the filter part of a traditional, PRI
 * based line and decodes the PRIs given in the selector line. It processed the
 * line up to the beginning of the action part. A pointer to that beginnig is
 * passed back to the caller.
 * rgerhards 2005-09-15
 */
/* GPLv3 - stems back to sysklogd */
rsRetVal cflineProcessTradPRIFilter(uchar **pline, register rule_t *pRule)
{
	uchar *p;
	register uchar *q;
	register int i, i2;
	uchar *bp;
	int pri;
	int singlpri = 0;
	int ignorepri = 0;
	uchar buf[2048]; /* buffer for facility and priority names */
	uchar xbuf[200];
	DEFiRet;

	ASSERT(pline != NULL);
	ASSERT(*pline != NULL);
	ISOBJ_TYPE_assert(pRule, rule);

	dbgprintf(" - traditional PRI filter '%s'\n", *pline);
	errno = 0;	/* keep strerror_r() stuff out of logerror messages */

	pRule->f_filter_type = FILTER_PRI;
	/* Note: file structure is pre-initialized to zero because it was
	 * created with calloc()!
	 */
	for (i = 0; i <= LOG_NFACILITIES; i++) {
		pRule->f_filterData.f_pmask[i] = TABLE_NOPRI;
	}

	/* scan through the list of selectors */
	for (p = *pline; *p && *p != '\t' && *p != ' ';) {
		/* find the end of this facility name list */
		for (q = p; *q && *q != '\t' && *q++ != '.'; )
			continue;

		/* collect priority name */
		for (bp = buf; *q && !strchr("\t ,;", *q) && bp < buf+sizeof(buf)-1 ; )
			*bp++ = *q++;
		*bp = '\0';

		/* skip cruft */
		if(*q) {
			while (strchr(",;", *q))
				q++;
		}

		/* decode priority name */
		if ( *buf == '!' ) {
			ignorepri = 1;
			/* copy below is ok, we can NOT go off the allocated area */
			for (bp=buf; *(bp+1); bp++)
				*bp=*(bp+1);
			*bp='\0';
		} else {
			ignorepri = 0;
		}
		if ( *buf == '=' ) {
			singlpri = 1;
			pri = decodeSyslogName(&buf[1], syslogPriNames);
		}
		else { singlpri = 0;
			pri = decodeSyslogName(buf, syslogPriNames);
		}

		if (pri < 0) {
			snprintf((char*) xbuf, sizeof(xbuf), "unknown priority name \"%s\"", buf);
			errmsg.LogError(0, RS_RET_ERR, "%s", xbuf);
			return RS_RET_ERR;
		}

		/* scan facilities */
		while (*p && !strchr("\t .;", *p)) {
			for (bp = buf; *p && !strchr("\t ,;.", *p) && bp < buf+sizeof(buf)-1 ; )
				*bp++ = *p++;
			*bp = '\0';
			if (*buf == '*') {
				for (i = 0; i <= LOG_NFACILITIES; i++) {
					if ( pri == INTERNAL_NOPRI ) {
						if ( ignorepri )
							pRule->f_filterData.f_pmask[i] = TABLE_ALLPRI;
						else
							pRule->f_filterData.f_pmask[i] = TABLE_NOPRI;
					}
					else if ( singlpri ) {
						if ( ignorepri )
				  			pRule->f_filterData.f_pmask[i] &= ~(1<<pri);
						else
				  			pRule->f_filterData.f_pmask[i] |= (1<<pri);
					} else {
						if ( pri == TABLE_ALLPRI ) {
							if ( ignorepri )
								pRule->f_filterData.f_pmask[i] = TABLE_NOPRI;
							else
								pRule->f_filterData.f_pmask[i] = TABLE_ALLPRI;
						} else {
							if ( ignorepri )
								for (i2= 0; i2 <= pri; ++i2)
									pRule->f_filterData.f_pmask[i] &= ~(1<<i2);
							else
								for (i2= 0; i2 <= pri; ++i2)
									pRule->f_filterData.f_pmask[i] |= (1<<i2);
						}
					}
				}
			} else {
				i = decodeSyslogName(buf, syslogFacNames);
				if (i < 0) {

					snprintf((char*) xbuf, sizeof(xbuf), "unknown facility name \"%s\"", buf);
					errmsg.LogError(0, RS_RET_ERR, "%s", xbuf);
					return RS_RET_ERR;
				}

				if ( pri == INTERNAL_NOPRI ) {
					if ( ignorepri )
						pRule->f_filterData.f_pmask[i >> 3] = TABLE_ALLPRI;
					else
						pRule->f_filterData.f_pmask[i >> 3] = TABLE_NOPRI;
				} else if ( singlpri ) {
					if ( ignorepri )
						pRule->f_filterData.f_pmask[i >> 3] &= ~(1<<pri);
					else
						pRule->f_filterData.f_pmask[i >> 3] |= (1<<pri);
				} else {
					if ( pri == TABLE_ALLPRI ) {
						if ( ignorepri )
							pRule->f_filterData.f_pmask[i >> 3] = TABLE_NOPRI;
						else
							pRule->f_filterData.f_pmask[i >> 3] = TABLE_ALLPRI;
					} else {
						if ( ignorepri )
							for (i2= 0; i2 <= pri; ++i2)
								pRule->f_filterData.f_pmask[i >> 3] &= ~(1<<i2);
						else
							for (i2= 0; i2 <= pri; ++i2)
								pRule->f_filterData.f_pmask[i >> 3] |= (1<<i2);
					}
				}
			}
			while (*p == ',' || *p == ' ')
				p++;
		}

		p = q;
	}

	/* skip to action part */
	while (*p == '\t' || *p == ' ')
		p++;

	*pline = p;
	RETiRet;
}


/* Helper to cfline(). This function takes the filter part of a property
 * based filter and decodes it. It processes the line up to the beginning
 * of the action part. A pointer to that beginnig is passed back to the caller.
 * rgerhards 2005-09-15
 */
rsRetVal cflineProcessPropFilter(uchar **pline, register rule_t *f)
{
	rsParsObj *pPars;
	cstr_t *pCSCompOp;
	cstr_t *pCSPropName;
	rsRetVal iRet;
	int iOffset; /* for compare operations */

	ASSERT(pline != NULL);
	ASSERT(*pline != NULL);
	ASSERT(f != NULL);

	dbgprintf(" - property-based filter '%s'\n", *pline);
	errno = 0;	/* keep strerror_r() stuff out of logerror messages */

	f->f_filter_type = FILTER_PROP;

	/* create parser object starting with line string without leading colon */
	if((iRet = rsParsConstructFromSz(&pPars, (*pline)+1)) != RS_RET_OK) {
		errmsg.LogError(0, iRet, "Error %d constructing parser object - ignoring selector", iRet);
		return(iRet);
	}

	/* read property */
	iRet = parsDelimCStr(pPars, &pCSPropName, ',', 1, 1, 1);
	if(iRet != RS_RET_OK) {
		errmsg.LogError(0, iRet, "error %d parsing filter property - ignoring selector", iRet);
		rsParsDestruct(pPars);
		return(iRet);
	}
	iRet = propNameToID(pCSPropName, &f->f_filterData.prop.propID);
	if(iRet != RS_RET_OK) {
		errmsg.LogError(0, iRet, "error %d parsing filter property - ignoring selector", iRet);
		rsParsDestruct(pPars);
		return(iRet);
	}
	if(f->f_filterData.prop.propID == PROP_CEE) {
		/* in CEE case, we need to preserve the actual property name */
		if((f->f_filterData.prop.propName =
		     es_newStrFromBuf((char*)cstrGetSzStrNoNULL(pCSPropName)+2, cstrLen(pCSPropName)-2)) == NULL) {
			cstrDestruct(&pCSPropName);
			return(RS_RET_ERR);
		}
	}
	cstrDestruct(&pCSPropName);

	/* read operation */
	iRet = parsDelimCStr(pPars, &pCSCompOp, ',', 1, 1, 1);
	if(iRet != RS_RET_OK) {
		errmsg.LogError(0, iRet, "error %d compare operation property - ignoring selector", iRet);
		rsParsDestruct(pPars);
		return(iRet);
	}

	/* we now first check if the condition is to be negated. To do so, we first
	 * must make sure we have at least one char in the param and then check the
	 * first one.
	 * rgerhards, 2005-09-26
	 */
	if(rsCStrLen(pCSCompOp) > 0) {
		if(*rsCStrGetBufBeg(pCSCompOp) == '!') {
			f->f_filterData.prop.isNegated = 1;
			iOffset = 1; /* ignore '!' */
		} else {
			f->f_filterData.prop.isNegated = 0;
			iOffset = 0;
		}
	} else {
		f->f_filterData.prop.isNegated = 0;
		iOffset = 0;
	}

	if(!rsCStrOffsetSzStrCmp(pCSCompOp, iOffset, (uchar*) "contains", 8)) {
		f->f_filterData.prop.operation = FIOP_CONTAINS;
	} else if(!rsCStrOffsetSzStrCmp(pCSCompOp, iOffset, (uchar*) "isequal", 7)) {
		f->f_filterData.prop.operation = FIOP_ISEQUAL;
	} else if(!rsCStrOffsetSzStrCmp(pCSCompOp, iOffset, (uchar*) "isempty", 7)) {
		f->f_filterData.prop.operation = FIOP_ISEMPTY;
	} else if(!rsCStrOffsetSzStrCmp(pCSCompOp, iOffset, (uchar*) "startswith", 10)) {
		f->f_filterData.prop.operation = FIOP_STARTSWITH;
	} else if(!rsCStrOffsetSzStrCmp(pCSCompOp, iOffset, (unsigned char*) "regex", 5)) {
		f->f_filterData.prop.operation = FIOP_REGEX;
	} else if(!rsCStrOffsetSzStrCmp(pCSCompOp, iOffset, (unsigned char*) "ereregex", 8)) {
		f->f_filterData.prop.operation = FIOP_EREREGEX;
	} else {
		errmsg.LogError(0, NO_ERRCODE, "error: invalid compare operation '%s' - ignoring selector",
		           (char*) rsCStrGetSzStrNoNULL(pCSCompOp));
	}
	rsCStrDestruct(&pCSCompOp); /* no longer needed */

	if(f->f_filterData.prop.operation != FIOP_ISEMPTY) {
		/* read compare value */
		iRet = parsQuotedCStr(pPars, &f->f_filterData.prop.pCSCompValue);
		if(iRet != RS_RET_OK) {
			errmsg.LogError(0, iRet, "error %d compare value property - ignoring selector", iRet);
			rsParsDestruct(pPars);
			return(iRet);
		}
	}

	/* skip to action part */
	if((iRet = parsSkipWhitespace(pPars)) != RS_RET_OK) {
		errmsg.LogError(0, iRet, "error %d skipping to action part - ignoring selector", iRet);
		rsParsDestruct(pPars);
		return(iRet);
	}

	/* cleanup */
	*pline = *pline + rsParsGetParsePointer(pPars) + 1;
		/* we are adding one for the skipped initial ":" */

	return rsParsDestruct(pPars);
}


/*
 * Helper to cfline(). This function interprets a BSD host selector line
 * from the config file ("+/-hostname"). It stores it for further reference.
 * rgerhards 2005-10-19
 */
rsRetVal cflineProcessHostSelector(uchar **pline)
{
	DEFiRet;

	ASSERT(pline != NULL);
	ASSERT(*pline != NULL);
	ASSERT(**pline == '-' || **pline == '+');

	dbgprintf(" - host selector line\n");

	/* check include/exclude setting */
	if(**pline == '+') {
		eDfltHostnameCmpMode = HN_COMP_MATCH;
	} else { /* we do not check for '-', it must be, else we wouldn't be here */
		eDfltHostnameCmpMode = HN_COMP_NOMATCH;
	}
	(*pline)++;	/* eat + or - */

	/* the below is somewhat of a quick hack, but it is efficient (this is
	 * why it is in here. "+*" resets the tag selector with BSD syslog. We mimic
	 * this, too. As it is easy to check that condition, we do not fire up a
	 * parser process, just make sure we do not address beyond our space.
	 * Order of conditions in the if-statement is vital! rgerhards 2005-10-18
	 */
	if(**pline != '\0' && **pline == '*' && *(*pline+1) == '\0') {
		dbgprintf("resetting BSD-like hostname filter\n");
		eDfltHostnameCmpMode = HN_NO_COMP;
		if(pDfltHostnameCmp != NULL) {
			CHKiRet(rsCStrSetSzStr(pDfltHostnameCmp, NULL));
		}
	} else {
		dbgprintf("setting BSD-like hostname filter to '%s'\n", *pline);
		if(pDfltHostnameCmp == NULL) {
			/* create string for parser */
			CHKiRet(rsCStrConstructFromszStr(&pDfltHostnameCmp, *pline));
		} else { /* string objects exists, just update... */
			CHKiRet(rsCStrSetSzStr(pDfltHostnameCmp, *pline));
		}
	}

finalize_it:
	RETiRet;
}


/*
 * Helper to cfline(). This function interprets a BSD tag selector line
 * from the config file ("!tagname"). It stores it for further reference.
 * rgerhards 2005-10-18
 */
rsRetVal cflineProcessTagSelector(uchar **pline)
{
	DEFiRet;

	ASSERT(pline != NULL);
	ASSERT(*pline != NULL);
	ASSERT(**pline == '!');

	dbgprintf(" - programname selector line\n");

	(*pline)++;	/* eat '!' */

	/* the below is somewhat of a quick hack, but it is efficient (this is
	 * why it is in here. "!*" resets the tag selector with BSD syslog. We mimic
	 * this, too. As it is easy to check that condition, we do not fire up a
	 * parser process, just make sure we do not address beyond our space.
	 * Order of conditions in the if-statement is vital! rgerhards 2005-10-18
	 */
	if(**pline != '\0' && **pline == '*' && *(*pline+1) == '\0') {
		dbgprintf("resetting programname filter\n");
		if(pDfltProgNameCmp != NULL) {
			rsCStrDestruct(&pDfltProgNameCmp);
		}
	} else {
		dbgprintf("setting programname filter to '%s'\n", *pline);
		if(pDfltProgNameCmp == NULL) {
			/* create string for parser */
			CHKiRet(rsCStrConstructFromszStr(&pDfltProgNameCmp, *pline));
		} else { /* string objects exists, just update... */
			CHKiRet(rsCStrSetSzStr(pDfltProgNameCmp, *pline));
		}
	}

finalize_it:
	RETiRet;
}


#if 0
/* read the filter part of a configuration line and store the filter
 * in the supplied rule_t
 * rgerhards, 2007-08-01
 */
static rsRetVal cflineDoFilter(uchar **pp, rule_t *f)
{
	DEFiRet;

	ASSERT(pp != NULL);
	ISOBJ_TYPE_assert(f, rule);

	/* check which filter we need to pull... */
	switch(**pp) {
		case ':':
			CHKiRet(cflineProcessPropFilter(pp, f));
			break;
		default:
			CHKiRet(cflineProcessTradPRIFilter(pp, f));
			break;
	}

	/* we now check if there are some global (BSD-style) filter conditions
	 * and, if so, we copy them over. rgerhards, 2005-10-18
	 */
	if(pDfltProgNameCmp != NULL) {
		CHKiRet(rsCStrConstructFromCStr(&(f->pCSProgNameComp), pDfltProgNameCmp));
	}

	if(eDfltHostnameCmpMode != HN_NO_COMP) {
		f->eHostnameCmpMode = eDfltHostnameCmpMode;
		CHKiRet(rsCStrConstructFromCStr(&(f->pCSHostnameComp), pDfltHostnameCmp));
	}

finalize_it:
	RETiRet;
}
#endif


/* process the action part of a selector line
 * rgerhards, 2007-08-01
 */
rsRetVal cflineDoAction(rsconf_t *conf, uchar **p, action_t **ppAction)
{
	modInfo_t *pMod;
	cfgmodules_etry_t *node;
	omodStringRequest_t *pOMSR;
	action_t *pAction = NULL;
	void *pModData;
	DEFiRet;

	ASSERT(p != NULL);
	ASSERT(ppAction != NULL);

	/* loop through all modules and see if one picks up the line */
	node = module.GetNxtCnfType(conf, NULL, eMOD_OUT);
	/* Note: clang static analyzer reports that node maybe == NULL. However, this is
	 * not possible, because we have the built-in output modules which are always
	 * present. Anyhow, we guard this by an assert. -- rgerhards, 2010-12-16
	 */
	assert(node != NULL);
	while(node != NULL) {
		pOMSR = NULL;
		pMod = node->pMod;
		iRet = pMod->mod.om.parseSelectorAct(p, &pModData, &pOMSR);
		dbgprintf("tried selector action for %s: %d\n", module.GetName(pMod), iRet);
		if(iRet == RS_RET_OK || iRet == RS_RET_SUSPENDED) {
			/* advance our config parser state: we now only accept an $End as valid,
			 * no more action statments.
			 */
			if(currConfObj == eConfObjAction)
				currConfObj = eConfObjActionWaitEnd;
			if((iRet = addAction(&pAction, pMod, pModData, pOMSR, (iRet == RS_RET_SUSPENDED)? 1 : 0)) == RS_RET_OK) {
				/* now check if the module is compatible with select features */
				if(pMod->isCompatibleWithFeature(sFEATURERepeatedMsgReduction) == RS_RET_OK)
					pAction->f_ReduceRepeated = loadConf->globals.bReduceRepeatMsgs;
				else {
					dbgprintf("module is incompatible with RepeatedMsgReduction - turned off\n");
					pAction->f_ReduceRepeated = 0;
				}
				pAction->eState = ACT_STATE_RDY; /* action is enabled */
				conf->actions.nbrActions++;	/* one more active action! */
			}
			break;
		}
		else if(iRet != RS_RET_CONFLINE_UNPROCESSED) {
			/* In this case, the module would have handled the config
			 * line, but some error occured while doing so. This error should
			 * already by reported by the module. We do not try any other
			 * modules on this line, because we found the right one.
			 * rgerhards, 2007-07-24
			 */
			dbgprintf("error %d parsing config line\n", (int) iRet);
			break;
		}
		node = module.GetNxtCnfType(conf, node, eMOD_OUT);
	}

	*ppAction = pAction;
	RETiRet;
}


/* return the current number of active actions
 * rgerhards, 2008-07-28
 */
static rsRetVal
GetNbrActActions(rsconf_t *conf, int *piNbrActions)
{
	DEFiRet;
	assert(piNbrActions != NULL);
	*piNbrActions = conf->actions.nbrActions;
	RETiRet;
}


/* queryInterface function
 * rgerhards, 2008-02-29
 */
BEGINobjQueryInterface(conf)
CODESTARTobjQueryInterface(conf)
	if(pIf->ifVersion != confCURR_IF_VERSION) { /* check for current version, increment on each change */
		ABORT_FINALIZE(RS_RET_INTERFACE_NOT_SUPPORTED);
	}

	/* ok, we have the right interface, so let's fill it
	 * Please note that we may also do some backwards-compatibility
	 * work here (if we can support an older interface version - that,
	 * of course, also affects the "if" above).
	 */
	pIf->doNameLine = doNameLine;
	pIf->cfsysline = cfsysline;
	pIf->doModLoad = doModLoad;
	pIf->GetNbrActActions = GetNbrActActions;

finalize_it:
ENDobjQueryInterface(conf)


/* switch to a new action scope. This means that we switch the current 
 * mode to action, but it also means we need to clear all scope variables,
 * so that we have a new environment.
 * rgerhards, 2010-07-23
 */
static inline rsRetVal
setActionScope(void)
{
	DEFiRet;
	cfgmodules_etry_t *node;

	currConfObj = eConfObjAction;
	DBGPRINTF("entering action scope\n");
	CHKiRet(actionNewScope());

	/* now tell each action to start the scope */
	node = NULL;
	while((node = module.GetNxtCnfType(loadConf, node, eMOD_OUT)) != NULL) {
		DBGPRINTF("beginning scope on module %s\n", node->pMod->pszName);
		node->pMod->mod.om.newScope();
	}

finalize_it:
	RETiRet;
}


/* switch back from action scope.
 * rgerhards, 2010-07-27
 */
static inline rsRetVal
unsetActionScope(void)
{
	DEFiRet;
	cfgmodules_etry_t *node;

	currConfObj = eConfObjAction;
	DBGPRINTF("exiting action scope\n");
	CHKiRet(actionRestoreScope());

	/* now tell each action to restore the scope */
	node = NULL;
	while((node = module.GetNxtCnfType(loadConf, node, eMOD_OUT)) != NULL) {
		DBGPRINTF("exiting scope on module %s\n", node->pMod->pszName);
		node->pMod->mod.om.restoreScope();
	}

finalize_it:
	RETiRet;
}


/* This method is called by our own handlers to begin a new config
 * object ($Begin statement). This also implies a new scope.
 * rgerhards, 2010-07-23
 */
static rsRetVal
beginConfObj(void __attribute__((unused)) *pVal, uchar *pszName)
{
	DEFiRet;

	if(currConfObj != eConfObjGlobal) {
		errmsg.LogError(0, RS_RET_CONF_NOT_GLBL, "not in global scope - can not nest $Begin");
		ABORT_FINALIZE(RS_RET_CONF_NOT_GLBL);
	}

	if(!strcasecmp((char*)pszName, "action")) {
		setActionScope();
	} else {
		errmsg.LogError(0, RS_RET_INVLD_CONF_OBJ, "invalid config object \"%s\" in $Begin", pszName);
		ABORT_FINALIZE(RS_RET_INVLD_CONF_OBJ);
	}

finalize_it:
	free(pszName); /* no longer needed */
	RETiRet;
}


/* This method is called to end a config scope and switch
 * back to global scope.
 * rgerhards, 2010-07-23
 */
static rsRetVal
endConfObj(void __attribute__((unused)) *pVal, uchar *pszName)
{
	DEFiRet;

	if(currConfObj == eConfObjGlobal) {
		errmsg.LogError(0, RS_RET_CONF_NOT_GLBL, "already in global scope - dangling $End");
		ABORT_FINALIZE(RS_RET_CONF_IN_GLBL);
	}

	if(!strcasecmp((char*)pszName, "action")) {
		if(currConfObj == eConfObjAction) {
			errmsg.LogError(0, RS_RET_CONF_END_NO_ACT, "$End action but not action specified");
			/* this is a warning, we continue processing in that case (unscope) */
		} else if(currConfObj != eConfObjActionWaitEnd) {
			errmsg.LogError(0, RS_RET_CONF_INVLD_END, "$End not for active config object - "
							          "nesting error?");
			ABORT_FINALIZE(RS_RET_CONF_INVLD_END);
		}
		currConfObj = eConfObjGlobal;
		CHKiRet(unsetActionScope());
	} else {
		errmsg.LogError(0, RS_RET_INVLD_CONF_OBJ, "invalid config object \"%s\" in $End", pszName);
		ABORT_FINALIZE(RS_RET_INVLD_CONF_OBJ);
	}

finalize_it:
	free(pszName); /* no longer needed */
	RETiRet;
}


/* Reset config variables to default values. Note that
 * when we are inside an scope, we simply reset this to global.
 * However, $ResetConfigVariables is a global directive, and as such
 * will not be honored inside a scope!
 * rgerhards, 2010-07-23
 */
static rsRetVal
resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	currConfObj = eConfObjGlobal;
	bConfStrictScoping = 0;
	return RS_RET_OK;
}


/* exit our class
 * rgerhards, 2008-03-11
 */
BEGINObjClassExit(conf, OBJ_IS_CORE_MODULE) /* CHANGE class also in END MACRO! */
CODESTARTObjClassExit(conf)
	/* free no-longer needed module-global variables */
	if(pDfltHostnameCmp != NULL) {
		rsCStrDestruct(&pDfltHostnameCmp);
	}

	if(pDfltProgNameCmp != NULL) {
		rsCStrDestruct(&pDfltProgNameCmp);
	}

	/* release objects we no longer need */
	objRelease(module, CORE_COMPONENT);
	objRelease(errmsg, CORE_COMPONENT);
	objRelease(net, LM_NET_FILENAME);
	objRelease(rule, CORE_COMPONENT);
	objRelease(ruleset, CORE_COMPONENT);
ENDObjClassExit(conf)


/* Initialize our class. Must be called as the very first method
 * before anything else is called inside this class.
 * rgerhards, 2008-02-29
 */
BEGINAbstractObjClassInit(conf, 1, OBJ_IS_CORE_MODULE) /* class, version - CHANGE class also in END MACRO! */
	/* request objects we use */
	CHKiRet(objUse(module, CORE_COMPONENT));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(net, LM_NET_FILENAME)); /* TODO: make this dependcy go away! */
	CHKiRet(objUse(rule, CORE_COMPONENT));
	CHKiRet(objUse(ruleset, CORE_COMPONENT));

	CHKiRet(regCfSysLineHdlr((uchar *)"begin", 0, eCmdHdlrGetWord, beginConfObj, NULL, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"end", 0, eCmdHdlrGetWord, endConfObj, NULL, NULL, eConfObjAlways));
	CHKiRet(regCfSysLineHdlr((uchar *)"strictscoping", 0, eCmdHdlrBinary, NULL, &bConfStrictScoping, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler, resetConfigVariables, NULL, NULL, eConfObjAction));
ENDObjClassInit(conf)

/* vi:set ai:
 */

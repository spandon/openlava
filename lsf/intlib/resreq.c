/*
 * Copyright (C) 2015 David Bigagli
 * Copyright (C) 2007 Platform Computing Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

#include "intlibout.h"

struct sections {
    char   *select;
    char   *order;
    char   *rusage;
    char   *filter;
    char   *span;
};

static int parseSection(char *, struct sections *);
static int parseSelect(char * ,
                       struct resVal *,
                       struct lsInfo *,
                       bool_t,
                       int);
static int parseOrder(char * ,
                      struct resVal *,
                      struct lsInfo *);
static int parseFilter(char * ,
                       struct resVal *,
                       struct lsInfo *);
static int parseUsage(char * ,
                      struct resVal *,
                      struct lsInfo *);
static int parseSpan (char *,
                      struct resVal *);
static int resToClass(char * ,
                      struct resVal *,
                      struct lsInfo *);
static int setDefaults(struct resVal *,
                       struct lsInfo *,
                       int);
static int getVal(char **, float *);
static int getKeyEntry (char *);
static int getTimeVal(char **, float *);
void freeResVal (struct resVal *);
void initResVal (struct resVal *);
static link_t *get_rusage_entries(const char *);

extern char isanumber_(char *);

hTab resNameTbl = {NULL, 0, 0};
hTab keyNameTbl = {NULL, 0, 0};
#define KEY_DURATION    1
#define KEY_HOSTS       2
#define KEY_PTILE       3
#define KEY_DECAY       4
#define NUM_KEYS        5

#define ALLOC_STRING(buffer, buffer_len, req_len) {     \
        if (buffer == NULL || buffer_len < req_len) {   \
            FREEUP(buffer);                             \
            buffer = malloc(req_len);                   \
            buffer_len = req_len;                       \
        }                                               \
    }

#define REALLOC_STRING(buffer, buffer_len, req_len) {   \
        if (buffer == NULL) {                           \
            buffer = malloc(req_len);                   \
            buffer_len = req_len;                       \
        }                                               \
        else if (buffer_len < req_len) {                \
            char *tmp;                                  \
            tmp = realloc(buffer, req_len);             \
            if (tmp == NULL)                            \
                FREEUP(buffer);                         \
            buffer = tmp;                               \
            buffer_len = req_len;                       \
        }                                               \
    }

/* initParse()
 */
void
initParse(struct lsInfo *lsInfo)
{
    int i;
    int *indx;
    hEnt *hashEntPtr;

    if (logclass & (LC_TRACE | LC_SCHED))
        ls_syslog(LOG_DEBUG1, "%s: Entering this routine...", __func__);

    if (!h_TabEmpty_(&resNameTbl)) {
        h_freeRefTab_(&resNameTbl);
    }

    h_initTab_(&resNameTbl, lsInfo->nRes);

    /* We firmly believe those calloc() will
     * succeed.
     */
    indx = calloc(lsInfo->nRes, sizeof(int));
    for (i = 0; i < lsInfo->nRes; i++) {
        hashEntPtr = h_addEnt_(&resNameTbl,
                               lsInfo->resTable[i].name,
                               NULL);
        indx[i] = i;
        hashEntPtr->hData = &indx[i];
    }

    /* login, swap, idle and cpu are more human
     * readable aliases for ls, swp,it and ut (?).
     */
    indx = calloc(4, sizeof(int));
    hashEntPtr = h_addEnt_(&resNameTbl, "login", NULL);
    indx[0] = LS;
    hashEntPtr->hData = &indx[0];

    hashEntPtr = h_addEnt_(&resNameTbl, "swap", NULL);
    indx[1] = SWP;
    hashEntPtr->hData = &indx[1];

    hashEntPtr = h_addEnt_(&resNameTbl, "idle", NULL);
    indx[2] = IT;
    hashEntPtr->hData = &indx[2];

    hashEntPtr = h_addEnt_(&resNameTbl, "cpu", NULL);
    indx[3] = R1M;
    hashEntPtr->hData = &indx[3];

    if (h_TabEmpty_(&keyNameTbl)) {
        int   *key;

        key = calloc(NUM_KEYS, sizeof(int));
        h_initTab_(&keyNameTbl, NUM_KEYS);

        hashEntPtr = h_addEnt_(&keyNameTbl, "duration", NULL);
        key[0] = KEY_DURATION;
        hashEntPtr->hData = &key[0];

        hashEntPtr = h_addEnt_(&keyNameTbl, "decay", NULL);
        key[1] = KEY_DECAY;
        hashEntPtr->hData = &key[1];

        hashEntPtr = h_addEnt_(&keyNameTbl, "hosts", NULL);
        key[2] = KEY_HOSTS;
        hashEntPtr->hData = &key[2];

        hashEntPtr = h_addEnt_(&keyNameTbl, "ptile", NULL);
        key[3] = KEY_PTILE;
        hashEntPtr->hData = &key[3];
    }
}
/* getResEntry()
 */
int
getResEntry(const char *res)
{
    hEnt   *ent;
    int    *indx;

    ent = h_getEnt_(&resNameTbl, res);
    if (ent == NULL)
        return -1;

    indx = ent->hData;
    return *indx;
}

/* getKeyEntry()
 */
static int
getKeyEntry(char *key)
{
    hEnt   *ent;
    int    *x;

    ent = h_getEnt_(&keyNameTbl, key);
    if (ent == NULL)
        return -1;

    x = ent->hData;

    return *x;
}

int
parseResReq(char *resReq,
            struct resVal *resVal,
            struct lsInfo *lsInfo,
            int options)
{
    int cc;
    struct sections reqSect;

    if (logclass & (LC_TRACE | LC_SCHED))
        ls_syslog(LOG_DEBUG3, "%s: resReq=%s", __func__, resReq);

    initResVal(resVal);

    ALLOC_STRING(resVal->selectStr,
                 resVal->selectStrSize,
                 MAX(3*strlen(resReq) + 1, MAXLINELEN + MAXLSFNAMELEN));

    if ((cc = parseSection(resReq, &reqSect)) != PARSE_OK)
        return cc;

    if ((cc = setDefaults(resVal, lsInfo, options)) < 0)
        return cc;

    if (options & PR_SELECT) {

        if (options & PR_XOR) {
            if ((cc = parseSelect(reqSect.select,
                                  resVal,
                                  lsInfo,
                                  TRUE,
                                  options)) != PARSE_OK)
                return cc;
        } else {
            if ((cc = parseSelect(reqSect.select,
                                  resVal,
                                  lsInfo,
                                  FALSE,
                                  options)) != PARSE_OK)
                return cc;
	}
    }

    if (options & PR_ORDER) {
        if ((cc = parseOrder(reqSect.order,
                             resVal,
                             lsInfo)) != PARSE_OK)
            return cc;
    }

    if (options & PR_RUSAGE) {
        if ((cc = parseUsage(reqSect.rusage,
                             resVal,
                             lsInfo)) != PARSE_OK)
            return cc;
    }

    if (options & PR_FILTER) {
        if ((cc = parseFilter(reqSect.filter,
                              resVal,
                              lsInfo)) != PARSE_OK)
            return cc;
    }

    if (options & PR_SPAN) {
        if ((cc = parseSpan(reqSect.span,
                            resVal)) != PARSE_OK)
            return cc;
    }

    return PARSE_OK;
}

static int
parseSection(char *resReq, struct sections *section)
{
    static char   *reqString;
    static int    reqStringSize;
    char          *cp;
    char          *p;
    int           i;
    int           j;

#define NSECTIONS       5
#define SELECT_SECT     0
#define ORDER_SECT      1
#define RUSAGE_SECT     2
#define FILTER_SECT     3
#define SPAN_SECT       4

    static char   *keywords[] =  {
        "select",
        "order",
        "rusage",
        "filter",
        "span"
    };
    char   *sectptr[NSECTIONS];

    for (i = 0; i < NSECTIONS ; i++) {
        sectptr[i] = NULL;
    }

    if (resReq == NULL)
        return PARSE_BAD_EXP;

    ALLOC_STRING(reqString, reqStringSize, strlen(resReq)+1);
    if (reqString == NULL)
        return PARSE_BAD_MEM;

    strcpy(reqString, resReq);

    for (i = 0; i < NSECTIONS ; i++) {
        cp = reqString;
        while ((p = strstr(cp, keywords[i])) != NULL) {
            if (*(p + strlen(keywords[i])) == '[') {
                sectptr[i] = p;
                break;
            } else {
                cp = p + strlen(keywords[i]);
            }
        }
    }

    for (i = 0; i < NSECTIONS ; i++) {

        if (sectptr[i] != NULL) {

            *sectptr[i] = '\0';
            sectptr[i] += strlen(keywords[i]);
            if (*sectptr[i] != '[')
                return PARSE_BAD_EXP;
            sectptr[i]++;


            cp = sectptr[i];
            while((*cp != ']') && (*cp != '\0'))
                cp++;
            if (*cp != ']')
                return PARSE_BAD_EXP;
            *cp  = '\0';
        } else {
            if (i == SELECT_SECT)
                sectptr[i] = reqString;
            else
                sectptr[i] = (reqString + reqStringSize - 1);
        }
    }


    for (i=0; i < NSECTIONS; i++) {
        for (j=strlen(sectptr[i]) - 1; j >= 0; j--) {
            if (sectptr[i][j] == ' ')
                sectptr[i][j] = '\0';
            else
                break;
        }
    }

    section->select = sectptr[SELECT_SECT];
    section->order  = sectptr[ORDER_SECT];
    section->rusage = sectptr[RUSAGE_SECT];
    section->filter = sectptr[FILTER_SECT];
    section->span   = sectptr[SPAN_SECT];

    return PARSE_OK;

}

static int
parseSelect(char *resReq, struct resVal *resVal,
            struct lsInfo *lsInfo, bool_t parseXor, int options)
{
    int cc;
    char *expr;
    char *countPtr;
    int i;
    int numXorExprs;
    struct resVal tmpResVal;
    char *resReq2 = NULL;

    if (logclass & (LC_TRACE | LC_SCHED))
        ls_syslog(LOG_DEBUG3, "%s: resReq=%s", __func__, resReq);

    if (parseXor && resReq[0] != '\0') {

	countPtr = malloc(strlen(resReq)+1);
	if (countPtr == NULL) {
	    return(PARSE_BAD_MEM);
	}
	strcpy(countPtr, resReq);
	expr = strtok(countPtr, ",");
	if (expr == NULL) {

	    numXorExprs = 0;
	} else {
	    numXorExprs = 1;

	    while (strtok(NULL, ",")) {
	        numXorExprs++;
            }
	}
	free(countPtr);

	if (logclass & (LC_TRACE |LC_SCHED))
	    ls_syslog(LOG_DEBUG3,"%s: numXorExprs = %d", __func__, numXorExprs);

        if (numXorExprs > 1) {

	    resVal->xorExprs =
		(char **)calloc(numXorExprs + 1, sizeof(char*));
	    if (resVal->xorExprs == NULL)
		return(PARSE_BAD_MEM);


            resReq2 = malloc(strlen(resReq) + numXorExprs * 4 - 1);
	    if ( resReq2== NULL)
		return (PARSE_BAD_MEM);
	    resReq2[0] = '\0';
	    expr = strtok(resReq, ",");
	    for ( i =0 ; i < numXorExprs; i++) {
		initResVal(&tmpResVal);

		ALLOC_STRING(tmpResVal.selectStr, tmpResVal.selectStrSize,
                             MAX(3*strlen(expr) + 1, MAXLINELEN + MAXLSFNAMELEN));

    		if (tmpResVal.selectStr == NULL) {
		    FREEUP(resReq2);
        	    return PARSE_BAD_MEM;
		}
		if (setDefaults(&tmpResVal, lsInfo, options) < 0) {
		    FREEUP(resReq2);
		    return (PARSE_BAD_MEM);
		}
		if ((cc = parseSelect(expr,
                                      &tmpResVal,
                                      lsInfo,
                                      FALSE,
                                      options)) != PARSE_OK) {
		    for (i--;i>=0; i--) {
			FREEUP(resVal->xorExprs[i]);
		    }
		    FREEUP(resReq2);
		    return cc;
                }
                resVal->xorExprs[i] =
		    (char *)calloc(strlen(tmpResVal.selectStr) + 1,
				   sizeof(char));
		if (resVal->xorExprs[i] == NULL)
		    return (PARSE_BAD_MEM);

		strcpy(resVal->xorExprs[i], tmpResVal.selectStr);
		if (logclass & (LC_TRACE | LC_SCHED))
		    ls_syslog(LOG_DEBUG3,"\
%s: xorExprs[%d] = %s", __func__, i, resVal->xorExprs[i]);
                if (i == 0 ) {
                    sprintf(resReq2, "(%s)", expr);
		} else {
                    sprintf(resReq2, "%s||(%s)", resReq2, expr);
		}
		freeResVal(&tmpResVal);
		expr = strtok(NULL, ",");
	    }
	    resVal->xorExprs[i] = NULL;
	    if (logclass & (LC_TRACE | LC_SCHED))
		ls_syslog(LOG_DEBUG3,"%s: new selectStr=%s", __func__, resReq2);
            resReq = resReq2;
	} else {
	    if (numXorExprs == 1) {

		if (strchr(resReq,',')) {
	            return(PARSE_BAD_EXP);
		}
	    } else {

	        return(PARSE_BAD_EXP);
	    }
	}
    }

    if ((cc = resToClass(resReq, resVal, lsInfo)) != PARSE_OK) {
        resVal->selectStr[0] = '\0';
        FREEUP(resReq2);
        return cc;
    }

    FREEUP(resReq2);
    return PARSE_OK;
}

static int
parseOrder(char *orderReq, struct resVal *resVal, struct lsInfo *lsInfo)
{
    int i;
    int m;
    int entry;
    char *token;
    char negate;

    if ((i = strlen(orderReq)) == 0)
        return PARSE_OK;

    for(m=0; m<i; m++)
        if (orderReq[m] != ' ')
            break;

    if (m == i)
        return PARSE_OK;

    resVal->nphase = 0;
    while ((token = getNextToken(&orderReq)) != NULL) {
        negate = FALSE;
        if (token[0] == '-') {
            negate = TRUE;
            token++;
        }
        entry = getResEntry(token);
        if ((entry < 0) || (lsInfo->resTable[entry].orderType == NA))
            return PARSE_BAD_NAME;

        if (resVal->nphase > NBUILTINDEX)
            break;

        if (negate)
            resVal->order[resVal->nphase] = -(entry +1);
        else
            resVal->order[resVal->nphase] =  entry + 1;
        resVal->nphase++;
    }
    resVal->options |= PR_ORDER;
    return PARSE_OK;
}


static int
parseFilter(char *filter, struct resVal *resVal, struct lsInfo *lsInfo)
{
    int i;
    int m;
    int entry;
    char *token;

    if ((i = strlen(filter)) == 0)
        return PARSE_OK;

    for (m = 0; m < i; m++)
        if (filter[m] != ' ')
            break;

    if (m == i)
        return PARSE_OK;

    resVal->nindex = 0;
    while ((token = getNextToken(&filter)) != NULL) {
        entry = getResEntry(token);

        if (entry <  0)
            return(PARSE_BAD_FILTER);
        if (!(lsInfo->resTable[entry].flags & RESF_DYNAMIC))
            return(PARSE_BAD_FILTER);
        if (lsInfo->resTable[entry].flags & RESF_SHARED)
            return(PARSE_BAD_FILTER);
        resVal->indicies[resVal->nindex++] = entry;
    }
    resVal->options |= PR_FILTER;
    return(PARSE_OK);
}

static int
parseUsage(char *usageReq, struct resVal *resVal, struct lsInfo *lsInfo)
{
    int i;
    int m;
    int entry;
    float value;
    char *token;
    link_t *link;
    linkiter_t iter;
    struct _rusage_ *r;
    char *s;
    int *rusage_bit_map;

    if ((i = strlen(usageReq)) == 0)
        return PARSE_OK;

    for (m = 0; m < i; m++)
        if (usageReq[m] != ' ')
            break;
    if (m == i)
        return PARSE_OK;

    resVal->rl = make_link();
    link = get_rusage_entries(usageReq);

    i = 0;
    traverse_init(link, &iter);
    while ((s = traverse_link(&iter))) {

        rusage_bit_map =  calloc(GET_INTNUM(lsInfo->nRes), sizeof(int));

        resVal->genClass = 0;
        while ((token = getNextToken(&s)) != NULL) {

            if (token[0] == '-')
                token++;

            entry = getKeyEntry(token);
            if (entry > 0) {
                if (entry != KEY_DURATION && entry != KEY_DECAY)
                    goto pryc;

                if (usageReq[0] == '=') {
                    int returnValue;
                    if (entry == KEY_DURATION)
                        returnValue =  getTimeVal(&s, &value);
                    else
                        returnValue = getVal(&s, &value);
                    if (returnValue < 0 || value < 0.0)
                        return PARSE_BAD_VAL;
                    if (entry == KEY_DURATION)
                        resVal->duration = value;
                    else
                        resVal->decay = value;

                    continue;
                }
            }

            entry = getResEntry(token);
            if (entry < 0)
                return PARSE_BAD_NAME;

            if (!(lsInfo->resTable[entry].flags & RESF_DYNAMIC)
                && (lsInfo->resTable[entry].valueType != LS_NUMERIC)) {
                if (usageReq[0] == '=') {
                    if (getVal(&s, &value) < 0 || value < 0.0)
                        goto pryc;
                }
                continue;
            }

            if (entry < MAXSRES)
                resVal->genClass |= 1 << entry;

            SET_BIT(entry, rusage_bit_map);

            if (s[0] == '=') {
                if (getVal(&s, &value) < 0 || value < 0.0)
                    goto pryc;
            }
        }

        r = calloc(1, sizeof(struct _rusage_));
        r->bitmap = rusage_bit_map;
        r->val = calloc(lsInfo->nRes, sizeof(float));
        r->val[entry] = value;
        enqueue_link(resVal->rl, r);

        if (i == 0) {
            /* The entry 0 is both in the link and
             * in the resVal.
             */
            resVal->rusage_bit_map = rusage_bit_map;
            resVal->val[entry] = value;
        }
        ++i;
    } /* while (s = traverse_link()) */

    resVal->options |= PR_RUSAGE;
    return PARSE_OK;

    while ((s = pop_link(link)))
        _free_(s);
    fin_link(link);

pryc:

    while ((s = pop_link(link)))
        _free_(s);
    fin_link(link);
    while ((r = pop_link(resVal->rl))) {
        _free_(r->bitmap);
        _free_(r->val);
        _free_(r);
    }
    fin_link(resVal->rl);

    return PARSE_BAD_NAME;
}

static int
parseSpan(char *usageReq, struct resVal *resVal)
{
    int i;
    int m;
    int entry;
    int val1;
    int val2;
    char *token;

    if ((i = strlen(usageReq)) == 0)
        return PARSE_OK;

    for(m = 0; m < i; m++)
        if (usageReq[m] != ' ')
            break;
    if (m == i)
        return PARSE_OK;

    while ((token = getNextToken(&usageReq)) != NULL) {

        if (token[0] == '-')
            token++;

        entry = getKeyEntry (token);
        if (entry >= 0 && entry < KEY_HOSTS)
            return(PARSE_BAD_NAME);
        if (getValPair (&usageReq, &val1, &val2) < 0
            || val1 <= 0.0 || val2 <= 0.0)
            return PARSE_BAD_VAL;
        switch (entry) {
            case KEY_HOSTS:
                resVal->numHosts = val1;
                resVal->maxNumHosts = val2;
                break;
            case KEY_PTILE:
                resVal->pTile = val1;
                if (val2 != INFINIT_INT)
                    return PARSE_BAD_VAL;
                break;
            default:
                return(PARSE_BAD_NAME);
        }
    }
    resVal->options |= PR_SPAN;
    return PARSE_OK;
}

static int
validValue(char *value, struct lsInfo *lsInfo, int nentry)
{
    int i;

    if (strcmp(value, WILDCARD_STR) == 0
        || strcmp(value, LOCAL_STR) == 0 )
        return 0;

    if (strcmp(lsInfo->resTable[nentry].name,"type") == 0) {
        for(i = 0; i < lsInfo->nTypes; i++)
            if (strcmp(lsInfo->hostTypes[i], value) == 0)
                break;
        if (i == lsInfo->nTypes)
            return -1;
        else
            return 0;
    }

    if (strcmp(lsInfo->resTable[nentry].name,"model") == 0) {
        for(i = 0; i < lsInfo->nModels; i++)
            if (strcmp(lsInfo->hostModels[i], value) == 0)
                break;
        if (i == lsInfo->nModels)
            return -1;
        else
            return 0;
    }

    if (strcmp(lsInfo->resTable[nentry].name,"status") == 0) {
        if (  strcmp(value,"ok") == 0    || strcmp(value,"busy") == 0  ||
              strcmp(value,"lockU") == 0 || strcmp(value,"lockW") == 0 ||
              strcmp(value,"lock") == 0  || strcmp(value,"lockM") == 0 ||
	      strcmp(value,"lockUW") == 0|| strcmp(value,"lockUM") == 0||
	      strcmp(value,"lockWM") == 0|| strcmp(value,"lockUWM") == 0 ||
	      strcmp(value,"unavail") == 0 )
            return 0;
        else
            return -1;
    }
    return 0;
}


static int
resToClass(char *resReq, struct resVal *resVal, struct lsInfo *lsInfo)
{
    int i;
    int s;
    int t;
    int len;
    int entry;
    int hasQuote;
    char res[MAXLSFNAMELEN], val[MAXLSFNAMELEN];
    char tmpbuf[MAXLSFNAMELEN*2];
    char *sp;
    char *op;

    len = strlen(resReq);

    sp = resVal->selectStr;
    strcpy(sp, "expr ");
    s = 0;
    t = strlen(sp);

    while (s < len) {

        if (t >= (resVal->selectStrSize - MAXLSFNAMELEN))
            return PARSE_BAD_EXP;

        if (resReq[s] == ' ') {
            s++;
            continue;
        }

        if (resReq[s] == '(' || resReq[s] == ')' || resReq[s] == '=' ||
            resReq[s] == '!' || resReq[s] == '>' || resReq[s] == '<' ||
            resReq[s] == '|' || resReq[s] == '&' || resReq[s] == '+' ||
            resReq[s] == '-' || resReq[s] == '*' || resReq[s] == '/' ||
            resReq[s] == '.' || isdigit(resReq[s])) {

            sp[t++] = resReq[s++];
            continue;
        }

        if (! isalpha(resReq[s]))
            return PARSE_BAD_EXP;

        if (isalpha(resReq[s])) {

            i = 0;
            while (isalnum(resReq[s])
                   || ispunct(resReq[s]))
                res[i++] = resReq[s++];
            res[i] = '\0';

            entry = getResEntry(res);

            if (entry < 0) {
                if (strncmp ("defined", res, strlen (res)) == 0) {
                    while (resReq[s] == ' ')
                        s++;
                    if (resReq[s] != '(')
                        return (PARSE_BAD_EXP);
                    i = 0;
		    s++;
                    while (isalnum(resReq[s])
                           || ispunct(resReq[s]))
                        res[i++] = resReq[s++];
                    res[i] = '\0';
                    entry = getResEntry(res);
                    if (entry < 0)
                        return PARSE_BAD_NAME;
		    sprintf(tmpbuf,"[%s \"%s\" ]",
                            "defined", lsInfo->resTable[entry].name);
                    sp[t] = '\0';
                    strcat(sp,tmpbuf);
                    t += strlen(tmpbuf);
                    while (resReq[s] == ' ')
                        s++;
                    if (resReq[s] != ')')
                        return PARSE_BAD_EXP;
                    s++;
                    continue;
                }
                return PARSE_BAD_NAME;
            }

            switch(lsInfo->resTable[entry].valueType) {

                case LS_NUMERIC:
                case LS_BOOLEAN:
                    strcat(res,"()");
                    sp[t] = '\0';
                    strcat(sp,res);
                    t += strlen(res);
                    break;
                case LS_STRING:

                    while(resReq[s] == ' ')
                        s++;

                    if (resReq[s] == '\0' || resReq[s+1] == '\0')
                        return PARSE_BAD_EXP;


                    op = NULL;
                    if (resReq[s] == '=' && resReq[s+1] == '=') {
                        op = "eq";
                        s += 2;
                    } else if (resReq[s] == '!' && resReq[s+1] == '=') {
                        op = "ne";
                        s += 2;
                    } else if (resReq[s] == '>' && resReq[s+1] == '=') {
                        op = "ge";
                        s += 2;
                    } else if (resReq[s] == '<' && resReq[s+1] == '=') {
                        op = "le";
                        s += 2;
                    } else if (resReq[s] == '<') {
                        op = "lt";
                        s += 1;
                    } else if (resReq[s] == '>') {
                        op = "gt";
                        s += 1;
                    } else {
                        return -1;
                    }

                    while(resReq[s] == ' ')
                        s++;

                    if (resReq[s] == '\''){
                        hasQuote = TRUE;
                        s++;
                    } else
                        hasQuote = FALSE;
                    i = 0;
                    if (!hasQuote){
                        while(isalnum(resReq[s])
                              || ispunct(resReq[s]))
                            val[i++] = resReq[s++];
                    } else {

                        while(resReq[s] && resReq[s] != '\''
                              && i < MAXLSFNAMELEN)
                            val[i++] = resReq[s++];

                        if (i - 1 == MAXLSFNAMELEN)
                            return PARSE_BAD_VAL;

                        if (resReq[s] == '\'')
                            s++;
                    }

                    val[i] = '\0';
                    if (i == 0) {
                        return PARSE_BAD_VAL;
                    }

                    if (validValue(val, lsInfo, entry) < 0) {
                        return PARSE_BAD_VAL;
                    }

                    sprintf(tmpbuf,"[%s \"%s\" \"%s\"]",
                            lsInfo->resTable[entry].name, op, val);
                    sp[t] = '\0';
                    strcat(sp,tmpbuf);
                    t += strlen(tmpbuf);
                default:
                    break;
            }
        } else {
            return (PARSE_BAD_EXP);
        }
    }
    sp[t] = '\0';
    resVal->options |= PR_SELECT;

    return PARSE_OK;
}

static int
setDefaults(struct resVal *resVal, struct lsInfo *lsInfo, int options)
{
    int i;

    if (options & PR_DEFFROMTYPE)
        strcpy(resVal->selectStr, "expr [type \"eq\" \"local\"]");
    else
        strcpy(resVal->selectStr, "expr [type \"eq\" \"any\"]");

    resVal->nphase = 2;
    resVal->order[0] = R15S + 1;
    resVal->order[1] = PG + 1;
    resVal->val = calloc(lsInfo->nRes, sizeof(float));
    resVal->indicies = calloc((lsInfo->numIndx), sizeof(int));
    if (!resVal->val || !resVal->indicies) {
	freeResVal (resVal);
        lserrno = LSE_MALLOC;
        ls_perror("intlib:resreq");
        return PARSE_BAD_MEM;
    }

    for (i = 0; i < lsInfo->nRes; i++)
        resVal->val[i] = INFINIT_LOAD;

    resVal->genClass =  0;
    if (!(options & PR_BATCH)) {
        resVal->genClass |=  1 << R15S;
        resVal->genClass |=  1 << R1M;
        resVal->genClass |=  1 << R15M;
        resVal->val[R15S] = 1.0;
        resVal->val[R1M]  = 1.0;
        resVal->val[R15M] = 1.0;
    }

    resVal->nindex = lsInfo->numIndx;
    for(i = 0; i < resVal->nindex; i++)
        resVal->indicies[i] = i;

    resVal->rusage_bit_map = calloc (GET_INTNUM(lsInfo->nRes), sizeof(int));
    if (resVal->rusage_bit_map == NULL) {
	lserrno = LSE_MALLOC;
	freeResVal (resVal);
        return PARSE_BAD_MEM;
    }

    for (i = 0; i < GET_INTNUM(lsInfo->nRes); i++)
	resVal->rusage_bit_map[i] = 0;

    if (!(options &PR_BATCH)) {
        SET_BIT(R15S, resVal->rusage_bit_map);
        SET_BIT(R1M, resVal->rusage_bit_map);
        SET_BIT(R15M, resVal->rusage_bit_map);
    }

    resVal->duration = INFINIT_INT;
    resVal->decay = INFINIT_FLOAT;
    resVal->numHosts = INFINIT_INT;
    resVal->maxNumHosts = INFINIT_INT;
    resVal->pTile = INFINIT_INT;

    resVal->options = 0;
    return 0;
}

void
freeResVal(struct resVal *resVal)
{
    if (resVal == NULL)
	return;

    FREEUP (resVal->val);
    FREEUP (resVal->indicies);
    FREEUP (resVal->selectStr);
    resVal->selectStrSize = 0;
    FREEUP (resVal->rusage_bit_map);
    if (resVal->xorExprs) {
        int i;
	for (i = 0; resVal->xorExprs[i]; i++)
	    FREEUP(resVal->xorExprs[i]);
	FREEUP(resVal->xorExprs);
    }
}

void
initResVal (struct resVal *resVal)
{
    if (resVal == NULL)
        return;

    resVal->val = NULL;
    resVal->indicies = NULL;
    resVal->rusage_bit_map = NULL;
    resVal->selectStr = NULL;
    resVal->selectStrSize= 0;
    resVal->xorExprs = NULL;
}


static int
getTimeVal(char **time, float *val)
{
    char *token, *cp, oneChar = '\0';

    token = getNextToken(time);
    cp = token;
    while (isdigit(*cp))
	cp++;

    if (*cp == 'm' || *cp == 'h' || *cp == 's') {
        oneChar = *cp;
        *cp = '\0';
    }
    if (!isanumber_(token))
        return -1;
    *val = atof(token);

    if (oneChar == 'h')
        *val *= 60 * 60;
    else if ((oneChar == 'm') || (oneChar == '\0')){
        *val *= 60;
    }
    return 0;
}

static int
getVal(char **resReq, float *val)
{
    char *token;

    token = getNextToken(resReq);
    if (!isanumber_(token))
        return -1;
    *val = atof(token);

    return 0;
}

/* get_rusage_entries()
 */
static link_t *
get_rusage_entries(const char *s)
{
    char *p;
    char *ss;
    char *z;
    link_t *l;

    l = make_link();

    /* Return an empty link
     */
    ss = strdup(s);
    if (! strstr(ss, "||")) {
        enqueue_link(l, ss);
        return l;
    }

    while ((p = strstr(ss, "||"))) {
        *p = 0;
        z = strdup(ss);
        enqueue_link(l, z);
        ss = p + 2;
    }

    z = strdup(ss);
    enqueue_link(l, z);

    return l;
}

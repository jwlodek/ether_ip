/* $Id$ */

#include <vxWorks.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <alarm.h>
#include <cvtTable.h>
#include <dbDefs.h>
#include <dbAccess.h>
#include <recSup.h>
#include <devSup.h>
#include <link.h>
#include <devLib.h>
#include <dbScan.h>
#include <menuConvert.h>
#include <aiRecord.h>
#include <biRecord.h>
#include <mbbiRecord.h>
#include <mbbiDirectRecord.h>
#include <aoRecord.h>
#include <boRecord.h>
#include <mbboRecord.h>
#include <mbboDirectRecord.h>
#include "drvEtherIP.h"

/* flags that pick special values instead of the tag's "value" */
typedef enum
{
    SPCO_VALUE, /* default */
    SPCO_READ_SINGLE_ELEMENT,
    SPCO_SCAN_PERIOD,
    SPCO_BIT,
    SPCO_PLC_ERRORS,
    SPCO_PLC_TASK_SLOW,
    SPCO_LIST_ERRORS,
    SPCO_LIST_TICKS,
    SPCO_LIST_SCAN_TIME,
    SPCO_LIST_MIN_SCAN_TIME,
    SPCO_LIST_MAX_SCAN_TIME,
    SPCO_TAG_TRANSFER_TIME,
    SPCO_INVALID
} SpecialOptions;

static struct
{
    const char *text;
    size_t      len;
} special_options[] =
{
  { "VALUE",               5 },
  { "E",                   1 },
  { "S ",                  2 }, /* note <space> */
  { "B ",                  2 }, /* note <space> */
  { "PLC_ERRORS",         10 }, /* Connection error count for tag's PLC */
  { "PLC_TASK_SLOW",      13 }, /* How often scan task had no time to wait */
  { "LIST_ERRORS",        11 }, /* Error count for tag's list */
  { "LIST_TICKS",         10 }, /* Ticktime when tag's list was checked */
  { "LIST_SCAN_TIME",     14 }, /* Time for handling scanlist */
  { "LIST_MIN_SCAN_TIME", 18 }, /* min. of '' */
  { "LIST_MAX_SCAN_TIME", 18 }, /* max. of '' */
  { "TAG_TRANSFER_TIME",  17 }, /* Time for last round-trip data request */
};

/* Device Private:
 * Link text is kept to check for changes when record is processed
 * (faster than re-parsing every time).
 *
 * Note on element for binary records:
 * We assume that tag refers to BOOL array,
 * so the original element that refers to a bit #
 * is transformed into
 * 1) element in the UDINT array
 * 2) mask then points to the bit inside that UDINT
 */
typedef struct
{
    char           *link_text;  /* Original text of INST_IO link  */
    char           *PLC_name;   /* PLC, */
    char           *string_tag; /* tag, */
    size_t         element;     /* array element parsed from that: 0, 1,... */
    CN_UDINT       mask;        /* For binaries: first bit of interest */
    SpecialOptions special;
    PLC            *plc;
    TagInfo        *tag;
    IOSCANPVT      ioscanpvt;
}   DevicePrivate;

/* Call to this routine can be triggered via TPRO field */
static void dump_DevicePrivate(const dbCommon *rec)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;

    if (!pvt)
    {
        printf("   Device Private = 0\n");
        return;
    }
    printf("   link_text  : '%s'\n",  pvt->link_text);
    printf("   PLC_name   : '%s'\n",  pvt->PLC_name);
    printf("   string_tag : '%s'\n",  pvt->string_tag);
    printf("   element    : %d\n",    pvt->element);
    printf("   mask       : 0x%0X\n", (unsigned int)pvt->mask);
    printf("   spec. opts.: %d\n",    pvt->special);
    printf("   plc        : 0x%0X\n", (unsigned int)pvt->plc);
    printf("   tag        : 0x%0X\n", (unsigned int)pvt->tag);
}

/* Helper: check for valid DevicePrivate, lock data
 * and see if it's valid */
static bool lock_data(const dbCommon *rec)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    if (!pvt ||
        !pvt->plc ||
        !pvt->tag ||
        !pvt->tag->scanlist)
        return false;
    if (semTake(pvt->tag->data_lock, sysClkRateGet()) != OK)
    {
        if (rec->sevr != INVALID_ALARM) /* don't flood w/ messages */
            errlogPrintf("devEtherIP (%s): no data_lock\n", rec->name);
        return false;
    }
    if (pvt->tag->valid_data_size <= 0  ||
        pvt->tag->elements <= pvt->element)
    {
        semGive(pvt->tag->data_lock);
        return false;
    }
    return true;
}

/* Helper for (multi-bit) binary type records:
 * Get bits from driver, pack them into rval
 *
 * Does not take/give data lock because output records
 * might need it after looking at the value.
 *
 * The mbbx's MASK is not used because
 * 1) NOBT might change but MASK is only set once
 * 2) MASK doesn't help when reading bits accross UDINT boundaries
 */
static bool get_bits(dbCommon *rec, size_t bits, unsigned long *rval)
{
    DevicePrivate  *pvt = (DevicePrivate *)rec->dpvt;
    bool           ok;
    size_t         i, element;
    CN_UDINT       value, mask;
    
    element = pvt->element;
    mask    = pvt->mask;
    *rval   = 0;
    ok = get_CIP_UDINT(pvt->tag->data, element, &value);
    if (ok)
    {
        /* Fetch bits from BOOL array.
         * First one directly (faster for BI case),
         * rest in loop */
        if (value & mask)
            *rval = 1;
        for (i=1/*!*/; i<bits; ++i)
        {
            mask <<= 1;
            if (mask == 0) /* end of current UDINT ? */
            {
                mask = 1;
                ++element;
                ok = get_CIP_UDINT(pvt->tag->data, element, &value);
                if (! ok)
                    break;
            }
            if (value & mask)
                *rval |= (unsigned long)1 << i;
        }
    }

    return ok;
}

/* Pendant to get_bits */
static bool put_bits(dbCommon *rec, size_t bits, unsigned long rval)
{
    DevicePrivate  *pvt = (DevicePrivate *)rec->dpvt;
    size_t         i, element = pvt->element;
    CN_UDINT       value, mask = pvt->mask;
    bool           ok = true;
    
    if (! get_CIP_UDINT(pvt->tag->data, element, &value))
        return false;
    /* Transfer bits into BOOL array.
     * First one directly (faster for BI case), rest in loop */
    if (rval & 1)
        value |= mask;
    else
        value &= ~mask;
    for (i=1/*!*/; i<bits; ++i)
    {
        rval >>= 1;
        mask <<= 1;
        if (mask == 0) /* end of current UDINT ? */
        {
            if (! (ok = put_CIP_UDINT (pvt->tag->data, element, value)))
                break;
            mask = 1; /* reset mask, go to next element */
            ++element;
            if (! (ok = get_CIP_UDINT (pvt->tag->data, element, &value)))
                break;
        }
        if (rval & 1)
            value |= mask;
        else
            value &= ~mask;
    }

    if (ok && put_CIP_UDINT(pvt->tag->data, element, value))
    {
        pvt->tag->do_write = true;
        return true;
    }
    
    return false;
}

/* Callback, registered with drvEtherIP, for input records.
 * Used _IF_ scan="I/O Event":
 * Driver has new value, process record
 */
static void scan_callback(void *arg)
{
    dbCommon      *rec = (dbCommon *) arg;
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    if (rec->tpro)
        printf("EIP scan_callback ('%s')\n", rec->name);
    scanIoRequest(pvt->ioscanpvt);
}

/* Callback for ao:
 * Used for every AO record to check if
 * PLC's value != record's idea of the current value,
 * causing process if necessary to update the record.
 */
static void check_ao_callback (void *arg)
{
    aoRecord      *rec = (aoRecord *) arg;
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    double        dbl;
    CN_UDINT      udint;
    bool          process = false;

    /* Check if record's (R)VAL is current */
    if (lock_data ((dbCommon *)rec))
    {
        if (get_CIP_typecode(pvt->tag->data) == T_CIP_REAL)
        {
            if (get_CIP_double (pvt->tag->data, pvt->element, &dbl) &&
                rec->val != dbl)
            {
                rec->val = rec->pval = dbl;
                process = true;
                if (rec->tpro)
                    printf ("AO '%s': got %g from driver\n",
                            rec->name, dbl);
            }
        }
        else
        {
            if (get_CIP_UDINT (pvt->tag->data, pvt->element, &udint) &&
                rec->rval != udint)
            {
                if (rec->tpro)
                    printf ("AO '%s': got %lu from driver\n",
                            rec->name, udint);
                /* back-convert raw value into val (copied from ao init) */
                dbl = (double)udint + (double)rec->roff;
                if (rec->aslo!=0.0)
                    dbl *= rec->aslo;
                dbl += rec->aoff;
                switch (rec->linr)
                {
                    case menuConvertNO_CONVERSION:
                        rec->val = rec->pval = dbl;
                        process = true;
                        break;
                    case menuConvertLINEAR:
                        dbl = dbl*rec->eslo + rec->eoff;
                        rec->val = rec->pval = dbl;
                        process = true;
                        break;
                    default:
                        if (cvtRawToEngBpt(&dbl,rec->linr,rec->init,
                                           (void *)&rec->pbrk,&rec->lbrk)!=0)
                            break; /* cannot back-convert */
                        rec->val = rec->pval = dbl;
                        process = true;
                }
                if (rec->tpro)
                    printf ("--> val = %g\n", rec->val);
            }
        }
        semGive (pvt->tag->data_lock);
    }
    /* Does record need processing and is not periodic? */
    if (process && rec->scan < SCAN_1ST_PERIODIC)
        scanOnce (rec);
}

/* Callback for bo */
static void check_bo_callback (void *arg)
{
    boRecord      *rec = (boRecord *) arg;
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    unsigned long rval;
    bool          process = false;

    /* Check if record's (R)VAL is current */
    if (lock_data ((dbCommon *) rec))
    {
        if (get_bits ((dbCommon *)rec, 1, &rval) &&
            rec->rval != rval)
        {
            if (rec->tpro)
                printf ("BO '%s': got %lu from driver\n",
                        rec->name, rval);
            /* back-convert rval into val */
            rec->rval = rval;
            rec->val  = (rec->rval==0) ? 0 : 1;
            if (rec->tpro)
                printf ("--> val = %u\n", (unsigned int)rec->val);
            process = true;
        }
        semGive (pvt->tag->data_lock);
    }
    /* Does record need processing and is not periodic? */
    if (process && rec->scan < SCAN_1ST_PERIODIC)
        scanOnce (rec);
}

/* Callback for mbbo */
static void check_mbbo_callback (void *arg)
{
    mbboRecord    *rec = (mbboRecord *) arg;
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    unsigned long rval, *state_val;
    size_t        i;
    bool          process = false;

    /* Check if record's (R)VAL is current */
    if (lock_data ((dbCommon *) rec))
    {
        if (get_bits ((dbCommon *)rec, rec->nobt, &rval) &&
            rec->rval != rval)
        {
            if (rec->tpro)
                printf ("MBBO '%s': got %lu from driver\n",
                        rec->name, rval);
            /* back-convert rval into val */
            if (rec->sdef)
            {
                rec->val  = 65535;  /* initalize to unknown state*/
                state_val = &rec->zrvl;
                for (i=0; i<16; ++i)
                {
                    if (*state_val == rval)
                    {
                        rec->val = i;
                        break;
                    }
                    state_val++;
                }
            }
            else
            {
                /* the raw value is the desired val */
                rec->val = (unsigned short)rval;
            }
            if (rec->tpro)
                printf ("--> val = %u\n", (unsigned int)rec->val);
            process = true;
        }
        semGive (pvt->tag->data_lock);
    }
    /* Does record need processing and is not periodic? */
    if (process && rec->scan < SCAN_1ST_PERIODIC)
        scanOnce (rec);
}

/* Callback for mbboDirect */
static void check_mbbo_direct_callback (void *arg)
{
    mbboDirectRecord *rec = (mbboDirectRecord *) arg;
    DevicePrivate    *pvt = (DevicePrivate *)rec->dpvt;
    unsigned long    rval;
    bool             process = false;

    /* Check if record's (R)VAL is current */
    if (lock_data ((dbCommon *) rec))
    {
        if (get_bits ((dbCommon *)rec, rec->nobt, &rval) &&
            rec->rval != rval)
        {
            if (rec->tpro)
                printf ("MBBODirect '%s': got %lu from driver\n",
                        rec->name, rval);
            rec->rval= rval;
            rec->val = (unsigned int) rval;
            process = true;
        }
        semGive (pvt->tag->data_lock);
    }
    /* Does record need processing and is not periodic? */
    if (process && rec->scan < SCAN_1ST_PERIODIC)
        scanOnce (rec);
}

/* device support routine get_ioint_info */
static long get_ioint_info(int cmd, dbCommon *rec, IOSCANPVT *ppvt)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    if (pvt)
        *ppvt = pvt->ioscanpvt;
    return 0;
}

/* Try to parse scan period from record's SCAN field.
 * Sounds simple, but I ended up with this mess.
 * Is there a more elegant way to get this?
 * Returns <= 0 for error.
 */
static double get_period(dbCommon *rec)
{
    char          *buf = 0, *p;
    size_t        buf_size = 0, len;
    struct dbAddr scan_field;
    long          options=0, count=1;
    double        period = -1.0;

    if (rec->scan < SCAN_1ST_PERIODIC)
        return period;
    
    /* Best guess for holding SCAN field name and value */
    if (! EIP_reserve_buffer((void**)&buf, &buf_size, 50))
        return period;
    /* Get SCAN field's address */
    len = strlen (rec->name);
    if (! EIP_reserve_buffer((void**)&buf, &buf_size, len+6))
        goto leave;
    memcpy(buf, rec->name, len);
    memcpy(buf+len, ".SCAN", 6);
    if (dbNameToAddr(buf, &scan_field) != 0)
        goto leave;

    /* Get value */
    len = dbBufferSize(DBR_STRING, options, count);
    if (! EIP_reserve_buffer((void**)&buf, &buf_size, len))
        goto leave;
    if (dbGet(&scan_field, DBR_STRING, buf, &options, &count, 0) != 0)
        goto leave;
    if (! strstr(buf, "second"))
        goto leave;
    period = strtod(buf, &p);
    if (p==buf || period==HUGE_VAL || period==-HUGE_VAL)
        period = -1.0;
  leave:
    free(buf);
    return period;
}

/* Given string "s", return next token and end of token
 * Returns 0 on end-of-string
 */
static char *find_token(char *s, char **end)
{
    /* skip initial space  */
    while (*s == ' ')
        ++s;

    if (*s == '\0')
        return 0;
    
    *end = strchr(s, ' ');
    if (*end == NULL)
        *end = s + strlen(s);

    return s;
}

/* This driver uses INST_IO links,
 * the string has to be of the format
 *
 *      @PLC tag flags
 *
 * bits: if >0 indicates that we want binary data,
 *       then it's the number of bits
 */
static long analyze_link(dbCommon *rec,
                         EIPCallback cbtype,
                         const DBLINK *link, size_t bits)
{
    DevicePrivate  *pvt = (DevicePrivate *)rec->dpvt;
    char           *p, *end;
    size_t         i, tag_len, last_element, bit=0;
    double         period = 0.0;
    bool           single_element = false;
    SpecialOptions special = SPCO_VALUE;
    
    if (! EIP_strdup(&pvt->link_text, link->value.instio.string,
                     strlen (link->value.instio.string)))
    {
        errlogPrintf("devEtherIP (%s): Cannot copy link\n", rec->name);
        return S_dev_noMemory;
    }
    /* Find PLC */
    p = find_token(pvt->link_text, &end);
    if (! p)
    {
        errlogPrintf("devEtherIP (%s): Missing PLC in link '%s'\n",
                     rec->name, pvt->link_text);
        return S_db_badField;
    }
    if (! EIP_strdup(&pvt->PLC_name, p, end-p))
    {
        errlogPrintf("devEtherIP (%s): Cannot copy PLC\n", rec->name);
        return S_dev_noMemory;
    }

    /* Find Tag */
    p = find_token(end, &end);
    if (! p)
    {
        errlogPrintf("devEtherIP (%s): Missing tag in link '%s'\n",
                     rec->name, pvt->link_text);
        return(S_db_badField);
    }
    tag_len = end-p;
    if (! EIP_strdup(&pvt->string_tag, p, tag_len))
    {
        errlogPrintf ("devEtherIP (%s): Cannot copy tag\n", rec->name);
        return S_dev_noMemory;
    }
    
    /* Check for more flags */
    while ((p = find_token(end, &end)))
    {
        for (i=1; i<SPCO_INVALID; ++i)
        {
            if (strncmp(p,
                        special_options[i].text,
                        special_options[i].len) == 0)
            {
                if (i==SPCO_READ_SINGLE_ELEMENT)
                        single_element = true;
                else if (i==SPCO_SCAN_PERIOD)
                {
                    period = strtod(p+2, &end);
                    if (end==p || period==HUGE_VAL || period==-HUGE_VAL)
                    {
                        errlogPrintf("devEtherIP (%s): "
                                     "Error in scan flag in link '%s'\n",
                                     rec->name, p, pvt->link_text);
                        return S_db_badField;
                    }
                }
                else if (i==SPCO_BIT)
                {
                    bit = strtod(p+2, &end);
                    special = SPCO_BIT;
                    if (end==p || period==HUGE_VAL || period==-HUGE_VAL)
                    {
                        errlogPrintf("devEtherIP (%s): "
                                     "Error in bit flag in link '%s'\n",
                                     rec->name, p, pvt->link_text);
                        return S_db_badField;
                    }
                }
                else
                    special = i;
                break;
            }
        }
        if (i == SPCO_INVALID)
        {
            errlogPrintf("devEtherIP (%s): Invalid flag '%s' in link '%s'\n",
                         rec->name, p, pvt->link_text);
            return S_db_badField;
        }
    }
    
    pvt->special = special;
    if (period <= 0.0) /* no scan flag-> get SCAN field: */
    {
        period = get_period(rec);
        if (period <= 0)
        {
            errlogPrintf("devEtherIP (%s): cannot decode SCAN field,"
                         " no scan flag given\n", rec->name);
            return S_db_badField;
        }
    }
    
    /* Parsed link_text into PLC_name, string_tag, special flags.
     * Analyse further */
    pvt->element = 0;
    p = &pvt->string_tag[tag_len-1];
    if (*p == ']') /* array tag? */
    {
        if (! single_element)
        {   /* Cut "array_tag[el]" into "array_tag" + el */
            while (p > pvt->string_tag)
                if (*(--p) == '[')
                    break;
            if (p <= pvt->string_tag)
            {
                errlogPrintf("devEtherIP (%s): malformed array tag in '%s'\n",
                             rec->name, pvt->link_text);
                return S_db_badField;
            }
            /* read element number */
            pvt->element = strtol(p+1, &end, 0);
            if (end==p+1 || pvt->element==LONG_MAX || pvt->element == LONG_MIN)
            {
                errlogPrintf("devEtherIP (%s): malformed array tag in '%s'\n",
                             rec->name, pvt->link_text);
                return S_db_badField;
            }
            /* remove element number text from tag */
            *p = '\0';
        }
    }

    pvt->plc = drvEtherIP_find_PLC(pvt->PLC_name);
    if (! pvt->plc)
    {
        errlogPrintf("devEtherIP (%s): unknown PLC '%s'\n",
                     rec->name, pvt->PLC_name);
        return S_db_badField;
    }

    /* For Element==0 the following makes no difference, only
     * for binary records (bits=1 or more)
     * Options:
     * a) assume BOOL array (default)
     * b) non-BOOL, SPCO_BIT selected a bit in INT, DINT, ...
     */
    if (bits>0 && special != SPCO_BIT)
    {
        /* For element>0, assume that it's a BOOL array,
         * so the data is packed into UDINTs (CIP "BITS").
         * The actual element requested is the UDINT index,
         * not the bit#.
         * Pick the bits within the UDINT via the mask. */
        pvt->mask = 1U << (pvt->element & 0x1F); /* 0x1F == 31 */
        last_element = pvt->element + bits - 1;
        pvt->element >>= 5;
        last_element >>= 5;
    }
    else
    {   /* Default (no binary rec) OR bit flag was given:
         * Use the element as is, use bit # (default:0)   */
        last_element = pvt->element;
        pvt->mask = 1U << bit;
    }

    /* tell driver to read up to this record's elements */
    pvt->tag = drvEtherIP_add_tag(pvt->plc, period,
                                  pvt->string_tag,
                                  last_element+1);
    if (! pvt->tag)
    {
        errlogPrintf("devEtherIP (%s): cannot register tag '%s' with driver\n",
                     rec->name, pvt->string_tag);
        return S_db_badField;
    }

    if (cbtype == scan_callback)
    {   /* scan_callback only allowed for SCAN=I/O Intr */
        if (rec->scan == SCAN_IO_EVENT)
            drvEtherIP_add_callback(pvt->plc, pvt->tag,
                                    scan_callback, rec);
        else
            drvEtherIP_remove_callback(pvt->plc, pvt->tag,
                                       scan_callback, rec);
    }
    else
        drvEtherIP_add_callback(pvt->plc, pvt->tag, cbtype, rec);
    
    return 0;
}

/* Check if link still matches what was recorded in DevicePrivate.
 * On change, reparse and restart driver */
static long check_link(dbCommon *rec, EIPCallback cbtype,
                       const DBLINK *link, size_t bits)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    long status;

    if (link->type != INST_IO)
    {
        errlogPrintf("devEtherIP (%s): INP is not INST_IO\n", rec->name);
        return S_db_badField;
    }
    if (strcmp(link->value.instio.string, pvt->link_text))
    {   /* Link has changed, start over */
        rec->udf = TRUE;
        if (pvt->plc && pvt->tag)
            drvEtherIP_remove_callback(pvt->plc, pvt->tag, cbtype, rec);
        status = analyze_link(rec, cbtype, link, bits);
        if (status)
            return status;
        drvEtherIP_restart();
    }
    return 0;
}

/* device init: Start driver after all records registered  */
static bool driver_started = false;
static long init(int run)
{
    if (run==1 && driver_started==false)
    {
        drvEtherIP_restart();
        driver_started = true;
    }

    return 0;
}

/* Common initialization for all record types */
static long init_record(dbCommon *rec, EIPCallback cbtype,
                        const DBLINK *link, size_t bits)
{
    DevicePrivate *pvt = calloc (sizeof (DevicePrivate), 1);

    if (! pvt)
    {
        errlogPrintf("devEtherIP (%s): cannot allocate DPVT\n", rec->name);
        return S_dev_noMemory;
    }
    if (link->type != INST_IO)
    {
        errlogPrintf("devEtherIP (%s): INP is not INST_IO\n", rec->name);
        return S_db_badField;
    }
    scanIoInit(&pvt->ioscanpvt);
    rec->dpvt = pvt;
    
    return analyze_link(rec, cbtype, link, bits);
}

static long ai_init_record(aiRecord *rec)
{
    long status = init_record((dbCommon *)rec, scan_callback, &rec->inp, 0);
    /* Make sure record processing routine does not perform any conversion*/
    rec->linr = 0;
    return status;
}

static long bi_init_record(biRecord *rec)
{
    return init_record((dbCommon *)rec, scan_callback, &rec->inp, 1);
}

static long mbbi_init_record(mbbiRecord *rec)
{
    long status = init_record((dbCommon *)rec, scan_callback,
                              &rec->inp, rec->nobt);
    rec->shft = 0;
    return status;
}

static long mbbi_direct_init_record(mbbiDirectRecord *rec)
{
    long status = init_record((dbCommon *)rec, scan_callback,
                              &rec->inp, rec->nobt);
    rec->shft = 0;
    return status;
}

static long ao_init_record(aoRecord *rec)
{
    return init_record((dbCommon *)rec, check_ao_callback,
                       &rec->out, 0);
}

static long bo_init_record(boRecord *rec)
{
    return init_record((dbCommon *)rec, check_bo_callback,
                       &rec->out, 1 /* bits */);
}

static long mbbo_init_record(mbboRecord *rec)
{
    long status = init_record((dbCommon *)rec, check_mbbo_callback,
                              &rec->out, rec->nobt);
    rec->shft = 0;
    return status;
}

static long mbbo_direct_init_record(mbboDirectRecord *rec) 
{
    long status = init_record((dbCommon *)rec, check_mbbo_direct_callback,
                              &rec->out, rec->nobt);
    rec->shft = 0;
    return status;
}

static long ai_read(aiRecord *rec)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    long status;
    bool ok;
    CN_UDINT rval;

    rec->pact = TRUE;
    if (rec->tpro)
        dump_DevicePrivate((dbCommon *)rec);
    status = check_link((dbCommon *)rec, scan_callback, &rec->inp, 0);
    if (status)
    {
        recGblSetSevr(rec,READ_ALARM,INVALID_ALARM);
        return status;
    }

    ok = pvt && pvt->plc && pvt->tag && pvt->tag->scanlist;
    if (ok)
    {
        switch (pvt->special)
        {
            case SPCO_VALUE:
            case SPCO_READ_SINGLE_ELEMENT:
                if (semTake(pvt->tag->data_lock, sysClkRateGet()) != OK)
                {
                    if (rec->sevr != INVALID_ALARM)
                        errlogPrintf("devEtherIP (%s): no data_lock\n",
                                     rec->name);
                    ok = false;
                    break;
                }
                if (pvt->tag->valid_data_size > 0 &&
                    pvt->tag->elements > pvt->element)
                {
                    if (get_CIP_typecode(pvt->tag->data) == T_CIP_REAL)
                    {
                        ok = get_CIP_double(pvt->tag->data,
                                            pvt->element, &rec->val);
                        status = 2; /* don't convert */
                    }
                    else
                    {
                        ok = get_CIP_UDINT(pvt->tag->data,
                                           pvt->element, &rval);
                        rec->rval = rval;
                    }
                }
                else
                    ok = false;
                semGive (pvt->tag->data_lock);
                break;
            case SPCO_PLC_ERRORS:
                rec->val = (double) pvt->plc->plc_errors;
                status = 2;
                break;
            case SPCO_PLC_TASK_SLOW:
                rec->val = (double) pvt->plc->slow_scans;
                status = 2;
                break;
            case SPCO_LIST_ERRORS:
                rec->val = (double) pvt->tag->scanlist->list_errors;
                status = 2;
                break;
            case SPCO_LIST_TICKS:
                rec->val = (double) pvt->tag->scanlist->scan_ticktime;
                status = 2;
                break;
            case SPCO_LIST_SCAN_TIME:
                rec->val = (double) pvt->tag->scanlist->last_scan_ticks
                           / sysClkRateGet();
                status = 2;
                break;
            case SPCO_LIST_MIN_SCAN_TIME:
                rec->val = (double) pvt->tag->scanlist->min_scan_ticks
                            / sysClkRateGet();
                status = 2;
                break;
            case SPCO_LIST_MAX_SCAN_TIME:
                rec->val = (double) pvt->tag->scanlist->max_scan_ticks
                            / sysClkRateGet();
                status = 2;
                break;
            case SPCO_TAG_TRANSFER_TIME:
                rec->val = (double) pvt->tag->transfer_ticktime
                            / sysClkRateGet();
                status = 2;
                break;
            default:
                ok = false;
        }
    }
    
    if (ok)
        rec->udf = FALSE;
    else
        recGblSetSevr(rec,READ_ALARM,INVALID_ALARM);
    rec->pact = FALSE;
    
    return status;
}

static long bi_read(biRecord *rec)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    long status;
    bool ok;
    
    rec->pact = TRUE;
    if (rec->tpro)
        dump_DevicePrivate((dbCommon *)rec);
    status = check_link((dbCommon *)rec, scan_callback, &rec->inp, 1);
    if (status)
    {
        recGblSetSevr(rec, READ_ALARM, INVALID_ALARM);
        return status;
    }

    if (lock_data((dbCommon *)rec))
    {
        ok = get_bits((dbCommon *)rec, 1, &rec->rval);
        semGive(pvt->tag->data_lock);
    }
    else
        ok = false;
    
    if (ok)
        rec->udf = FALSE;
    else
        recGblSetSevr(rec, READ_ALARM, INVALID_ALARM);
    rec->pact = FALSE;
    
    return 0;
}

static long mbbi_read (mbbiRecord *rec)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    long status;
    bool ok;
    
    rec->pact = TRUE;
    if (rec->tpro)
        dump_DevicePrivate ((dbCommon *)rec);
    status = check_link ((dbCommon *)rec, scan_callback,
                         &rec->inp, rec->nobt);
    if (status)
    {
        recGblSetSevr(rec,READ_ALARM,INVALID_ALARM);
        return status;
    }

    if (lock_data ((dbCommon *)rec))
    {
        ok = get_bits ((dbCommon *)rec, rec->nobt, &rec->rval);
        semGive (pvt->tag->data_lock);
    }
    else
        ok = false;
    
    if (ok)
        rec->udf = FALSE;
    else
        recGblSetSevr(rec, READ_ALARM, INVALID_ALARM);
    rec->pact = FALSE;
    
    return 0;
}

static long mbbi_direct_read (mbbiDirectRecord *rec)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    long status;
    bool ok;

    rec->pact = TRUE;
    if (rec->tpro)
        dump_DevicePrivate ((dbCommon *)rec);
    status = check_link ((dbCommon *)rec, scan_callback,
                         &rec->inp, rec->nobt);
    if (status)
    {
        recGblSetSevr(rec, READ_ALARM, INVALID_ALARM);
        return status;
    }

    if (lock_data ((dbCommon *)rec))
    {
        ok = get_bits ((dbCommon *)rec, rec->nobt, &rec->rval);
        semGive (pvt->tag->data_lock);
    }
    else
        ok = false;
    
    if (ok)
        rec->udf = FALSE;
    else
        recGblSetSevr(rec, READ_ALARM, INVALID_ALARM);
    rec->pact = FALSE;
    
    return 0;
}

static long ao_write(aoRecord *rec)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    long          status;
    double        dbl;
    CN_UDINT      udint;
    bool          ok = true;

    rec->pact = TRUE;
    if (rec->tpro)
        dump_DevicePrivate((dbCommon *)rec);
    status = check_link((dbCommon *)rec, check_ao_callback, &rec->out, 0);
    if (status)
    {
        recGblSetSevr(rec, WRITE_ALARM, INVALID_ALARM);
        return status;
    }

    if (lock_data((dbCommon *)rec))
    {
        /* Check if record's (R)VAL is current */
        if (get_CIP_typecode(pvt->tag->data) == T_CIP_REAL)
        {
            if (get_CIP_double(pvt->tag->data, pvt->element, &dbl) &&
                rec->val != dbl)
            {
                EIP_printf(5, "rec '%s': write %g!\n",
                           rec->name, rec->val);
                ok = put_CIP_double(pvt->tag->data, pvt->element, rec->val);
                pvt->tag->do_write = true;
            }
        }
        else
        {
            if (get_CIP_UDINT(pvt->tag->data, pvt->element, &udint) &&
                rec->rval != udint)
            {
                if (rec->tpro)
                    printf("rec '%s': write %lu!\n",
                           rec->name, rec->rval);
                ok = put_CIP_UDINT(pvt->tag->data, pvt->element, rec->rval);
                pvt->tag->do_write = true;
            }
        }
        semGive(pvt->tag->data_lock);
    }
    else
        ok = false;
    
    if (ok)
        rec->udf = FALSE;
    else
        recGblSetSevr(rec, WRITE_ALARM, INVALID_ALARM);
    rec->pact = FALSE;
    
    return 0;
}

static long bo_write(boRecord *rec)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    long          status;
    unsigned long rval;
    bool          ok = true;

    rec->pact = TRUE;
    if (rec->tpro)
        dump_DevicePrivate((dbCommon *)rec);
    status = check_link((dbCommon *)rec, check_bo_callback, &rec->out, 1);
    if (status)
    {
        recGblSetSevr(rec, WRITE_ALARM, INVALID_ALARM);
        return status;
    }

    if (lock_data((dbCommon *)rec))
    {
        if (get_bits((dbCommon *)rec, 1, &rval) &&
            rec->rval != rval)
        {
            if (rec->tpro)
                printf ("rec '%s': write %lu\n",
                        rec->name, rec->rval);
            ok = put_bits((dbCommon *)rec, 1, rec->rval);
        }
        semGive(pvt->tag->data_lock);
    }
    else
        ok = false;
    
    if (ok)
        rec->udf = FALSE;
    else
        recGblSetSevr(rec, WRITE_ALARM, INVALID_ALARM);
    rec->pact = FALSE;
    
    return 0;
}

static long mbbo_write (mbboRecord *rec)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    long          status;
    unsigned long rval;
    bool          ok = true;

    rec->pact = TRUE;
    if (rec->tpro)
        dump_DevicePrivate ((dbCommon *)rec);
    status = check_link ((dbCommon *)rec, check_mbbo_callback,
                         &rec->out, rec->nobt);
    if (status)
    {
        recGblSetSevr(rec, WRITE_ALARM, INVALID_ALARM);
        return status;
    }

    if (lock_data ((dbCommon *)rec))
    {
        if (get_bits ((dbCommon *)rec, rec->nobt, &rval) &&
            rec->rval != rval)
        {
            if (rec->tpro)
                printf ("rec '%s': write %lu\n",
                        rec->name, rec->rval);
            ok = put_bits ((dbCommon *)rec, rec->nobt, rec->rval);
        }
        semGive (pvt->tag->data_lock);
    }
    else
        ok = false;
    
    if (ok)
        rec->udf = FALSE;
    else
        recGblSetSevr(rec, WRITE_ALARM, INVALID_ALARM);
    rec->pact = FALSE;
    
    return 0;
}

static long mbbo_direct_write (mbboDirectRecord *rec)
{
    DevicePrivate *pvt = (DevicePrivate *)rec->dpvt;
    long          status;
    unsigned long rval;
    bool          ok = true;

    rec->pact = TRUE;
    if (rec->tpro)
        dump_DevicePrivate ((dbCommon *)rec);
    status = check_link ((dbCommon *)rec, check_mbbo_direct_callback,
                         &rec->out, rec->nobt);
    if (status)
    {
        recGblSetSevr(rec, WRITE_ALARM, INVALID_ALARM);
        return status;
    }

    if (lock_data ((dbCommon *)rec))
    {
        if (get_bits ((dbCommon *)rec, rec->nobt, &rval) &&
            rec->rval != rval)
        {
            if (rec->tpro)
                printf ("rec '%s': write %lu\n",
                        rec->name, rec->rval);
            ok = put_bits ((dbCommon *)rec, rec->nobt, rec->rval);
        }
        semGive (pvt->tag->data_lock);
    }
    else
        ok = false;
    
    if (ok)
        rec->udf = FALSE;
    else
        recGblSetSevr(rec, WRITE_ALARM, INVALID_ALARM);
    rec->pact = FALSE;
    
    return 0;
}

/* Create the device support entry tables */
typedef struct
{
    long        number;
    DEVSUPFUN   report;
    DEVSUPFUN   init;
    DEVSUPFUN   init_record;
    DEVSUPFUN   get_ioint_info;
    DEVSUPFUN   read_write;
    DEVSUPFUN   special_linconv;
} DSET;

DSET devAiEtherIP =
{
    6,
    NULL,
    init,
    ai_init_record,
    get_ioint_info,
    ai_read,
    NULL
};

DSET devBiEtherIP =
{
    6,
    NULL,
    init,
    bi_init_record,
    get_ioint_info,
    bi_read,
    NULL
};

DSET devMbbiEtherIP =
{
    6,
    NULL,
    init,
    mbbi_init_record,
    get_ioint_info,
    mbbi_read,
    NULL
};

DSET devMbbiDirectEtherIP =
{
    6,
    NULL,
    init,
    mbbi_direct_init_record,
    get_ioint_info,
    mbbi_direct_read,
    NULL
};

DSET devAoEtherIP =
{
    6,
    NULL,
    init,
    ao_init_record,
    NULL,
    ao_write,
    NULL
};

DSET devBoEtherIP =
{
    6,
    NULL,
    init,
    bo_init_record,
    NULL,
    bo_write,
    NULL
};

DSET devMbboEtherIP =
{
    6,
    NULL,
    init,
    mbbo_init_record,
    NULL,
    mbbo_write,
    NULL
};

DSET devMbboDirectEtherIP =
{
    6,
    NULL,
    init,
    mbbo_direct_init_record,
    NULL,
    mbbo_direct_write,
    NULL
};

/* EOF devEtherIP.c */
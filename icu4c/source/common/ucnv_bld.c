/*
 ********************************************************************
 * COPYRIGHT: 
 * Copyright (c) 1996-2003, International Business Machines Corporation and
 * others. All Rights Reserved.
 ********************************************************************
 *
 *  uconv_bld.c:
 *
 *  Defines functions that are used in the creation/initialization/deletion
 *  of converters and related structures.
 *  uses uconv_io.h routines to access disk information
 *  is used by ucnv.h to implement public API create/delete/flushCache routines
 * Modification History:
 * 
 *   Date        Name        Description
 * 
 *   06/20/2000  helena      OS/400 port changes; mostly typecast.
 *   06/29/2000  helena      Major rewrite of the callback interface.
*/


#include "unicode/udata.h"
#include "unicode/ucnv.h"
#include "unicode/ucnv_err.h"
#include "unicode/uloc.h"
#include "ucnv_io.h"
#include "ucnv_bld.h"
#include "ucnvmbcs.h"
#include "ucnv_ext.h"
#include "ucnv_cnv.h"
#include "ucnv_imp.h"
#include "uhash.h"
#include "umutex.h"
#include "cstring.h"
#include "cmemory.h"
#include "ucln_cmn.h"
#include "ustr_imp.h"



#if 0
#include <stdio.h>
extern void UCNV_DEBUG_LOG(char *what, char *who, void *p, int l);
#define UCNV_DEBUG_LOG(x,y,z) UCNV_DEBUG_LOG(x,y,z,__LINE__)
#else
# define UCNV_DEBUG_LOG(x,y,z)
#endif

static const UConverterSharedData * const
converterData[UCNV_NUMBER_OF_SUPPORTED_CONVERTER_TYPES]={
    NULL, NULL,

#if UCONFIG_NO_LEGACY_CONVERSION
    NULL,
#else
    &_MBCSData,
#endif

    &_Latin1Data,
    &_UTF8Data, &_UTF16BEData, &_UTF16LEData, &_UTF32BEData, &_UTF32LEData,
    NULL,

#if UCONFIG_NO_LEGACY_CONVERSION
    NULL,
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL,
#else
    &_ISO2022Data,
    &_LMBCSData1,&_LMBCSData2, &_LMBCSData3, &_LMBCSData4, &_LMBCSData5, &_LMBCSData6,
    &_LMBCSData8,&_LMBCSData11,&_LMBCSData16,&_LMBCSData17,&_LMBCSData18,&_LMBCSData19,
    &_HZData,
#endif

    &_SCSUData,

#if UCONFIG_NO_LEGACY_CONVERSION
    NULL,
#else
    &_ISCIIData,
#endif

    &_ASCIIData,
    &_UTF7Data, &_Bocu1Data, &_UTF16Data, &_UTF32Data, &_CESU8Data, &_IMAPData
};

/* Please keep this in binary sorted order for getAlgorithmicTypeFromName.
   Also the name should be in lower case and all spaces, dashes and underscores
   removed
*/
static struct {
  const char *name;
  const UConverterType type;
} const cnvNameType[] = {
  { "bocu1", UCNV_BOCU1 },
  { "cesu8", UCNV_CESU8 },
#if !UCONFIG_NO_LEGACY_CONVERSION
  { "hz",UCNV_HZ },
#endif
  { "imapmailboxname", UCNV_IMAP_MAILBOX },
#if !UCONFIG_NO_LEGACY_CONVERSION
  { "iscii", UCNV_ISCII },
  { "iso2022", UCNV_ISO_2022 },
#endif
  { "iso88591", UCNV_LATIN_1 },
#if !UCONFIG_NO_LEGACY_CONVERSION
  { "lmbcs1", UCNV_LMBCS_1 },
  { "lmbcs11",UCNV_LMBCS_11 },
  { "lmbcs16",UCNV_LMBCS_16 },
  { "lmbcs17",UCNV_LMBCS_17 },
  { "lmbcs18",UCNV_LMBCS_18 },
  { "lmbcs19",UCNV_LMBCS_19 },
  { "lmbcs2", UCNV_LMBCS_2 },
  { "lmbcs3", UCNV_LMBCS_3 },
  { "lmbcs4", UCNV_LMBCS_4 },
  { "lmbcs5", UCNV_LMBCS_5 },
  { "lmbcs6", UCNV_LMBCS_6 },
  { "lmbcs8", UCNV_LMBCS_8 },
#endif
  { "scsu", UCNV_SCSU },
  { "usascii", UCNV_US_ASCII },
  { "utf16", UCNV_UTF16 },
  { "utf16be", UCNV_UTF16_BigEndian },
  { "utf16le", UCNV_UTF16_LittleEndian },
#if U_IS_BIG_ENDIAN
  { "utf16oppositeendian", UCNV_UTF16_LittleEndian },
  { "utf16platformendian", UCNV_UTF16_BigEndian },
#else
  { "utf16oppositeendian", UCNV_UTF16_BigEndian},
  { "utf16platformendian", UCNV_UTF16_LittleEndian },
#endif
  { "utf32", UCNV_UTF32 },
  { "utf32be", UCNV_UTF32_BigEndian },
  { "utf32le", UCNV_UTF32_LittleEndian },
#if U_IS_BIG_ENDIAN
  { "utf32oppositeendian", UCNV_UTF32_LittleEndian },
  { "utf32platformendian", UCNV_UTF32_BigEndian },
#else
  { "utf32oppositeendian", UCNV_UTF32_BigEndian },
  { "utf32platformendian", UCNV_UTF32_LittleEndian },
#endif
  { "utf7", UCNV_UTF7 },
  { "utf8", UCNV_UTF8 }
};


/*initializes some global variables */
static UHashtable *SHARED_DATA_HASHTABLE = NULL;
static UMTX        cnvCacheMutex = NULL;  /* Mutex for synchronizing cnv cache access. */
                                          /*  Note:  the global mutex is used for      */
                                          /*         reference count updates.          */


static const char DATA_TYPE[] = "cnv";

/* ucnv_cleanup - delete all storage held by the converter cache, except any in use    */
/*                by open converters.                                                  */
/*                Not thread safe.                                                     */
/*                Not supported  API.  Marked U_CAPI only for use by test programs.    */
U_CFUNC UBool U_EXPORT2 ucnv_cleanup(void) {
    if (SHARED_DATA_HASHTABLE != NULL) {
        ucnv_flushCache();
        if (SHARED_DATA_HASHTABLE != NULL && uhash_count(SHARED_DATA_HASHTABLE) == 0) {
            uhash_close(SHARED_DATA_HASHTABLE);
            SHARED_DATA_HASHTABLE = NULL;
        }
    }

    umtx_destroy(&cnvCacheMutex);           /* Don't worry about destroying the mutex even  */
                                            /*  if the hash table still exists.  The mutex  */
                                            /*  will lazily re-init  itself if needed.      */
    return (SHARED_DATA_HASHTABLE == NULL);
}

U_CFUNC void ucnv_init(UErrorCode *status) {
    umtx_init(&cnvCacheMutex);
}

static UBool U_CALLCONV
isCnvAcceptable(void *context,
             const char *type, const char *name,
             const UDataInfo *pInfo) {
    return (UBool)(
        pInfo->size>=20 &&
        pInfo->isBigEndian==U_IS_BIG_ENDIAN &&
        pInfo->charsetFamily==U_CHARSET_FAMILY &&
        pInfo->sizeofUChar==U_SIZEOF_UCHAR &&
        pInfo->dataFormat[0]==0x63 &&   /* dataFormat="cnvt" */
        pInfo->dataFormat[1]==0x6e &&
        pInfo->dataFormat[2]==0x76 &&
        pInfo->dataFormat[3]==0x74 &&
        pInfo->formatVersion[0]==6);  /* Everything will be version 6 */
}

/**
 * Un flatten shared data from a UDATA..
 */
static UConverterSharedData*
ucnv_data_unFlattenClone(UDataMemory *pData, UErrorCode *status)
{
    /* UDataInfo info; -- necessary only if some converters have different formatVersion */
    const uint8_t *raw = (const uint8_t *)udata_getMemory(pData);
    const UConverterStaticData *source = (const UConverterStaticData *) raw;
    UConverterSharedData *data;
    UConverterType type = (UConverterType)source->conversionType;

    if(U_FAILURE(*status))
        return NULL;

    if( (uint16_t)type >= UCNV_NUMBER_OF_SUPPORTED_CONVERTER_TYPES ||
        converterData[type] == NULL ||
        converterData[type]->referenceCounter != 1 ||
        source->structSize != sizeof(UConverterStaticData))
    {
        *status = U_INVALID_TABLE_FORMAT;
        return NULL;
    }

    data = (UConverterSharedData *)uprv_malloc(sizeof(UConverterSharedData));
    if(data == NULL) {
        *status = U_MEMORY_ALLOCATION_ERROR;
        return NULL;
    }

    /* copy initial values from the static structure for this type */
    uprv_memcpy(data, converterData[type], sizeof(UConverterSharedData));

    /*
     * It would be much more efficient if the table were a direct member, not a pointer.
     * However, that would add to the size of all UConverterSharedData objects
     * even if they do not use this table (especially algorithmic ones).
     * If this changes, then the static templates from converterData[type]
     * need more entries.
     */
    data->table = (UConverterTable *)uprv_malloc(sizeof(UConverterTable));
    if(data->table == NULL) {
        uprv_free(data);
        *status = U_MEMORY_ALLOCATION_ERROR;
        return NULL;
    }
    uprv_memset(data->table, 0, sizeof(UConverterTable));
    
    data->staticData = source;
    
    data->sharedDataCached = FALSE;

    /* fill in fields from the loaded data */
    data->dataMemory = (void*)pData; /* for future use */

    if(data->impl->load != NULL) {
        data->impl->load(data, raw + source->structSize, status);
        if(U_FAILURE(*status)) {
            uprv_free(data->table);
            uprv_free(data);
            return NULL;
        }
    }
    return data;
}

/*Takes an alias name gets an actual converter file name
 *goes to disk and opens it.
 *allocates the memory and returns a new UConverter object
 */
static UConverterSharedData *createConverterFromFile(const char* pkg, const char *fileName, UErrorCode * err)
{
    UDataMemory *data;
    UConverterSharedData *sharedData;

    if (err == NULL || U_FAILURE (*err)) {
        return NULL;
    }

    data = udata_openChoice(pkg, DATA_TYPE, fileName, isCnvAcceptable, NULL, err);
    if(U_FAILURE(*err))
    {
        return NULL;
    }

    sharedData = ucnv_data_unFlattenClone(data, err);
    if(U_FAILURE(*err))
    {
        udata_close(data);
        return NULL;
    }

    return sharedData;
}

int32_t 
ucnv_copyPlatformString(char *platformString, UConverterPlatform pltfrm)
{
    switch (pltfrm)
    {
    case UCNV_IBM:
        uprv_strcpy(platformString, "ibm-");
        return 4;
    case UCNV_UNKNOWN:
        break;
    }

    /* default to empty string */
    *platformString = 0;
    return 0;
}

/*returns a converter type from a string
 */
static const UConverterSharedData *
getAlgorithmicTypeFromName(const char *realName)
{
    uint32_t mid, start, limit;
	uint32_t lastMid;
    int result;
    char strippedName[UCNV_MAX_CONVERTER_NAME_LENGTH];

    /* Lower case and remove ignoreable characters. */
    ucnv_io_stripForCompare(strippedName, realName);

    /* do a binary search for the alias */
    start = 0;
    limit = sizeof(cnvNameType)/sizeof(cnvNameType[0]);
    mid = limit;
	lastMid = UINT32_MAX;

    for (;;) {
        mid = (uint32_t)((start + limit) / 2);
		if (lastMid == mid) {	/* Have we moved? */
			break;	/* We haven't moved, and it wasn't found. */
		}
		lastMid = mid;
        result = uprv_strcmp(strippedName, cnvNameType[mid].name);

        if (result < 0) {
            limit = mid;
        } else if (result > 0) {
            start = mid;
        } else {
            return converterData[cnvNameType[mid].type];
        }
    }

    return NULL;
}

/* Puts the shared data in the static hashtable SHARED_DATA_HASHTABLE */
/*   Will always be called with the cnvCacheMutex alrady being held   */
/*     by the calling function.                                       */
/* Stores the shared data in the SHARED_DATA_HASHTABLE
 * @param data The shared data
 */
static void
ucnv_shareConverterData(UConverterSharedData * data)
{
    UErrorCode err = U_ZERO_ERROR;
    /*Lazy evaluates the Hashtable itself */
    /*void *sanity = NULL;*/

    if (SHARED_DATA_HASHTABLE == NULL)
    {
        SHARED_DATA_HASHTABLE = uhash_openSize(uhash_hashChars, uhash_compareChars,
                            ucnv_io_countAvailableAliases(&err),
                            &err);
        if (U_FAILURE(err)) 
            return;
    }

    /* ### check to see if the element is not already there! */

    /*
    sanity =   ucnv_getSharedConverterData (data->staticData->name);
    if(sanity != NULL)
    {
    UCNV_DEBUG_LOG("put:overwrite!",data->staticData->name,sanity);
    }
    UCNV_DEBUG_LOG("put:chk",data->staticData->name,sanity);
    */
    
    /* Mark it shared */
    data->sharedDataCached = TRUE;

    uhash_put(SHARED_DATA_HASHTABLE,
            (void*) data->staticData->name, /* Okay to cast away const as long as
            keyDeleter == NULL */
            data,
            &err);
    UCNV_DEBUG_LOG("put", data->staticData->name,data);

}

/*  Look up a converter name in the shared data cache.                    */
/*    cnvCacheMutex must be held by the caller to protect the hash table. */
/* gets the shared data from the SHARED_DATA_HASHTABLE (might return NULL if it isn't there)
 * @param name The name of the shared data
 * @return the shared data from the SHARED_DATA_HASHTABLE
 */
static UConverterSharedData *
ucnv_getSharedConverterData(const char *name)
{
    /*special case when no Table has yet been created we return NULL */
    if (SHARED_DATA_HASHTABLE == NULL)
    {
        return NULL;
    }
    else
    {
        UConverterSharedData *rc;

        rc = (UConverterSharedData*)uhash_get(SHARED_DATA_HASHTABLE, name);
        UCNV_DEBUG_LOG("get",name,rc);
        return rc;
    }
}

/*frees the string of memory blocks associates with a sharedConverter
 *if and only if the referenceCounter == 0
 */
/* Deletes (frees) the Shared data it's passed. first it checks the referenceCounter to
 * see if anyone is using it, if not it frees all the memory stemming from sharedConverterData and
 * returns TRUE,
 * otherwise returns FALSE
 * @param sharedConverterData The shared data
 * @return if not it frees all the memory stemming from sharedConverterData and
 * returns TRUE, otherwise returns FALSE
 */
static UBool
ucnv_deleteSharedConverterData(UConverterSharedData * deadSharedData)
{
    if (deadSharedData->referenceCounter > 0)
        return FALSE;

    if (deadSharedData->impl->unload != NULL) {
        deadSharedData->impl->unload(deadSharedData);
    }
    
    if(deadSharedData->dataMemory != NULL)
    {
        UDataMemory *data = (UDataMemory*)deadSharedData->dataMemory;
        udata_close(data);
    }

    if(deadSharedData->table != NULL)
    {
        uprv_free(deadSharedData->table);
    }

#if 0
    /* if the static data is actually owned by the shared data */
    /* enable if we ever have this situation. */
    if(deadSharedData->staticDataOwned == TRUE) /* see ucnv_bld.h */
    {
        uprv_free((void*)deadSharedData->staticData);
    }
#endif

#if 0
    /* Zap it ! */
    uprv_memset(deadSharedData->0, sizeof(*deadSharedData));
#endif

    uprv_free(deadSharedData);
    
    return TRUE;
}

void
ucnv_unloadSharedDataIfReady(UConverterSharedData *sharedData)
{
    umtx_lock(&cnvCacheMutex);
    /*
    Double checking doesn't work on some platforms.
    Don't check referenceCounter outside of a mutex block.
    */
    if (sharedData->referenceCounter != ~0) {
        if (sharedData->referenceCounter > 0) {
            sharedData->referenceCounter--;
        }
        
        if((sharedData->referenceCounter <= 0)&&(sharedData->sharedDataCached == FALSE)) {
            ucnv_deleteSharedConverterData(sharedData);
        }
    }
    umtx_unlock(&cnvCacheMutex);
}

void
ucnv_incrementRefCount(UConverterSharedData *sharedData)
{
    umtx_lock(&cnvCacheMutex);
    /*
    Double checking doesn't work on some platforms.
    Don't check referenceCounter outside of a mutex block.
    */
    if (sharedData->referenceCounter != ~0) {
        sharedData->referenceCounter++;
    }
    umtx_unlock(&cnvCacheMutex);
}

static void
parseConverterOptions(const char *inName,
                      char *cnvName,
                      char *locale,
                      uint32_t *pFlags,
                      UErrorCode *err)
{
    char c;
    int32_t len = 0;

    /* copy the converter name itself to cnvName */
    while((c=*inName)!=0 && c!=UCNV_OPTION_SEP_CHAR) {
        if (++len>=UCNV_MAX_CONVERTER_NAME_LENGTH) {
            *err = U_ILLEGAL_ARGUMENT_ERROR;    /* bad name */
            *cnvName=0;
            return;
        }
        *cnvName++=c;
        inName++;
    }
    *cnvName=0;

    /* parse options. No more name copying should occur. */
    while((c=*inName)!=0) {
        if(c==UCNV_OPTION_SEP_CHAR) {
            ++inName;
        }

        /* inName is behind an option separator */
        if(uprv_strncmp(inName, "locale=", 7)==0) {
            /* do not modify locale itself in case we have multiple locale options */
            char *dest=locale;

            /* copy the locale option value */
            inName+=7;
            len=0;
            while((c=*inName)!=0 && c!=UCNV_OPTION_SEP_CHAR) {
                ++inName;

                if(++len>=ULOC_FULLNAME_CAPACITY) {
                    *err=U_ILLEGAL_ARGUMENT_ERROR;    /* bad name */
                    *locale=0;
                    return;
                }

                *dest++=c;
            }
            *dest=0;
        } else if(uprv_strncmp(inName, "version=", 8)==0) {
            /* copy the version option value into bits 3..0 of *pFlags */
            inName+=8;
            c=*inName;
            if(c==0) {
                *pFlags&=~UCNV_OPTION_VERSION;
                return;
            } else if((uint8_t)(c-'0')<10) {
                *pFlags=(*pFlags&~UCNV_OPTION_VERSION)|(uint32_t)(c-'0');
                ++inName;
            }
        } else if(uprv_strncmp(inName, "swaplfnl", 8)==0) {
            inName+=8;
            *pFlags|=UCNV_OPTION_SWAP_LFNL;
        /* add processing for new options here with another } else if(uprv_strncmp(inName, "option-name=", XX)==0) { */
        } else {
            /* ignore any other options until we define some */
            while(((c = *inName++) != 0) && (c != UCNV_OPTION_SEP_CHAR)) {
            }
            if(c==0) {
                return;
            }
        }
    }
}

/*Logic determines if the converter is Algorithmic AND/OR cached
 *depending on that:
 * -we either go to get data from disk and cache it (Data=TRUE, Cached=False)
 * -Get it from a Hashtable (Data=X, Cached=TRUE)
 * -Call dataConverter initializer (Data=TRUE, Cached=TRUE)
 * -Call AlgorithmicConverter initializer (Data=FALSE, Cached=TRUE)
 */
UConverter *
ucnv_createConverter(UConverter *myUConverter, const char *converterName, UErrorCode * err)
{
    char cnvName[UCNV_MAX_CONVERTER_NAME_LENGTH], locale[ULOC_FULLNAME_CAPACITY];
    const char *realName;
    UConverterSharedData *mySharedConverterData = NULL;
    UErrorCode internalErrorCode = U_ZERO_ERROR;
    uint32_t options = 0;
    if (U_FAILURE (*err))
        return NULL;

    locale[0] = 0;

    /* In case "name" is NULL we want to open the default converter. */
    if (converterName == NULL) {
        realName = ucnv_io_getDefaultConverterName();
        if (realName == NULL) {
            *err = U_MISSING_RESOURCE_ERROR;
            return NULL;
        }
        /* the default converter name is already canonical */
    } else {
        /* separate the converter name from the options */
        parseConverterOptions(converterName, cnvName, locale, &options, err);
        if (U_FAILURE(*err)) {
            /* Very bad name used. */
            return NULL;
        }

        /* get the canonical converter name */
        realName = ucnv_io_getConverterName(cnvName, &internalErrorCode);
        if (U_FAILURE(internalErrorCode) || realName == NULL) {
            /*
            * set the input name in case the converter was added
            * without updating the alias table, or when there is no alias table
            */
            realName = cnvName;
        }
    }

    /* separate the converter name from the options */
    if(realName != cnvName) {
        parseConverterOptions(realName, cnvName, locale, &options, err);
        realName = cnvName;
    }
    
    /* get the shared data for an algorithmic converter, if it is one */
    mySharedConverterData = (UConverterSharedData *)getAlgorithmicTypeFromName(realName);
    if (mySharedConverterData == NULL)
    {
        /* it is a data-based converter, get its shared data.               */
        /* Hold the cnvCacheMutex through the whole process of checking the */
        /*   converter data cache, and adding new entries to the cache      */
        /*   to prevent other threads from modifying the cache during the   */
        /*   process.                                                       */
        umtx_lock(&cnvCacheMutex);
        mySharedConverterData = ucnv_getSharedConverterData(realName);
        if (mySharedConverterData == NULL)
        {
            /*Not cached, we need to stream it in from file */
            mySharedConverterData = createConverterFromFile(NULL, realName, err);
            if (U_FAILURE (*err) || (mySharedConverterData == NULL))
            {
                umtx_unlock(&cnvCacheMutex);
                return NULL;
            }
            else
            {
                /* share it with other library clients */
                ucnv_shareConverterData(mySharedConverterData);
            }
        }
        else
        {
            /* The data for this converter was already in the cache.            */
            /* Update the reference counter on the shared data: one more client */
            mySharedConverterData->referenceCounter++;
        }
        umtx_unlock(&cnvCacheMutex);
    }

    myUConverter = ucnv_createConverterFromSharedData(myUConverter, mySharedConverterData, realName, locale, options, err);

    if (U_FAILURE(*err))
    {
        /*
        Checking whether it's an algorithic converter is okay
        in multithreaded applications because the value never changes.
        Don't check referenceCounter for any other value.
        */
        if (mySharedConverterData->referenceCounter != ~0) {
            umtx_lock(&cnvCacheMutex);
            --mySharedConverterData->referenceCounter;
            umtx_unlock(&cnvCacheMutex);
        }
        return NULL;
    }

    return myUConverter;
}

UConverter *
ucnv_createAlgorithmicConverter(UConverter *myUConverter,
                                UConverterType type,
                                const char *locale, uint32_t options,
                                UErrorCode *err) {
    const UConverterSharedData *sharedData;
    UBool isAlgorithmicConverter;

    if(type<0 || UCNV_NUMBER_OF_SUPPORTED_CONVERTER_TYPES<=type) {
        *err = U_ILLEGAL_ARGUMENT_ERROR;
        return NULL;
    }

    sharedData = converterData[type];
    umtx_lock(&cnvCacheMutex);
    isAlgorithmicConverter = (UBool)(sharedData == NULL || sharedData->referenceCounter != ~0);
    umtx_unlock(&cnvCacheMutex);
    if (isAlgorithmicConverter) {
        /* not a valid type, or not an algorithmic converter */
        *err = U_ILLEGAL_ARGUMENT_ERROR;
        return NULL;
    }

    return ucnv_createConverterFromSharedData(myUConverter, (UConverterSharedData *)sharedData, "",
                locale != NULL ? locale : "", options, err);
}

UConverter*
ucnv_createConverterFromPackage(const char *packageName, const char *converterName, UErrorCode * err)
{
    char cnvName[UCNV_MAX_CONVERTER_NAME_LENGTH], locale[ULOC_FULLNAME_CAPACITY];
    uint32_t options=0;
    UConverter *myUConverter;
    UConverterSharedData *mySharedConverterData = NULL;

    if(U_FAILURE(*err)) {
        return NULL; 
    }

    /* first, get the options out of the convertername string */
    parseConverterOptions(converterName, cnvName, locale, &options, err);
    if (U_FAILURE(*err)) {
        /* Very bad name used. */
        return NULL;
    }
    
    /* open the data, unflatten the shared structure */
    mySharedConverterData = createConverterFromFile(packageName, cnvName, err);
    
    if (U_FAILURE(*err)) {
        return NULL; 
    }

    /* create the actual converter */
    myUConverter = ucnv_createConverterFromSharedData(NULL, mySharedConverterData, cnvName, locale, options, err);
    
    if (U_FAILURE(*err)) {
        ucnv_close(myUConverter);
        return NULL; 
    }
    
    return myUConverter;
}


UConverter*
ucnv_createConverterFromSharedData(UConverter *myUConverter,
                                   UConverterSharedData *mySharedConverterData,
                                   const char *realName, const char *locale, uint32_t options,
                                   UErrorCode *err)
{
    UBool isCopyLocal;

    if(myUConverter == NULL)
    {
        myUConverter = (UConverter *) uprv_malloc (sizeof (UConverter));
        if(myUConverter == NULL)
        {
            *err = U_MEMORY_ALLOCATION_ERROR;
            return NULL;
        }
        isCopyLocal = FALSE;
    } else {
        isCopyLocal = TRUE;
    }

    /* initialize the converter */
    uprv_memset(myUConverter, 0, sizeof(UConverter));
    myUConverter->isCopyLocal = isCopyLocal;
    myUConverter->isExtraLocal = FALSE;
    myUConverter->sharedData = mySharedConverterData;
    myUConverter->options = options;
    myUConverter->fromCharErrorBehaviour = (UConverterToUCallback) UCNV_TO_U_CALLBACK_SUBSTITUTE;
    myUConverter->fromUCharErrorBehaviour = (UConverterFromUCallback) UCNV_FROM_U_CALLBACK_SUBSTITUTE;
    myUConverter->toUnicodeStatus = myUConverter->sharedData->toUnicodeStatus;
    myUConverter->maxBytesPerUChar = myUConverter->sharedData->staticData->maxBytesPerChar;
    myUConverter->subChar1 = myUConverter->sharedData->staticData->subChar1;
    myUConverter->subCharLen = myUConverter->sharedData->staticData->subCharLen;
    uprv_memcpy (myUConverter->subChar, myUConverter->sharedData->staticData->subChar, myUConverter->subCharLen);
    myUConverter->preFromUFirstCP = U_SENTINEL;

    if(myUConverter != NULL && myUConverter->sharedData->impl->open != NULL) {
        myUConverter->sharedData->impl->open(myUConverter, realName, locale,options, err);
        if(U_FAILURE(*err)) {
            ucnv_close(myUConverter);
            return NULL;
        }
    }

    return myUConverter;
}

/*Frees all shared immutable objects that aren't referred to (reference count = 0)
 */
U_CAPI int32_t U_EXPORT2
ucnv_flushCache ()
{
    UConverterSharedData *mySharedData = NULL;
    int32_t pos = -1;
    int32_t tableDeletedNum = 0;
    const UHashElement *e;
    UErrorCode status = U_ILLEGAL_ARGUMENT_ERROR;

    /* Close the default converter without creating a new one so that everything will be flushed. */
    ucnv_close(u_getDefaultConverter(&status));

    /*if shared data hasn't even been lazy evaluated yet
    * return 0
    */
    if (SHARED_DATA_HASHTABLE == NULL)
        return 0;

    /*creates an enumeration to iterate through every element in the
    * table
    *
    * Synchronization:  holding cnvCacheMutex will prevent any other thread from
    *                   accessing or modifying the hash table during the iteration.
    *                   The reference count of an entry may be decremented by
    *                   ucnv_close while the iteration is in process, but this is
    *                   benign.  It can't be incremented (in ucnv_createConverter())
    *                   because the sequence of looking up in the cache + incrementing
    *                   is protected by cnvCacheMutex.
    */
    umtx_lock(&cnvCacheMutex);
    while ((e = uhash_nextElement (SHARED_DATA_HASHTABLE, &pos)) != NULL)
    {
        mySharedData = (UConverterSharedData *) e->value.pointer;
        /*deletes only if reference counter == 0 */
        if (mySharedData->referenceCounter == 0)
        {
            tableDeletedNum++;
            
            UCNV_DEBUG_LOG("del",mySharedData->staticData->name,mySharedData);
            
            uhash_removeElement(SHARED_DATA_HASHTABLE, e);
            mySharedData->sharedDataCached = FALSE;
            ucnv_deleteSharedConverterData (mySharedData);
        }
    }
    umtx_unlock(&cnvCacheMutex);

    ucnv_io_flushAvailableConverterCache();

    return tableDeletedNum;
}

/* data swapping ------------------------------------------------------------ */

/* most of this might belong more properly into ucnvmbcs.c, but that is so large */

#if !UCONFIG_NO_LEGACY_CONVERSION

U_CAPI int32_t U_EXPORT2
ucnv_swap(const UDataSwapper *ds,
          const void *inData, int32_t length, void *outData,
          UErrorCode *pErrorCode) {
    const UDataInfo *pInfo;
    int32_t headerSize;

    const uint8_t *inBytes;
    uint8_t *outBytes;

    uint32_t offset, count, staticDataSize;
    int32_t size;

    const UConverterStaticData *inStaticData;
    UConverterStaticData *outStaticData;

    const _MBCSHeader *inMBCSHeader;
    _MBCSHeader *outMBCSHeader;
    _MBCSHeader mbcsHeader;
    uint8_t outputType;

    const int32_t *inExtIndexes;
    int32_t extOffset;

    /* udata_swapDataHeader checks the arguments */
    headerSize=udata_swapDataHeader(ds, inData, length, outData, pErrorCode);
    if(pErrorCode==NULL || U_FAILURE(*pErrorCode)) {
        return 0;
    }

    /* check data format and format version */
    pInfo=(const UDataInfo *)((const char *)inData+4);
    if(!(
        pInfo->dataFormat[0]==0x63 &&   /* dataFormat="cnvt" */
        pInfo->dataFormat[1]==0x6e &&
        pInfo->dataFormat[2]==0x76 &&
        pInfo->dataFormat[3]==0x74 &&
        pInfo->formatVersion[0]==6 &&
        pInfo->formatVersion[1]>=2
    )) {
        udata_printError(ds, "ucnv_swap(): data format %02x.%02x.%02x.%02x (format version %02x.%02x) is not recognized as an ICU .cnv conversion table\n",
                         pInfo->dataFormat[0], pInfo->dataFormat[1],
                         pInfo->dataFormat[2], pInfo->dataFormat[3],
                         pInfo->formatVersion[0], pInfo->formatVersion[1]);
        *pErrorCode=U_UNSUPPORTED_ERROR;
        return 0;
    }

    inBytes=(const uint8_t *)inData+headerSize;
    outBytes=(uint8_t *)outData+headerSize;

    /* read the initial UConverterStaticData structure after the UDataInfo header */
    inStaticData=(const UConverterStaticData *)inBytes;
    outStaticData=(UConverterStaticData *)outBytes;

    if(length<0) {
        staticDataSize=ds->readUInt32(inStaticData->structSize);
    } else {
        length-=headerSize;
        if( length<sizeof(UConverterStaticData) ||
            (uint32_t)length<(staticDataSize=ds->readUInt32(inStaticData->structSize))
        ) {
            udata_printError(ds, "ucnv_swap(): too few bytes (%d after header) for an ICU .cnv conversion table\n",
                             length);
            *pErrorCode=U_INDEX_OUTOFBOUNDS_ERROR;
            return 0;
        }
    }

    if(length>=0) {
        /* swap the static data */
        if(inStaticData!=outStaticData) {
            uprv_memcpy(outStaticData, inStaticData, staticDataSize);
        }

        ds->swapArray32(ds, &inStaticData->structSize, 4,
                           &outStaticData->structSize, pErrorCode);
        ds->swapArray32(ds, &inStaticData->codepage, 4,
                           &outStaticData->codepage, pErrorCode);

        ds->swapInvChars(ds, inStaticData->name, uprv_strlen(inStaticData->name),
                            outStaticData->name, pErrorCode);
        if(U_FAILURE(*pErrorCode)) {
            udata_printError(ds, "ucnv_swap(): error swapping converter name - %s\n",
                             u_errorName(*pErrorCode));
            return 0;
        }
    }

    inBytes+=staticDataSize;
    outBytes+=staticDataSize;
    if(length>=0) {
        length-=(int32_t)staticDataSize;
    }

    /* check for supported conversionType values */
    if(inStaticData->conversionType==UCNV_MBCS) {
        /* swap MBCS data */
        inMBCSHeader=(const _MBCSHeader *)inBytes;
        outMBCSHeader=(_MBCSHeader *)outBytes;

        if(!(inMBCSHeader->version[0]==4 || inMBCSHeader->version[1]>=1)) {
            udata_printError(ds, "ucnv_swap(): unsupported _MBCSHeader.version %d.%d\n",
                             inMBCSHeader->version[0], inMBCSHeader->version[1]);
            *pErrorCode=U_UNSUPPORTED_ERROR;
            return 0;
        }

        uprv_memcpy(mbcsHeader.version, inMBCSHeader->version, 4);
        mbcsHeader.countStates=         ds->readUInt32(inMBCSHeader->countStates);
        mbcsHeader.countToUFallbacks=   ds->readUInt32(inMBCSHeader->countToUFallbacks);
        mbcsHeader.offsetToUCodeUnits=  ds->readUInt32(inMBCSHeader->offsetToUCodeUnits);
        mbcsHeader.offsetFromUTable=    ds->readUInt32(inMBCSHeader->offsetFromUTable);
        mbcsHeader.offsetFromUBytes=    ds->readUInt32(inMBCSHeader->offsetFromUBytes);
        mbcsHeader.flags=               ds->readUInt32(inMBCSHeader->flags);
        mbcsHeader.fromUBytesLength=    ds->readUInt32(inMBCSHeader->fromUBytesLength);

        extOffset=(int32_t)mbcsHeader.flags>>8;
        outputType=(uint8_t)mbcsHeader.flags;

        /* make sure that the output type is known */
        switch(outputType) {
        case MBCS_OUTPUT_1:
        case MBCS_OUTPUT_2:
        case MBCS_OUTPUT_3:
        case MBCS_OUTPUT_4:
        case MBCS_OUTPUT_3_EUC:
        case MBCS_OUTPUT_4_EUC:
        case MBCS_OUTPUT_2_SISO:
        case MBCS_OUTPUT_EXT_ONLY:
            /* OK */
            break;
        default:
            udata_printError(ds, "ucnv_swap(): unsupported MBCS output type 0x%x\n",
                             outputType);
            *pErrorCode=U_UNSUPPORTED_ERROR;
            return 0;
        }

        /* calculate the length of the MBCS data */
        if(extOffset==0) {
            size=(int32_t)(mbcsHeader.offsetFromUBytes+mbcsHeader.fromUBytesLength);
        } else {
            /* there is extension data after the base data, see ucnv_ext.h */
            if(length<(extOffset+UCNV_EXT_INDEXES_MIN_LENGTH*4)) {
                udata_printError(ds, "ucnv_swap(): too few bytes (%d after headers) for an ICU MBCS .cnv conversion table with extension data\n",
                                 length);
                *pErrorCode=U_INDEX_OUTOFBOUNDS_ERROR;
                return 0;
            }

            inExtIndexes=(const int32_t *)(inBytes+extOffset);
            size=extOffset+udata_readInt32(ds, inExtIndexes[UCNV_EXT_SIZE]);
        }

        if(length>=0) {
            if(length<size) {
                udata_printError(ds, "ucnv_swap(): too few bytes (%d after headers) for an ICU MBCS .cnv conversion table\n",
                                 length);
                *pErrorCode=U_INDEX_OUTOFBOUNDS_ERROR;
                return 0;
            }

            /* copy the data for inaccessible bytes */
            if(inBytes!=outBytes) {
                uprv_memcpy(outBytes, inBytes, size);
            }

            /* swap the _MBCSHeader */
            ds->swapArray32(ds, &inMBCSHeader->countStates, 7*4,
                               &outMBCSHeader->countStates, pErrorCode);

            if(outputType==MBCS_OUTPUT_EXT_ONLY) {
                /*
                 * extension-only file,
                 * contains a base name instead of normal base table data
                 */

                /* swap the base name, between the header and the extension data */
                ds->swapInvChars(ds, inMBCSHeader+1, uprv_strlen((const char *)(inMBCSHeader+1)),
                                    outMBCSHeader+1, pErrorCode);
            } else {
                /* normal file with base table data */

                /* swap the state table, 1kB per state */
                ds->swapArray32(ds, inMBCSHeader+1, (int32_t)(mbcsHeader.countStates*1024),
                                   outMBCSHeader+1, pErrorCode);

                /* swap the toUFallbacks[] */
                offset=sizeof(_MBCSHeader)+mbcsHeader.countStates*1024;
                ds->swapArray32(ds, inBytes+offset, (int32_t)(mbcsHeader.countToUFallbacks*8),
                                   outBytes+offset, pErrorCode);

                /* swap the unicodeCodeUnits[] */
                offset=mbcsHeader.offsetToUCodeUnits;
                count=mbcsHeader.offsetFromUTable-offset;
                ds->swapArray16(ds, inBytes+offset, (int32_t)count,
                                   outBytes+offset, pErrorCode);

                /* offset to the stage 1 table, independent of the outputType */
                offset=mbcsHeader.offsetFromUTable;

                if(outputType==MBCS_OUTPUT_1) {
                    /* SBCS: swap the fromU tables, all 16 bits wide */
                    count=(mbcsHeader.offsetFromUBytes-offset)+mbcsHeader.fromUBytesLength;
                    ds->swapArray16(ds, inBytes+offset, (int32_t)count,
                                       outBytes+offset, pErrorCode);
                } else {
                    /* otherwise: swap the stage tables separately */

                    /* stage 1 table: uint16_t[0x440 or 0x40] */
                    if(inStaticData->unicodeMask&UCNV_HAS_SUPPLEMENTARY) {
                        count=0x440*2; /* for all of Unicode */
                    } else {
                        count=0x40*2; /* only BMP */
                    }
                    ds->swapArray16(ds, inBytes+offset, (int32_t)count,
                                       outBytes+offset, pErrorCode);

                    /* stage 2 table: uint32_t[] */
                    offset+=count;
                    count=mbcsHeader.offsetFromUBytes-offset;
                    ds->swapArray32(ds, inBytes+offset, (int32_t)count,
                                       outBytes+offset, pErrorCode);

                    /* stage 3/result bytes: sometimes uint16_t[] or uint32_t[] */
                    offset=mbcsHeader.offsetFromUBytes;
                    count=mbcsHeader.fromUBytesLength;
                    switch(outputType) {
                    case MBCS_OUTPUT_2:
                    case MBCS_OUTPUT_3_EUC:
                    case MBCS_OUTPUT_2_SISO:
                        ds->swapArray16(ds, inBytes+offset, (int32_t)count,
                                           outBytes+offset, pErrorCode);
                        break;
                    case MBCS_OUTPUT_4:
                        ds->swapArray32(ds, inBytes+offset, (int32_t)count,
                                           outBytes+offset, pErrorCode);
                        break;
                    default:
                        /* just uint8_t[], nothing to swap */
                        break;
                    }
                }
            }

            if(extOffset!=0) {
                /* swap the extension data */
                inBytes+=extOffset;
                outBytes+=extOffset;

                /* swap toUTable[] */
                offset=udata_readInt32(ds, inExtIndexes[UCNV_EXT_TO_U_INDEX]);
                length=udata_readInt32(ds, inExtIndexes[UCNV_EXT_TO_U_LENGTH]);
                ds->swapArray32(ds, inBytes+offset, length*4, outBytes+offset, pErrorCode);

                /* swap toUUChars[] */
                offset=udata_readInt32(ds, inExtIndexes[UCNV_EXT_TO_U_UCHARS_INDEX]);
                length=udata_readInt32(ds, inExtIndexes[UCNV_EXT_TO_U_UCHARS_LENGTH]);
                ds->swapArray16(ds, inBytes+offset, length*2, outBytes+offset, pErrorCode);

                /* swap fromUTableUChars[] */
                offset=udata_readInt32(ds, inExtIndexes[UCNV_EXT_FROM_U_UCHARS_INDEX]);
                length=udata_readInt32(ds, inExtIndexes[UCNV_EXT_FROM_U_LENGTH]);
                ds->swapArray16(ds, inBytes+offset, length*2, outBytes+offset, pErrorCode);

                /* swap fromUTableValues[] */
                offset=udata_readInt32(ds, inExtIndexes[UCNV_EXT_FROM_U_VALUES_INDEX]);
                /* same length as for fromUTableUChars[] */
                ds->swapArray32(ds, inBytes+offset, length*4, outBytes+offset, pErrorCode);

                /* no need to swap fromUBytes[] */

                /* swap fromUStage12[] */
                offset=udata_readInt32(ds, inExtIndexes[UCNV_EXT_FROM_U_STAGE_12_INDEX]);
                length=udata_readInt32(ds, inExtIndexes[UCNV_EXT_FROM_U_STAGE_12_LENGTH]);
                ds->swapArray16(ds, inBytes+offset, length*2, outBytes+offset, pErrorCode);

                /* swap fromUStage3[] */
                offset=udata_readInt32(ds, inExtIndexes[UCNV_EXT_FROM_U_STAGE_3_INDEX]);
                length=udata_readInt32(ds, inExtIndexes[UCNV_EXT_FROM_U_STAGE_3_LENGTH]);
                ds->swapArray16(ds, inBytes+offset, length*2, outBytes+offset, pErrorCode);

                /* swap fromUStage3b[] */
                offset=udata_readInt32(ds, inExtIndexes[UCNV_EXT_FROM_U_STAGE_3B_INDEX]);
                length=udata_readInt32(ds, inExtIndexes[UCNV_EXT_FROM_U_STAGE_3B_LENGTH]);
                ds->swapArray32(ds, inBytes+offset, length*4, outBytes+offset, pErrorCode);

                /* swap indexes[] */
                length=udata_readInt32(ds, inExtIndexes[UCNV_EXT_INDEXES_LENGTH]);
                ds->swapArray32(ds, inBytes, length*4, outBytes, pErrorCode);
            }
        }
    } else {
        udata_printError(ds, "ucnv_swap(): unknown conversionType=%d!=UCNV_MBCS\n",
                         inStaticData->conversionType);
        *pErrorCode=U_UNSUPPORTED_ERROR;
        return 0;
    }

    return headerSize+(int32_t)staticDataSize+size;
}

#endif /* #if !UCONFIG_NO_LEGACY_CONVERSION */

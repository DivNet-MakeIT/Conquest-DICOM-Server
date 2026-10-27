// GetPrivateProfileString() -- approximate implementation of
// Windows NT System Services version of GetPrivateProfileString()
// probably doesn't handle the NULL key for section name or value key
// correctly also, doesn't provide Microsoft backwards compatability
// wrt TAB characters in the value string -- Microsoft terminates value
// at the first TAB, but I couldn't discover what the behavior should
// be regarding TABS in quoted strings so, I treat tabs like any other
// characters -- NO comments following value string separated by a TAB
// are allowed (that is an anachronism anyway)

// 20040111	mvh	Fixes: defaults now work, allow DOS type files, case insensitive
// 20070330	mvh	Change by Mark Pearson: also allow tabs instead of spaces around =
// 20090620	jf	Include file stuff
// 20091231	bcb	Changed char* to const char* and added blank[]="" for gcc4.2 warnings
// 20100111	mvh	Merged
// 20100124	mvh	Fix: blank item (e.g., password = ) returned default not empty
// 20100124	mvh	Added cache for ini items: accelerates server with cached disk data 7x!!!!
// 20100124	mvh	ini file is tested for newer version every second
// 20100309	bcb	Added double parentheses (gcc4.2 Warnings)
// 20100703	mvh	Merged	
// 20110320	mvh	Removal of trailing blanks in value also removed one character after blank
// 20110331	mvh	Added FlushPrivateProfileStringCache
// 20110407	mvh	Extended FlushPrivateProfileStringCache to reset timestamp
//			Fixed that was reading beyond current section
// 20191019	mvh     Attemp to fix buffer overrun when reading long entries
// 20261026	mmd	Cache: a hit is copied with the caller's buffer length (cut and the same
//			'trucated' line as a file read, instead of an unbounded strcpy) and inside
//			the cache lock; a section, key or value that does not fit the cache arrays
//			(64/64/1024) or a value cut to the caller's buffer is not cached (it is read
//			from the file each time). Values that fit: results unchanged.
// 20261026	mmd	Cache lock made once by a static object (it was initialised again on every
//			emptied cache, possibly while another thread held it); the empty test, the
//			ini file time check, the invalidation, FlushPrivateProfileStringCache and the
//			gpps/gppstime statistics are done under it. A value read from the file is not
//			cached when the cache was emptied while it was read (it may be the old one).
//			Lookup order and the values returned are unchanged.
// 20261026	mmd	GppsStatistics: gpps and gppstime read together under the cache lock
// 20261027	mmd	The cache lock is made at its first use (IniLock), not by a static object: global
//			PDU objects in dgate.cpp read dicom.ini in their constructors before the static
//			objects of this file are made (a zeroed CRITICAL_SECTION crashes on Windows)

int      gpps=0, gppstime=0;

#include <stdio.h>
#ifdef UNIX
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include "gpps.hpp"
#include "dicom.hpp"
#include "dprintf.hpp"	// Debug output

#ifndef WIN32
#define stricmp(s1, s2) strcasecmp(s1, s2) //DUCKHEAD92
#endif

#define NINICACHE 1000

char			IniCache[NINICACHE][64];
char			IniSections[NINICACHE][64];
char			IniValues[NINICACHE][1024];
int			IniCheckSums[NINICACHE];
CRITICAL_SECTION 	IniCritical;
int			IniTop=0, IniBottom=0;
time_t			IniTm=0, lasttime=0;
static unsigned int	IniGeneration=0;	// + 1 whenever the cache is emptied or flushed

// The lock is made at its first use, which comes before main() starts any thread: the global
// ExtendedPDU_Service objects in dgate.cpp read dicom.ini in their constructors while the static
// objects are made, before those of this file (included last in total.cpp). A lock made by a
// static object here was entered there before it was made: harmless with a zeroed pthread
// mutex, a crash with a zeroed Windows CRITICAL_SECTION.
static CRITICAL_SECTION *IniLock(void)
	{
	static int made = 0;
	if (!made)
		{
		InitializeCriticalSection(&IniCritical);
		made = 1;
		}
	return &IniCritical;
	}

// statistics (gpps, gppstime) under the cache lock
static void IniAddTime(int t)
	{
	EnterCriticalSection(IniLock());
	gppstime += time(NULL)-t;
	LeaveCriticalSection(&IniCritical);
	}

// gen: IniGeneration seen by the isin_IniCache call before the file was read; when the
// cache was emptied since, the value may be from the old file and is not stored.
int into_IniCache(const char *section, const char *in, char *value, unsigned int gen)
	{ 
	int i, sum, r;

	// only what fits the fixed arrays is cached (else read from the file every time)
	if (!section || !in || !value) return -1;
	if (strlen(section) >= sizeof(IniSections[0]) || strlen(in) >= sizeof(IniCache[0]) ||
	    strlen(value) >= sizeof(IniValues[0])) return -1;

	const char *p = in;
	sum = 0;
	while(*p) sum+=*p++;
	p = section;
	while(*p) sum+=*p++;

	EnterCriticalSection(IniLock());

	if (gen != IniGeneration)
		{
		LeaveCriticalSection(&IniCritical);
		return -1;
		}

	/* clear cache if it is empty */
	if (IniTop==0 && IniBottom==0)
		for (i=0; i<NINICACHE; i++)
			IniCache[i][0]=0;

	if ((IniTop+1)%NINICACHE == IniBottom)
		IniBottom = (IniBottom+1)%NINICACHE;	// delete oldest entry

	strcpy(IniCache[IniTop], in);
	strcpy(IniSections[IniTop], section);
	strcpy(IniValues[IniTop], value);
        IniCheckSums[IniTop] = sum;
        r = IniTop;

      	IniTop = (IniTop+1)%NINICACHE;

	LeaveCriticalSection(&IniCritical);

	return r;
  	}

// On a hit the value is copied to out (at most outlen-1 characters, NUL ended) while
// the cache is locked; *len = its length as returned to the caller. Counts the call
// (gpps) and, on a hit, its time since t (gppstime); *gen = IniGeneration (for
// into_IniCache after a miss). Everything under the cache lock.
int isin_IniCache(const char *section, const char *in, char *file, char *out, size_t outlen, size_t *len, int t, unsigned int *gen)
	{
	int i, sum;
	const char *p;
	struct stat statbuf;

	EnterCriticalSection(IniLock());
	gpps++;
	*gen = IniGeneration;

	if ((IniTop==0 && IniBottom==0) || !section || !in || !out || !outlen)
		{
		LeaveCriticalSection(&IniCritical);
		return -1;
		}

	sum = 0;
	p = in;
	while(*p) sum+=*p++;
	p = section;
	while(*p) sum+=*p++;

	// retest for changed ini file once per second
	if (lasttime!=time(NULL))
	  {
	  lasttime = time(NULL);
	  if (stat(file, &statbuf) != -1)
		if (statbuf.st_mtime != IniTm)
		{
		IniTop=0;
		IniBottom=0;
		IniGeneration++;
		*gen = IniGeneration;
		IniTm = statbuf.st_mtime;
		lasttime = time(NULL);
	        LeaveCriticalSection(&IniCritical);
		return -1;
		}
	  }

	for (i=IniTop; i!=IniBottom; i=(i==0 ? NINICACHE-1 : i-1))
		{
		if (IniCheckSums[i] == sum)
			if (strcmp(IniCache[i], in)==0 && strcmp(IniSections[i], section)==0)
				{
				size_t n = strlen(IniValues[i]);
				if (n >= outlen)
					{
					n = outlen-1;
					OperatorConsole.printf("***Ini file item '%s:%s' trucated\n", section, in);
					}
				memcpy(out, IniValues[i], n);
				out[n] = 0;
				*len = n;
				gppstime += time(NULL)-t;
				LeaveCriticalSection(&IniCritical);
				return i;
				}
		}
				
	LeaveCriticalSection(&IniCritical);
	return -1;
	}

DWORD
MyGetPrivateProfileString(const char *theSection,	// section name
	const char *theKey,			// search key name
	const char *theDefault,			// default value if not found
	char *theReturnBuffer,			// return valuse stored here
	size_t theReturnBufferLength,		// byte length of return buffer
	char *theIniFileName)			// pathname of ini file to search
{
	int i, t = time(NULL);
	size_t cachedlen = 0;
	unsigned int gen = 0;
	BOOL truncated = FALSE;

	// try to find it in the cache (copied with the caller's length); counts the call
	if ((i = isin_IniCache(theSection, theKey, theIniFileName, theReturnBuffer, theReturnBufferLength, &cachedlen, t, &gen))>=0)
	{
	  return cachedlen;
	}

	FILE *aFile = theIniFileName ? fopen(theIniFileName, "r") : NULL;
	size_t aLength = (theDefault == NULL) ? 0 : strlen(theDefault);
	BOOL matched = FALSE;

	if(theReturnBufferLength == 0 || theReturnBuffer == NULL)
	{
		if(aFile)
		{
			fclose(aFile);
		}
		return 0;
	}

	if(aFile == NULL)
	{
		// no ini file specified, return the default

		++aLength;	// room for NULL char
		aLength = theReturnBufferLength < aLength ?
			theReturnBufferLength : aLength;
		strncpy(theReturnBuffer, theDefault, aLength);
		theReturnBuffer[aLength - 1] = '\0';
		return aLength - 1;
	}

	char aLine[2048];
	char *aValue;
	char *aString;
	char blank[] = "";
	size_t aLineLength;
	size_t aReturnLength = 0;
	BOOL aSectionFound = FALSE;

	while(fgets(aLine, sizeof(aLine), aFile) != NULL)
	{
		aLineLength = strlen(aLine);
		// strip final '\n'
		if(aLineLength > 0 && aLine[aLineLength - 1] == '\n')
		{
			aLine[aLineLength - 1] = '\0';
			aLineLength--;
		}
		// strip final '\r'
		if(aLineLength > 0 && aLine[aLineLength - 1] == '\r')
		{
			aLine[aLineLength - 1] = '\0';
			aLineLength--;
		}
		switch(*aLine)
		{
			case ' ': // blank line
			case ';': // comment line
				continue;
			break;
			
			case '[':	// section marker

				if((aString = strchr(aLine, ']')))
				{
					*aString = '\0';

					// accept as matched if NULL key or exact match

					if(!theSection || !stricmp(aLine + 1, theSection))
					{
						aSectionFound = TRUE;
					}
					else if (theSection && stricmp(aLine + 1, theSection))
					{
						aSectionFound = FALSE;
					}
				}

			break;

			default:

				// try to match value keys if in proper section

				if(aSectionFound)
				{
					// try to match requested key
	
					if((aString = aValue = strchr(aLine, '=')))
					{
						*aValue = '\0';
						++aValue;

						// strip leading blanks in value field

						while((*aValue == ' ' || *aValue == '\t') && aValue < aLine + sizeof(aLine))
						{
							*aValue++ = '\0';
						}
						if(aValue >= aLine + sizeof(aLine))
						{
							aValue = blank;
						}
					}
					else
					{
						aValue = blank;
					}

					// strip trailing blanks from key

					if(aString)
					{
						while(--aString >= aLine && (*aString == ' ' || *aString == '\t'))
						{
							*aString = '\0';
						}
					}

					// see if key is matched
					
					if(theKey == NULL || !stricmp(theKey, aLine))
					{
						// matched -- first, terminate value part
						matched = TRUE;
						aLineLength = strlen(aValue);

						// remove trailing blanks from aValue if any

						aString = aValue + aLineLength;
						
						while(--aString > aValue && (*aString == ' ' || *aString == '\t'))
						{
							*aString = '\0';
							--aLineLength;
						}

						// unquote value if quoted

						if(aLineLength >= 2 && aValue[0] == '"' &&
							aValue[aLineLength - 1] == '"')
						{
							// string quoted with double quotes

							aValue[aLineLength - 1] = '\0';
							++aValue;
							aLineLength -= 2;
						}
						else
						{
							// single quotes allowed also...

							if(aLineLength >= 2 && aValue[0] == '\'' &&
								aValue[aLineLength - 1] == '\'')
							{
								aValue[aLineLength - 1] = '\0';
								++aValue;
								aLineLength -= 2;
							}
						}

						// compute maximum length copyable

						aLineLength = (aLineLength <
							theReturnBufferLength - aReturnLength) ? aLineLength :
							theReturnBufferLength - aReturnLength;

						// do the copy to return buffer

						if(aLineLength>=0)
						{
							strncpy(&theReturnBuffer[aReturnLength],
								aValue, aLineLength);
							aReturnLength += aLineLength;
							if (aReturnLength >= theReturnBufferLength)
							{
								truncated = TRUE;	// not cached: a larger buffer must get it all
								aReturnLength = theReturnBufferLength-1;
								OperatorConsole.printf("***Ini file item '%s:%s' trucated\n", theSection, theKey);
							}								
							if(aReturnLength < theReturnBufferLength)
							{
								theReturnBuffer[aReturnLength] = '\0';
								++aReturnLength;
								
							}
						}
					}
				}

			break;
		}
	}

	if(aFile)
	{
		fclose(aFile);
	}

	if (aReturnLength==0 && !matched)
	{
		++aLength;	// room for NULL char
		aLength = theReturnBufferLength < aLength ?
			theReturnBufferLength : aLength;
		strncpy(theReturnBuffer, theDefault, aLength);
		theReturnBuffer[aLength - 1] = '\0';
		// store into cache (not a default that was cut to this buffer)
		if (theDefault && strlen(theDefault) < theReturnBufferLength)
			into_IniCache(theSection, theKey, theReturnBuffer, gen);
		IniAddTime(t);
		return aLength - 1;
	}

	// store into cache (not a value that was cut to this buffer)
	if (!truncated)
		into_IniCache(theSection, theKey, theReturnBuffer, gen);
	IniAddTime(t);
	return aReturnLength > 0 ? aReturnLength - 1 : 0;
}

// the statistics (number of reads, seconds spent) as one consistent pair, under the cache lock
void GppsStatistics(int *count, int *seconds)
{ EnterCriticalSection(IniLock());
  if (count)   *count   = gpps;
  if (seconds) *seconds = gppstime;
  LeaveCriticalSection(&IniCritical);
}

void FlushPrivateProfileStringCache(void)
{ EnterCriticalSection(IniLock());
  lasttime = 0;
  IniTm    = 0;
  IniGeneration++;	// a value being read now is not cached
  LeaveCriticalSection(&IniCritical);
}

/* mvh  19990827: added (limited) wildcard mapping in GetACRNema: AE, port or IP may end on '*' 
   mvh  20001105: replaced m-alloc by new
   mvh  20011109: Made AE mapping case insensitive
   ljz  20020524: In function 'GetACRNema', first try case SENSITIVE then INSENSITIVE
   mvh  20030701: Added compression column (with this code is this column is not optional!)
   mvh  20030703: KNOWN BUG: number of columns may not be 3 (crashes)
   ljz  20030709: Solved above problem; rewritten parsing of Acrnema.map
   ljz  20030711: Fixed trailing spaces in acrnema.map
   ljz  20031118: Fixed leak InitACRNemaAddressArray
   mvh  20261005: Protect against too long passed AEs; limit reading of amap fields to 32-128-64-32
   mmd  20261025: Map access under a lock; the map is built aside and swapped in (same result as before);
                  entry copy/put/delete functions so that other files do not touch the array
   mmd  20261025: Optional 5th column (options T, SC, R, Rn, RI); the rest of the line after the
                  compression column is kept and written back; GetACRNemaEntryByAE
*/
   

#ifndef	WHEDGE
#	include	"dgate.hpp"
#else
#	include	"master.h"
#endif
#include <ctype.h>


BYTE	ACRNEMAMAP [ 256 ] = "acrnema.map";
static Array < ACRNemaAddress * >	ACRNemaAddressArray;

#ifdef UNIX
#ifndef EnterCriticalSection
#	define EnterCriticalSection(a) pthread_mutex_lock(a)
#endif
#ifndef LeaveCriticalSection
#	define LeaveCriticalSection(a) pthread_mutex_unlock(a)
#endif
#ifndef CRITICAL_SECTION
#	define CRITICAL_SECTION pthread_mutex_t
#endif
#ifndef InitializeCriticalSection
#	define InitializeCriticalSection(a) pthread_mutex_init(a, NULL);
#endif
#endif

// One lock for every access to the map. It is initialised by a static object,
// i.e. before main() starts any thread (the incoming-folder monitor thread can
// run before the map is first loaded).
static CRITICAL_SECTION	AMapCritical;
static struct AMapCriticalInit
	{
	AMapCriticalInit() { InitializeCriticalSection(&AMapCritical); }
	}	AMapCriticalInitObject;

static void AMapLock(void)
	{
	EnterCriticalSection(&AMapCritical);
	}

static void AMapUnlock(void)
	{
	LeaveCriticalSection(&AMapCritical);
	}

static void AMapClear(Array < ACRNemaAddress * > &A)
	{
	while ( A.GetSize() )
		{
		delete A.Get(0);
		A.RemoveAt(0);
		}
	}

static void AMapCopyField(char *dest, const char *src, int size)
	{
	strncpy(dest, src, size-1);
	dest[size-1] = 0;
	}



// Parse the first token of the options column. Recognised (any case):
//   -        no options
//   T        TLS              SC   storage commitment for data sent to the node
//   TSC/SCT  both             R    re-send missing objects, limit from dicom.ini
//   Rn       limit n (1..999) RI   until committed            R0   no re-send
// T and SC in any order, R last (e.g. SCR5, TSCRI). Anything else gives no
// options (the column may hold user data, e.g. lua/association.lua).
int
ParseACRNemaOptions(const char *Options, const char *Name, int *ResendLimit, BOOL Warn)
	{
	char	tok[32];
	int	i, flags = 0, n;
	const char *p;

	*ResendLimit = 0;
	tok[0] = 0;
	if (Options==NULL || sscanf(Options, "%31s", tok)!=1) return 0;
	if (strcmp(tok, "-")==0) return 0;
	for (i=0; tok[i]; i++) tok[i] = toupper((unsigned char)tok[i]);

	p = tok;
	while (*p)
		{
		if (p[0]=='S' && p[1]=='C' && !(flags & ACR_SC)) { flags |= ACR_SC; p += 2; continue; }
		if (p[0]=='T' && !(flags & ACR_TLS)) { flags |= ACR_TLS; p += 1; continue; }
		if (p[0]=='R')
			{
			if (p[1]==0) { flags |= ACR_RESEND; *ResendLimit = ACR_RESEND_INI; p += 1; continue; }
			if (p[1]=='I' && p[2]==0) { flags |= ACR_RESEND; *ResendLimit = ACR_RESEND_ALWAYS; p += 2; continue; }
			n = 0;
			for (i=1; p[i]>='0' && p[i]<='9' && i<5; i++) n = n*10 + p[i]-'0';
			if (i>1 && p[i]==0 && n<=999)
				{
				if (n>0) { flags |= ACR_RESEND; *ResendLimit = n; }
				p += i;
				continue;
				}
			}
		// not an option token: no options for this line
		if (Warn) OperatorConsole.printf("*** acrnema.map: %s: 5th column '%s' is not an option (T, SC, R...), ignored\n", Name, tok);
		*ResendLimit = 0;
		return 0;
		}

	if ((flags & ACR_RESEND) && !(flags & ACR_SC))
		{
		if (Warn) OperatorConsole.printf("*** acrnema.map: %s: R needs SC, R ignored\n", Name);
		flags &= ~ACR_RESEND; *ResendLimit = 0;
		}
	if ((flags & ACR_RESEND) && strchr(Name, '*'))
		{
		if (Warn) OperatorConsole.printf("*** acrnema.map: %s: R is not allowed on a wildcard entry, R ignored\n", Name);
		flags &= ~ACR_RESEND; *ResendLimit = 0;
		}
	return flags;
	}

static void
SetACRNemaOptions(ACRNemaAddress *pACR, const char *Options, BOOL Warn)
	{
	while (*Options==' ') Options++;
	if (strlen(Options) > sizeof(pACR->Options)-1 && Warn)
		OperatorConsole.printf("*** acrnema.map: %s: text after the compression column cut to %d characters\n", pACR->Name, (int)sizeof(pACR->Options)-1);
	strncpy(pACR->Options, Options, sizeof(pACR->Options)-1);
	pACR->Options[sizeof(pACR->Options)-1] = 0;
	pACR->Flags = ParseACRNemaOptions(pACR->Options, pACR->Name, &pACR->ResendLimit, Warn);
	}

static BOOL
ParseACRNemaMap(Array < ACRNemaAddress * > &NewArray)
{ FILE*			f;
  char*			pBuffer;
  int			i, iSize;
  char			*pSrc, *pDest;
  ACRNemaAddress*	pACR;
  int			iNbAddresses;

  /* Open AcrNema.map */
  f = fopen((char*)ACRNEMAMAP, "rb");
  if (!f)
    return FALSE;

  /* Read it in memory */
  fseek(f, 0, SEEK_END);
  iSize = ftell(f);
  fseek(f, 0, SEEK_SET);
  pBuffer = (char*)malloc(iSize + 2);
  if (!pBuffer)
  { fclose(f);
    return FALSE;
  }
  if (fread(pBuffer, 1, iSize, f) != (unsigned int)iSize)
  { fclose(f);
    free(pBuffer);
    return FALSE;
  }
  fclose(f);

  /* Make it a CR and zero terminated string */
  pBuffer[iSize++] = '\r';
  pBuffer[iSize]   = '\0';

  /* Get rid of all comments like this one */
  pSrc = pBuffer;
  pDest = pBuffer;
  i = 0;
  while (i<iSize)
  { if ((pSrc[i] == '/') && (pSrc[i + 1] == '*'))
    { /* Found the start of a comment */
      i += 2;
      while (i<iSize)
      { if ((pSrc[i] == '*') && (pSrc[i + 1] == '/'))
	{ i++;
	  break;
	}
	i++;
      }
    }
    else
      *pDest++ = pSrc[i];
    i++;
  }
  *pDest = 0;

  /* Replace all LF by CR, and all '\t' by ' ' */
  iSize = strlen(pSrc);
  for (i=0; i<iSize; i++)
    if (pSrc[i] == '\n')
      pSrc[i] = '\r';
    else if (pSrc[i] == '\t')
      pSrc[i] = ' ';

  /* The characters '#', ';' and '//' are one-line comments */
  pDest = pSrc;
  i = 0;
  while (i<iSize)
  { if ((pSrc[i] == '/') && (pSrc[i + 1] == '/'))
    { /* Found the start of a comment */
      i += 2;
      while (i<iSize)
      { if (pSrc[i] == '\r')
	  break;
	i++;
      }
    }
    else if ((pSrc[i] == '#') || (pSrc[i + 1] == ';'))
    { /* Found the start of a comment */
      i += 1;
      while (i<iSize)
      { if (pSrc[i] == '\r')
	  break;
	i++;
      }
    }
    else
      *pDest++ = pSrc[i];
    i++;
  }
  *pDest = 0;

  /* Eliminate empty lines, double space... */
  iSize = strlen(pSrc);
  pDest = pSrc;
  i = 0;
  while (i<iSize)
  { if (pSrc[i] == ' ')
    { if (pDest == pSrc)
      { /* Skip leading space */
        i++;
	continue;
      }
      else if ((pDest[-1] == ' ') || (pDest[-1] == '\r'))
      { /* Skip space when the previous was space or CR */
        i++;
	continue;
      }
      else if (pSrc[i+1] == '\r')
      { /* Skip space when the next is CR */
        i++;
	continue;
      }
    }
    else if (pSrc[i] == '\r')
    { if (pDest == pSrc)
      { /* Skip leading CR */
        i++;
	continue;
      }
      else if (pDest[-1] == '\r')
      { /* Skip CR when the previous was a CR */
        i++;
	continue;
      }
    }
    *pDest++ = pSrc[i];
    i++;
  }
  *pDest = 0;

  /* Count the iNbAddresses */
  iSize = strlen(pSrc);
  iNbAddresses = 0;
  for (i=0; i<iSize; i++)
    if (pSrc[i] == '\r')
      iNbAddresses++;

  /* Now fill the ACRNemaAddressArray */
  pDest = pSrc;
  for (i=0; i<iNbAddresses; i++)
  { pACR = new ACRNemaAddress;
    strcpy(pACR->Name, "");
    strcpy(pACR->IP, "");
    strcpy(pACR->Port, "");
    strcpy(pACR->Compress, "un");
    pACR->Options[0] = 0;
    pACR->Flags = 0;
    pACR->ResendLimit = 0;

    /* Get a line */
    *strchr(pDest, '\r') = 0;
    int nused = -1;
    if (sscanf(pDest, "%31s %127s %63s %31s%n",
	pACR->Name,
	pACR->IP,
	pACR->Port,
	pACR->Compress, &nused)==4 && nused>0)
      SetACRNemaOptions(pACR, pDest+nused, TRUE);	// optional 5th column and the rest of the line

    NewArray.Add(pACR);
    pDest = pDest + strlen(pDest) + 1;
  }

  free(pBuffer);
  return TRUE;
}

// Load (or reload) the map. The new map is built aside and swapped in under
// the lock, so a thread looking up an AE never sees a half-built map. As
// before, a file that cannot be read leaves an empty map.
BOOL
InitACRNemaAddressArray(void)
	{
	Array < ACRNemaAddress * >	NewArray;
	BOOL				ok;

	ok = ParseACRNemaMap(NewArray);
	if (!ok) AMapClear(NewArray);

	AMapLock();
	AMapClear(ACRNemaAddressArray);
	while ( NewArray.GetSize() )
		{
		ACRNemaAddressArray.Add(NewArray.Get(0));
		NewArray.RemoveAt(0);
		}
	AMapUnlock();
	return ok;
	}


BOOL
CloseACRNemaAddressArray()
	{
	AMapLock();
	AMapClear(ACRNemaAddressArray);
	AMapUnlock();
	return ( TRUE );
	}

// Number of entries in the map
UINT
GetACRNemaCount(void)
	{
	UINT	n;
	AMapLock();
	n = ACRNemaAddressArray.GetSize();
	AMapUnlock();
	return n;
	}

// Copy entry Index into *Entry; FALSE if there is no such entry
BOOL
GetACRNemaEntry(UINT Index, ACRNemaAddress *Entry)
	{
	BOOL	ok = FALSE;
	AMapLock();
	if (Index < ACRNemaAddressArray.GetSize())
		{
		*Entry = *ACRNemaAddressArray.Get(Index);
		ok = TRUE;
		}
	AMapUnlock();
	return ok;
	}

// Change entry Index, or append a new entry when Index is past the end.
// A NULL field keeps its value (a new entry starts as "" "" "" "un").
// Fields are cut to the size of the map structure.
BOOL
PutACRNemaEntry(UINT Index, const char *Name, const char *IP, const char *Port, const char *Compress, const char *Options)
	{
	ACRNemaAddress	*AAPtr;

	AMapLock();
	if (Index < ACRNemaAddressArray.GetSize())
		AAPtr = ACRNemaAddressArray.Get(Index);
	else
		{
		AAPtr = new ACRNemaAddress;
		strcpy(AAPtr->Name, "");
		strcpy(AAPtr->IP, "");
		strcpy(AAPtr->Port, "");
		strcpy(AAPtr->Compress, "un");
		AAPtr->Options[0] = 0;
		AAPtr->Flags = 0;
		AAPtr->ResendLimit = 0;
		ACRNemaAddressArray.Add(AAPtr);
		}
	if (Name)     AMapCopyField(AAPtr->Name,     Name,     sizeof(AAPtr->Name));
	if (IP)       AMapCopyField(AAPtr->IP,       IP,       sizeof(AAPtr->IP));
	if (Port)     AMapCopyField(AAPtr->Port,     Port,     sizeof(AAPtr->Port));
	if (Compress) AMapCopyField(AAPtr->Compress, Compress, sizeof(AAPtr->Compress));
	if (Options)  SetACRNemaOptions(AAPtr, Options, TRUE);
	else if (Name) AAPtr->Flags = ParseACRNemaOptions(AAPtr->Options, AAPtr->Name, &AAPtr->ResendLimit, FALSE);
	AMapUnlock();
	return TRUE;
	}

// Remove entry Index; FALSE if there is no such entry
BOOL
DeleteACRNemaEntry(UINT Index)
	{
	BOOL	ok = FALSE;
	AMapLock();
	if (Index < ACRNemaAddressArray.GetSize())
		{
		delete ACRNemaAddressArray.Get(Index);
		ACRNemaAddressArray.RemoveAt(Index);
		ok = TRUE;
		}
	AMapUnlock();
	return ok;
	}


static
BOOL
__iswhitespacea(char	ch)
	{
	switch ( ch )
		{
		case	' ':
		case	'\t':
		case	'\r':
		case	'\n':
		case	0:
			return ( TRUE );
		}
	return ( FALSE );
	}


BOOL
GetACRNema(char* ACRNema, char* ip, char* port, char* compress)
	{
	UINT	Index;
	char	s[20];
	char	*t;

	// trim the ACRNema address

        memset(s, 0, 20);
	strncpy(s, ACRNema, 19);

	if(!strlen(s))
		return ( FALSE );
	while(__iswhitespacea(s[strlen(s)-1]))
		{
		s[strlen(s)-1] = '\0';
		if(!strlen(s))
			return ( FALSE );
		}


	AMapLock();
	Index = 0;
	while ( Index < ACRNemaAddressArray.GetSize())
		{
		t = ACRNemaAddressArray.Get(Index)->Name;

		// try wildcard mapping, e.g., s=NG12, t=NG*, IP=101.11.11.*, Port=56*
		if (t[strlen(t)-1] == '*')
			{
			if (strlen(t)-1 <= strlen(s) && !memicmp(t, s, strlen(t)-1))	// do not compare past the end of s
				{
				strcpy(ip, ACRNemaAddressArray.Get(Index)->IP);
				if (ip[strlen(ip)-1]=='*')
					strcpy(ip+strlen(ip)-1, s+strlen(t)-1);

				strcpy(port, ACRNemaAddressArray.Get(Index)->Port);
				if (port[strlen(port)-1]=='*')
					strcpy(port+strlen(port)-1, s+strlen(t)-1);
				strcpy(compress, ACRNemaAddressArray.Get(Index)->Compress);
				AMapUnlock();
				return ( TRUE );
				}
			}

		// try exact match (case sensitive)
		if (!strcmp(t, s))
			{
			strcpy(ip, ACRNemaAddressArray.Get(Index)->IP);
			strcpy(port, ACRNemaAddressArray.Get(Index)->Port);
			strcpy(compress, ACRNemaAddressArray.Get(Index)->Compress);
			AMapUnlock();
			return ( TRUE );
			}
		++Index;
		}

	Index = 0;
	while ( Index < ACRNemaAddressArray.GetSize())
		{
		t = ACRNemaAddressArray.Get(Index)->Name;

		// try exact match (case insensitive)
		if (!stricmp(t, s))
			{
			strcpy(ip, ACRNemaAddressArray.Get(Index)->IP);
			strcpy(port, ACRNemaAddressArray.Get(Index)->Port);
			strcpy(compress, ACRNemaAddressArray.Get(Index)->Compress);
			AMapUnlock();
			return ( TRUE );
			}
		++Index;
		}

	AMapUnlock();
	return ( FALSE );
	}

// Same matching as GetACRNema (wildcard entry, exact, then case insensitive)
// but returns the whole entry from one map version: address, port,
// compression and options together. IP and port wildcards are filled in
// as in GetACRNema, cut to the field size.
BOOL
GetACRNemaEntryByAE(char* ACRNema, ACRNemaAddress *Entry)
	{
	UINT	Index;
	char	s[20];
	char	*t;
	BOOL	found = FALSE;

	memset(s, 0, 20);
	strncpy(s, ACRNema, 19);
	while (strlen(s) && __iswhitespacea(s[strlen(s)-1]))
		s[strlen(s)-1] = '\0';
	if (!strlen(s))
		return ( FALSE );

	AMapLock();
	for (Index=0; Index < ACRNemaAddressArray.GetSize() && !found; Index++)
		{
		t = ACRNemaAddressArray.Get(Index)->Name;
		if (t[0] && t[strlen(t)-1] == '*')
			{
			if (strlen(t)-1 <= strlen(s) && !memicmp(t, s, strlen(t)-1))
				{
				*Entry = *ACRNemaAddressArray.Get(Index);
				if (Entry->IP[0] && Entry->IP[strlen(Entry->IP)-1]=='*')
					snprintf(Entry->IP+strlen(Entry->IP)-1, sizeof(Entry->IP)-strlen(Entry->IP)+1, "%s", s+strlen(t)-1);
				if (Entry->Port[0] && Entry->Port[strlen(Entry->Port)-1]=='*')
					snprintf(Entry->Port+strlen(Entry->Port)-1, sizeof(Entry->Port)-strlen(Entry->Port)+1, "%s", s+strlen(t)-1);
				found = TRUE;
				break;
				}
			}
		if (!strcmp(t, s))
			{
			*Entry = *ACRNemaAddressArray.Get(Index);
			found = TRUE;
			}
		}
	for (Index=0; Index < ACRNemaAddressArray.GetSize() && !found; Index++)
		{
		if (!stricmp(ACRNemaAddressArray.Get(Index)->Name, s))
			{
			*Entry = *ACRNemaAddressArray.Get(Index);
			found = TRUE;
			}
		}
	AMapUnlock();
	return found;
	}

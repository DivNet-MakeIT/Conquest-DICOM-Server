/*
20010426	ljz	Added 'setsockopt' in 'Listen' and 'Open'
			Call 'shutdown' before closing a socket
20010720	ljz     Changed 'print' to 'fprint(stderr,...)'
20050116	mvh     Adapted for LINUX compile
20050119        mvh     Blocked out SOCKET END ERROR message
20070406	mvh	Use reentrant gethostbyname_r when available
20070415	mvh	Small fix there for windows
20090620	jf	Added unistd.h for UNIX
20090824	mvh	Debug output of adress connecting to socket
20091231	bcb	Changed char* to const char* for gcc4.2 warnings
20100111	mvh	Merged
20100122	mvh     Gethostbyname: fixed bug reported by Arash 2station would not be found
20100122	mvh     Added Poll() method: returns TRUE if data available
20100125	mvh     Linux warning
20100309	bcb     Commented out or inited to fix unused variable warning (gcc4.2 Warnings)
20100619	bcb     Fix gcc4 warnings, improve speed and made TimeOut local.
20100707	mvh     Merged
20121214        mvh     Set l_onoff to 0 (3x) to fix connection with some dicom systems
20130429        lsp     Removed unused FarProc
20140528        lsp     Kept member initialization only in constructors: not GNUC specific
20200617        mvh     Replaced UNIX version of SendBinary with luasocket inspired waiting code
20200618        mvh     Made send() timeout ~20s by increasingly longer nanosleep
20220922        mvh     Host names like 2.aap.mies are looked up; 1.2.3 taken as is
20250701	mvh	Added error messages and connection retries
20250702	mvh	Fixed connection retry for Linux; fix small mistake
20260912	mvh	Corrected dates in update
20261026	mmd	Outgoing TLS behind HAVE_OPENSSL (SetTLS before Open): handshake after
			connect with a time limit, full write loop, read uses data buffered in
			OpenSSL before select(), bounded close_notify. Never falls back to plain;
			without HAVE_OPENSSL a TLS connection is refused. Plain sockets unchanged.
20261026	mmd	Incoming TLS: TLSAccept (server handshake with a time limit on an
			accepted socket) and the ServerContext hook
20261026	mmd	TLS mode only: the TCP connect is non-blocking with a deadline of TimeOut
			(TCPIPTimeOut at the connect sites); plain connect unchanged
20261026	mmd	Windows constructor: TLS members set before the WSAStartup early return
20261026	mmd	TLS: the calls that make the socket non-blocking are checked; when one
			fails the TLS connect, handshake or accept fails (never a blocking wait)

*/

/****************************************************************************
          Copyright (C) 1995, University of California, Davis

          THIS SOFTWARE IS MADE AVAILABLE, AS IS, AND THE UNIVERSITY
          OF CALIFORNIA DOES NOT MAKE ANY WARRANTY ABOUT THE SOFTWARE, ITS
          PERFORMANCE, ITS MERCHANTABILITY OR FITNESS FOR ANY PARTICULAR
          USE, FREEDOM FROM ANY COMPUTER DISEASES OR ITS CONFORMITY TO ANY
          SPECIFICATION. THE ENTIRE RISK AS TO QUALITY AND PERFORMANCE OF
          THE SOFTWARE IS WITH THE USER.

          Copyright of the software and supporting documentation is
          owned by the University of California, and free access
          is hereby granted as a license to use this software, copy this
          software and prepare derivative works based upon this software.
          However, any distribution of this software source code or
          supporting documentation or derivative works (source code and
          supporting documentation) must include this copyright notice.
****************************************************************************/

/***************************************************************************
 *
 * University of California, Davis
 * UCDMC DICOM Network Transport Libraries
 * Version 0.1 Beta
 *
 * Technical Contact: mhoskin@ucdavis.edu
 *
 ***************************************************************************/

/*******************************************************************************
 *
 * socket.C
 *
 *******************************************************************************/

#	include	"dicom.hpp"
#	include	<stdlib.h>

#ifdef unix
#       include <unistd.h> //DUCKHEAD92
#endif

#	include	<stdarg.h>
#ifdef	HAVE_OPENSSL
#	include	<openssl/ssl.h>
#	include	<openssl/err.h>
#	include	<time.h>
#ifndef	WINDOWS
#	include	<fcntl.h>
#endif
#endif

// outgoing TLS hooks, set by the application (all NULL: TLS is refused)
SocketTLSHooks	SocketTLS = { NULL, NULL, NULL, NULL, NULL };

// TLS message to the application log (level 0 operator, 1 debug)
static	void	SocketTLSPrint(int level, const char *fmt, ...)
	{
	char	text[512];
	va_list	args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	if (SocketTLS.Log)
		SocketTLS.Log(level, text);
	else if (level==0)
		fprintf(stderr, "%s\n", text);
	}


#ifdef	WINDOWS
#define SD_BOTH         0x02		/* From winsock2.h */
static	WORD	WinSockUsed	=	0;
static	WSADATA	wsaData;



Socket	::	Socket ()
	{
	WORD		VersionNeeded;

	TLSHandle = NULL;	// first: the destructor tests it even when WSAStartup failed
	TLSMode = 0;
	TLSPeerAE[0] = 0;
	TimeOut = 60 * 5;	// 5 minutes	
	UDP = 0;
	if(! WinSockUsed )
		{
		/* WinSock.dll not WSAStartup(), so call it and init the library */
		VersionNeeded = 0x0101;
		
		Error = WSAStartup( VersionNeeded, &wsaData);
		if( Error )
			{
			/* no socket library */
			return;
			}
		/*FarProc = (FARPROC) SocketBlockingHook;
		if(!WSASetBlockingHook(FarProc))
			{
			OutputDebugString("Failed to Install Blocking Hook\n");
			}*/
		}
	++WinSockUsed;
	Socketfd = 0;
	ListenSocketfd = 0;
	Connected = 0;
	Listened = 0;
	}

Socket	::	~Socket ()
	{
	if ( TLSHandle )
		TLSFree(TRUE);

	if( ! WinSockUsed )
		return;	// must have failed installation
	
	if ( Socketfd )
		{
		shutdown(Socketfd, SD_BOTH); 
		closesocket(Socketfd);
		}
	if ( ListenSocketfd )
		{
		shutdown(ListenSocketfd, SD_BOTH); 
		closesocket(ListenSocketfd);
		}
	
	--WinSockUsed;
	if(! WinSockUsed )
		{
		//WSAUnhookBlockingHook();
		WSACleanup();
		}
	
	}
#else

#define SYSV_SOCKET

#	define	closesocket(xxx)	close(xxx)

/*
int	closesocket(int	sock)
	{
	int	flags;
	struct	linger	sl;

	sl.l_onoff=0; // 20121214 was 1
	sl.l_linger=30;
	errno=0;
	setsockopt(sock, SOL_SOCKET, SO_LINGER, &sl, sizeof(linger));
	fprintf(stderr, "%d\n", errno);

	errno = 0;
	fprintf(stderr, "CloseSocket(%d) : %d ", sock, close(sock));
	fprintf(stderr, "%d\n", errno);
	return(0);
	}
*/

Socket	::	Socket()
  : Error(0),
    tulong(0),
    tulongptr(NULL),
    Socketfd(0),
    ListenSocketfd(0),
    UDP(0),
    Connected(0),
    Listened(0),
    sa(),
    hes(),
    servs(),
    TimeOut(300) 	// 5 minutes
	{
	TLSHandle = NULL;
	TLSMode = 0;
	TLSPeerAE[0] = 0;
	}

Socket	::	~Socket()
	{
	if ( TLSHandle )
		TLSFree(TRUE);
	if ( Socketfd )
		{
		closesocket(Socketfd);
		}
	if ( ListenSocketfd )
		{
		closesocket(ListenSocketfd);
		}
	}

#endif
	
#ifndef gethostbyname_r
#define gethostbyname_r(name, a, b, c, d, e) *d=gethostbyname(name)
#endif


struct	hostent	*	Socket	::	Gethostbyname(char	*name1)
	{
	struct	hostent		*he;
	struct  hostent he2;
	unsigned	long	ip;
//	unsigned	long	i1,i2,i3,i4;
//	char		*dot2, *dot3, *dot4;
	char		*name;
	char		ndat[128], buf[64];
	int		dum;
	
	he2.h_length = 0;//Added for compiler unused warning. bcb
	buf[63] = 0;//Added for compiler unused warning. bcb
	dum = 0;//Added for compiler unused warning. bcb

	strcpy(ndat, name1);
	name = &ndat[0];
	
	// previous code failed for e.g. 2.aap.mies
	BOOL isname=false;
	for (int i=0; i<strlen(name1); i++)
		{
		if (name1[i]<'.' || name1[i]>'9') // outside ascii ./0123456789
			{
			isname = true;
			break;
			}
		}

	if(isname)
		gethostbyname_r(name, &he2, buf, 64, &he, &dum);
	else
		he = NULL;
	if(he)
		{
		memcpy ((void *) &hes, (void *) he, sizeof(struct hostent));
		return(&hes);
		}
#ifdef	MAC
	struct	in_addr 	ipa	= inet_addr(name);
	ip = ipa.s_addr;
#else
	ip = inet_addr(name);
#endif
	hes.h_addr_list = (char **) &tulongptr;
	tulongptr = &tulong;
	tulong = ip;
	return ( &hes );	
	}
	
struct	servent	*	Socket	::	Getservbyname(const char	*name, char	*prot)
	{
	unsigned	short	s;
//	unsigned	short	s1;
	struct	servent		*se;
	
	s = atoi(name);
	if(!s)
		{
		se = getservbyname(name, prot);
		if(!se)
			return(se);
		memcpy ((void *) &servs, (void *) se, sizeof(struct servent));
		return(&servs);
		}
#ifdef	MAC
	servs.s_port = s;
#else
	servs.s_port = htons(s);
#endif
	return ( &servs );
	}

BOOL	Socket	::	Listen ( char 	*port )
	{
	UINT32				tuint32;
	struct	servent		*se;
	struct	servent		secopy;
	struct	linger		Linger;

	Connected = 0;
	Listened = 0;

	if ( Socketfd )
		closesocket(Socketfd);
	
	se = Getservbyname(port, NULL);
	
	if ( !se )
		return ( FALSE );	// could not resolv port
	memcpy ((void *) &secopy, (void *) se, sizeof(struct servent));

	Socketfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(!Socketfd)
		return ( FALSE );	// could not create socket
	Linger.l_onoff  = 0; // 20121214 was 1
	Linger.l_linger = 0;
	setsockopt(Socketfd, SOL_SOCKET, SO_LINGER, (char*)&Linger, sizeof(struct linger));
	//memcpy((void *) &sa.sin_addr.S_un.S_addr , (void *) hecopy.h_addr_list[0], 4);	// adddress
	// should bind to the default address
	tuint32 = 0;
#ifdef	SYSV_SOCKET
	sa.sin_addr.s_addr = tuint32;
#else
	memcpy((void *) &sa.sin_addr.S_un.S_addr, (void *) &tuint32, 4);
#endif
	memcpy((void *) &sa.sin_port , (void *) &secopy.s_port, 2);			// port
	sa.sin_port = secopy.s_port;
	sa.sin_family = AF_INET;

	if(bind(Socketfd, (struct sockaddr *) &sa, sizeof(struct sockaddr_in)))
		{
//		fprintf(stderr, "Erro (bind) : %d\n", GetLinkError());
#ifdef	FORCEDIO
/*		while(errno==125)
			{
			errno=0;
			bind(Socketfd, (struct sockaddr *) &sa, sizeof(struct
				sockaddr_in));
			}*/
#endif
		if(GetLinkError())
			{
			closesocket(Socketfd);
			Error = GetLinkError();
			Socketfd = 0;
			return ( FALSE );
			}
		}
	if(listen(Socketfd, 5))
		{
		//fprintf(stderr, "Erro (bind): %d\n", GetLinkError());fflush(stderr);
		closesocket(Socketfd);
		Socketfd = 0;
		return ( FALSE );
		}

	Listened = 1;
	ListenSocketfd = Socketfd;
	Socketfd = 0;
	Connected = 0;
	return ( TRUE );
	}

BOOL	Socket	::	Accept()
	{
#ifdef	SYSV_SOCKET
	socklen_t	Size;
#else
	int		Size;
#endif

	//fprintf(stderr, "Accept()\n");fflush(stderr);
	if(!Listened)
		return ( FALSE );
	if(!ListenSocketfd)
		return ( FALSE );

	Size = sizeof(struct sockaddr_in);
	Socketfd = accept(ListenSocketfd, (struct sockaddr *) &sa, &Size);
	if(Socketfd>0)
		{
		Connected = TRUE;

DicomError(DCM_ERROR_DEBUG, "Connected by address: %08x", sa.sin_addr.s_addr);

		return ( TRUE );
		}
	//fprintf(stderr, "Error (accept) : %d\n", errno);
	closesocket(ListenSocketfd);
	Listened = 0;
	ListenSocketfd = 0;
	Socketfd = 0;
	Connected = FALSE;
	return ( FALSE );
	}
	

BOOL	Socket	::	Open ( char	*ip, char	*port)
	{
	struct	hostent		*he;
	struct	hostent		hecopy;
	struct	servent		*se;
	struct	servent		secopy;
	struct	linger		Linger;

	if ( TLSHandle )
		TLSFree(TRUE);
	if ( Socketfd )
		closesocket(Socketfd);

	Connected = 0;

#ifndef	HAVE_OPENSSL
	if ( TLSMode )
		{
		// the node needs TLS: never connect plain instead
		Socketfd = 0;
		SocketTLSPrint(0, "*** TLS: connection to %s (%s) refused: this dgate was built without TLS support", TLSPeerAE, ip);
		return ( FALSE );
		}
#else
	if ( TLSMode && !(SocketTLS.ClientContext && SocketTLS.ClientContext()) )
		{
		// TLS configuration not usable: do not even connect
		Socketfd = 0;
		SocketTLSPrint(0, "*** TLS: connection to %s (%s) refused: no usable TLS client configuration", TLSPeerAE, ip);
		return ( FALSE );
		}
#endif

	he = Gethostbyname(ip);
	
	if ( !he )
		{
	//	fprintf(stderr, "Could not resolve host\n");
		return ( FALSE );	// could not resolve host name
		}
	   
	memcpy ((void *) &hecopy, (void *) he, sizeof(struct hostent));
	
	se = Getservbyname(port, NULL);
	
	if ( !se )
		{
//		fprintf(stderr, "Could not resolve port\n");
		DicomError(DCM_ERROR_DEBUG, "Could not resolve port", 0);
		return ( FALSE );	// could not resolv port
		}
	
	memcpy ((void *) &secopy, (void *) se, sizeof(struct servent));
	if(!hecopy.h_addr_list)
		{
		DicomError(DCM_ERROR_DEBUG, "no h_addr_list", 0);
		return ( FALSE );
		}
	
	Socketfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(Socketfd<0)
		{
		DicomError(DCM_ERROR_DEBUG, "could not create socket", 0);
		return ( FALSE );	// could not create socket
		}
	Linger.l_onoff  = 0; // 20121214 was 1
	Linger.l_linger = 0;
	setsockopt(Socketfd, SOL_SOCKET, SO_LINGER, (char*)&Linger, sizeof(struct linger));
		
#ifdef	SYSV_SOCKET
	memcpy((void *) &sa.sin_addr.s_addr , (void *) hecopy.h_addr_list[0], 4);	// adddress
#else
	memcpy((void *) &sa.sin_addr.S_un.S_addr , (void *) hecopy.h_addr_list[0], 4);	// adddress
#endif
	memcpy((void *) &sa.sin_port , (void *) &secopy.s_port, 2);			// port
	sa.sin_family = AF_INET;
	sa.sin_port = secopy.s_port;

#ifdef	HAVE_OPENSSL
	// TLS mode only: all connect attempts together end at the deadline (a node that
	// does not answer would otherwise block for the kernel's connect timeout)
	long	ConnectEnd = (long) time(NULL) + (TimeOut > 0 ? TimeOut : 300);
#endif
	for (int i=0; i<5; i++)
		{
#ifdef	HAVE_OPENSSL
		if ( TLSMode )
			{
			int r = TLSConnect(ConnectEnd);
			if ( r == 2 )
				{
				SocketTLSPrint(0, "*** TLS: connection to %s (%s) timed out after %d s", TLSPeerAE, ip, TimeOut > 0 ? TimeOut : 300);
				break;
				}
			if ( r == 3 )
				{
				SocketTLSPrint(0, "*** TLS: connection to %s (%s) refused: the socket cannot be made non-blocking", TLSPeerAE, ip);
				break;
				}
			Error = r ? -1 : 0;
			}
		else
#endif
		Error = connect(Socketfd, (struct sockaddr *) &sa, sizeof(struct sockaddr_in));
		if( ! Error )
			{
			Connected = TRUE;
			if ( TLSMode && !TLSOpen(ip) )
				{
				// failed handshake or certificate check: no plain fallback
				closesocket(Socketfd);
				Socketfd = 0;
				Connected = FALSE;
				return ( FALSE );
				}
			return ( TRUE );
			}
#ifdef	WINDOWS
		Sleep(rand()&31);
#else
		struct timespec t;
		t.tv_sec =0;
		t.tv_nsec=(rand()&31)*1000000;
                nanosleep(&t, NULL);
#endif
		}

	closesocket(Socketfd);
	Socketfd = 0;
#ifdef	WINDOWS
	Sleep(10);
	DicomError(DCM_ERROR_DEBUG, "Socket connect error %d", WSAGetLastError());
#else
	DicomError(DCM_ERROR_DEBUG, "Socket connect error", 0);
#endif
	return ( FALSE );
	}

BOOL	Socket	::	Close()
	{

	if(TLSHandle)
		TLSFree(TRUE);

	if(ListenSocketfd)
		{
		closesocket(ListenSocketfd);
		}
	if(Socketfd)
		{
		closesocket(Socketfd);
		}
	ListenSocketfd = 0;
	Socketfd = 0;
	Connected = 0;
	Listened = 0;
	UDP = 0;
	return ( TRUE );
	}

BOOL	Socket	::	Poll(void)
	{
#ifdef	SYSV_SOCKET
	fd_set	fds;
#else
	struct	fd_set	fds;
#endif
	struct	timeval	tv	= {0, 0};	// return immediately

	if ( ! Connected )
		return ( FALSE );
#ifdef	HAVE_OPENSSL
	if ( TLSHandle && SSL_pending((SSL *)TLSHandle) > 0 )
		return ( TRUE );	// decrypted data waiting inside OpenSSL
#endif
#ifdef	MAC
	memset((void*)&fds, 0, sizeof(struct fd_set));
#else
	FD_ZERO(&fds);
#endif
	FD_SET(Socketfd, &fds);
	return (select(Socketfd + 1, &fds, NULL, NULL, &tv)==1);
	}

#ifdef	WINDOWS
BOOL		Socket	::	SendBinary(BYTE *s, UINT	count)
	{
	if ( !Connected)
		return ( 0 );
#ifdef	HAVE_OPENSSL
	if ( TLSHandle )
		return ( TLSSend(s, count) );
#endif
	
	if(send ( Socketfd, (char *) s, count, 0)<0)
		{
//		fprintf(stderr, "SOCKET END ERROR: %d\n", GetLastError());
		}
	return ( TRUE );
	}
#else
BOOL		Socket	::	SendBinary(BYTE *s, UINT	count)
	{
        struct timespec t;
	if ( !Connected)
		return ( 0 );
#ifdef	HAVE_OPENSSL
	if ( TLSHandle )
		return ( TLSSend(s, count) );
#endif
		
        t.tv_sec =0;
        t.tv_nsec=1000000;

	for (int i=1; i<200; i++)
		{
		long put = send ( Socketfd, (char *) s, count, 0);
		if (put>0)
			{
			return TRUE;
			}
		int err = errno;
		/* send can't really return 0, but EPIPE means the connection was closed */
		if (put == 0 || err == EPIPE) return FALSE;
		/* the call was interrupted, just try again */
		if (err == EINTR) continue;
		/* if failed fatal reason, report error */
		if (err != EAGAIN) return FALSE;
		/* wait until we can send something or we timeout */
                nanosleep(&t, NULL);
                t.tv_nsec+=1000000;
		}

	return ( FALSE );
	}
#endif
	
INT	Socket	::	ReadBinary(BYTE *s, UINT	count)
	{
#ifdef	SYSV_SOCKET
	fd_set	fds;
#else
	struct	fd_set	fds;
#endif
	struct	timeval	tv	= {TimeOut, 0};	// poll

	if ( ! Connected )
		return ( -1 );
#ifdef	HAVE_OPENSSL
	if ( TLSHandle )
		return ( TLSRead(s, count) );
#endif
#ifdef	MAC
	memset((void*)&fds, 0, sizeof(struct fd_set));
#else
	FD_ZERO(&fds);
#endif
	FD_SET(Socketfd, &fds);
	if(select(Socketfd + 1, &fds, NULL, NULL, &tv)==1)
		{
		if((count = recv(Socketfd, (char *) s, count, 0))==0)
			{
/*#ifdef	WINDOWS
			if(WSAGetLastError()==WSAEDISCON)
				return(-1);
			else
				return(0);
#else
			return ( -1 );
#endif
*/
			Close ();			
			return ( -1 );
			}
		return ( count );
		}
	if ( TimeOut )
		return ( -1 );
	return ( 0 );
	}
	
int		Socket	::	GetLinkError()
	{
#ifdef	WINDOWS
	return ( WSAGetLastError() );
#else
	return ( errno );
#endif
	}


BOOL	Socket	::	SetTimeOut(int	lTimeOut)
	{
	this->TimeOut = lTimeOut;
	return ( TRUE );
	}	

BOOL
Socket	::	BindUDPServer (
	char	*port)
	{
	Close();
	struct	servent		*se;
	struct	servent		secopy;
	se = Getservbyname(port, NULL);
	
	if ( !se )
		{
		return ( FALSE );	// could not resolv port
		}
	
	memcpy ((void *) &secopy, (void *) se, sizeof(struct servent));
	Socketfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(Socketfd<0)
		return ( FALSE );	// could not create socket
		
#ifdef	SYSV_SOCKET
	memset((void *) &sa.sin_addr.s_addr , 0, 4);
#else
	memset((void *) &sa.sin_addr.S_un.S_addr , 0, 4);
#endif
	memcpy((void *) &sa.sin_port , (void *) &secopy.s_port, 2);			// port
	sa.sin_family = AF_INET;
	sa.sin_port = secopy.s_port;
	
	if(bind(Socketfd, (struct sockaddr *) &sa, sizeof(struct sockaddr_in)))
		{
		closesocket(Socketfd);
		Error = GetLinkError();
		Socketfd = 0;
		return ( FALSE );
		}
	Connected = 1;
	UDP = 1;
	return ( TRUE );
	}

BOOL	
Socket	::	 BindUDPClient (
	char	*ip,
	const char	*port)
	{
	struct	hostent		*he;
	struct	hostent		hecopy;
	struct	servent		*se;
	struct	servent		secopy;
	

	if ( Socketfd )
		closesocket(Socketfd);
	if ( ListenSocketfd )
		closesocket(ListenSocketfd);
	
	Listened = 0;
	Connected = 0;
	Socketfd = 0;
	ListenSocketfd = 0;
	
	he = Gethostbyname(ip);
	
	if ( !he )
		{
		fprintf(stderr, "Could not resolve host\n");
		return ( FALSE );	// could not resolve host name
		}
	   
	memcpy ((void *) &hecopy, (void *) he, sizeof(struct hostent));
	
	se = Getservbyname(port, NULL);
	
	if ( !se )
		{
		fprintf(stderr, "Could not resolve port\n");
		return ( FALSE );	// could not resolv port
		}
	
	memcpy ((void *) &secopy, (void *) se, sizeof(struct servent));
	if(!hecopy.h_addr_list)
		return ( FALSE );
	
	Socketfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(Socketfd<0)
		return ( FALSE );	// could not create socket
		
#ifdef	SYSV_SOCKET
	memcpy((void *) &sa.sin_addr.s_addr , (void *) hecopy.h_addr_list[0], 4);	// adddress
#else
	memcpy((void *) &sa.sin_addr.S_un.S_addr , (void *) hecopy.h_addr_list[0], 4);	// adddress
#endif
	memcpy((void *) &sa.sin_port , (void *) &secopy.s_port, 2);			// port
	sa.sin_family = AF_INET;
	sa.sin_port = secopy.s_port;
	Error = connect(Socketfd, (struct sockaddr *) &sa, sizeof(struct sockaddr_in));
	if( ! Error )
		{
		Connected = TRUE;
		UDP = 1;
		return ( TRUE );
		}
	closesocket(Socketfd);
	Socketfd = 0;
	return ( FALSE );
	}

// Ask for TLS (on=1) or plain (on=0) on the next Open; AE is the node being
// connected (used for its certificate checks). Returns FALSE when TLS is asked
// for and this build has no TLS: Open will then refuse the connection.
BOOL	Socket	::	SetTLS(int on, const char *AE)
	{
	TLSMode = on ? 1 : 0;
	memset(TLSPeerAE, 0, sizeof(TLSPeerAE));
	if (AE)
		strncpy(TLSPeerAE, AE, sizeof(TLSPeerAE)-1);
#ifdef	HAVE_OPENSSL
	return ( TRUE );
#else
	return ( !TLSMode );
#endif
	}

#ifdef	HAVE_OPENSSL

#define	TLS_HANDSHAKE_SECONDS	30	// limit for the TLS handshake
#define	TLS_CLOSE_SECONDS	2	// limit for sending close_notify

// Make a TLS socket non-blocking. Every wait of the TLS code has a time limit only
// because of this mode, so a failure must end the connection: FALSE when the call
// (fcntl or ioctlsocket) fails.
static	BOOL	SocketSetNonBlocking(int fd)
	{
#ifdef	WINDOWS
	u_long	nb = 1;
	return ( ioctlsocket(fd, FIONBIO, &nb) == 0 );
#else
	int	fl = fcntl(fd, F_GETFL, 0);
	if (fl == -1)
		return ( FALSE );
	return ( fcntl(fd, F_SETFL, fl | O_NONBLOCK) != -1 );
#endif
	}

// Wait until the socket can be read (ForWrite=0) or written (ForWrite=1).
// Returns 1 when ready, 0 on timeout, -1 on error.
int	Socket	::	TLSWait(int ForWrite, int Seconds)
	{
	fd_set		fds;
	struct	timeval	tv;
	time_t		end = time(NULL) + Seconds;
	int		left, r;

	for (;;)
		{
		left = (int)(end - time(NULL));
		if (left < 0) left = 0;
		tv.tv_sec = left;
		tv.tv_usec = 0;
		FD_ZERO(&fds);
		FD_SET(Socketfd, &fds);
		r = select(Socketfd + 1, ForWrite ? NULL : &fds, ForWrite ? &fds : NULL, NULL, &tv);
		if (r > 0) return 1;
		if (r == 0) return 0;
#ifndef	WINDOWS
		if (errno == EINTR && left > 0) continue;
#endif
		return -1;
		}
	}

// Text of the last TLS error, with the certificate verification result if that failed
static	void	SocketTLSErrorText(SSL *ssl, int e, char *buf, int len)
	{
	long	v = SSL_get_verify_result(ssl);
	unsigned long	err = ERR_peek_last_error();

	if (v != X509_V_OK)
		snprintf(buf, len, "certificate verification failed: %s", X509_verify_cert_error_string(v));
	else if (err)
		ERR_error_string_n(err, buf, len);
	else if (e == SSL_ERROR_ZERO_RETURN)
		snprintf(buf, len, "closed by peer");
	else
		snprintf(buf, len, "TLS error %d (connection closed or reset)", e);
	}

// TCP connect for TLS mode: non-blocking, ends at time end (seconds since the
// epoch). Returns 0 connected, 1 failed (e.g. refused; the caller may retry),
// 2 timed out, 3 the socket could not be made non-blocking (no retry). The socket
// stays non-blocking (TLSOpen needs that anyway).
int	Socket	::	TLSConnect(long end)
	{
	fd_set		wfds, efds;
	struct	timeval	tv;
	int		r, err, left;
#ifdef	WINDOWS
	int		len = sizeof(err);
#else
	socklen_t	len = sizeof(err);
#endif

	if (!SocketSetNonBlocking(Socketfd))
		return 3;
	r = connect(Socketfd, (struct sockaddr *) &sa, sizeof(struct sockaddr_in));
	if (r == 0)
		return 0;
#ifdef	WINDOWS
	if (WSAGetLastError() != WSAEWOULDBLOCK)
		return 1;
#else
	if (errno != EINPROGRESS && errno != EINTR)
		return 1;
#endif
	for (;;)
		{
		left = (int)(end - (long) time(NULL));
		if (left <= 0)
			return 2;
		tv.tv_sec = left;
		tv.tv_usec = 0;
		FD_ZERO(&wfds);
		FD_SET(Socketfd, &wfds);
		FD_ZERO(&efds);
		FD_SET(Socketfd, &efds);		// Windows reports a failed connect here
		r = select(Socketfd + 1, NULL, &wfds, &efds, &tv);
		if (r == 0)
			return 2;
		if (r < 0)
			{
#ifndef	WINDOWS
			if (errno == EINTR) continue;
#endif
			return 1;
			}
		err = 0;
		if (getsockopt(Socketfd, SOL_SOCKET, SO_ERROR, (char *) &err, &len) != 0 || err != 0)
			return 1;
		return 0;
		}
	}

// TLS handshake on the connected socket (client side), with a time limit and
// the application's certificate checks. On failure the SSL object is freed and
// FALSE returned; the caller closes the socket.
BOOL	Socket	::	TLSOpen(char *host)
	{
	SSL_CTX	*ctx;
	SSL	*ssl;
	char	why[256];
	int	limit, r, e, w, left;
	time_t	end;
	BOOL	isname = FALSE;

	ctx = SocketTLS.ClientContext ? (SSL_CTX *) SocketTLS.ClientContext() : NULL;
	if (!ctx)
		{
		SocketTLSPrint(0, "*** TLS: connection to %s (%s) refused: no usable TLS client configuration", TLSPeerAE, host);
		return ( FALSE );
		}
	ssl = SSL_new(ctx);
	if (!ssl)
		{
		SocketTLSPrint(0, "*** TLS: connection to %s (%s) refused: SSL_new failed", TLSPeerAE, host);
		return ( FALSE );
		}

	// non-blocking from here: every wait below has a time limit
	if (!SocketSetNonBlocking(Socketfd))
		{
		SocketTLSPrint(0, "*** TLS: connection to %s (%s) refused: the socket cannot be made non-blocking", TLSPeerAE, host);
		SSL_free(ssl);
		return ( FALSE );
		}
	SSL_set_fd(ssl, Socketfd);
	SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	TLSHandle = ssl;

	// server name indication only for host names, not for IP addresses
	for (int i=0; host[i]; i++)
		if ((host[i]<'0' || host[i]>'9') && host[i]!='.' && host[i]!=':')
			isname = TRUE;
	if (isname)
		SSL_set_tlsext_host_name(ssl, host);

	if (SocketTLS.BeforeHandshake && !SocketTLS.BeforeHandshake(ssl, TLSPeerAE, host))
		{
		SSL_set_quiet_shutdown(ssl, 1);
		TLSFree(FALSE);
		return ( FALSE );
		}

	limit = TLS_HANDSHAKE_SECONDS;
	if (TimeOut > 0 && TimeOut < limit)
		limit = TimeOut;
	end = time(NULL) + limit;
	for (;;)
		{
		ERR_clear_error();
		r = SSL_connect(ssl);
		if (r == 1)
			break;
		e = SSL_get_error(ssl, r);
		if (e == SSL_ERROR_WANT_READ) w = 0;
		else if (e == SSL_ERROR_WANT_WRITE) w = 1;
		else
			{
			SocketTLSErrorText(ssl, e, why, sizeof(why));
			SocketTLSPrint(0, "*** TLS: handshake with %s (%s) failed: %s", TLSPeerAE, host, why);
			SSL_set_quiet_shutdown(ssl, 1);
			TLSFree(FALSE);
			return ( FALSE );
			}
		left = (int)(end - time(NULL));
		if (left <= 0 || TLSWait(w, left) <= 0)
			{
			SocketTLSPrint(0, "*** TLS: handshake with %s (%s) timed out after %d s", TLSPeerAE, host, limit);
			SSL_set_quiet_shutdown(ssl, 1);
			TLSFree(FALSE);
			return ( FALSE );
			}
		}

	if (SocketTLS.AfterHandshake && !SocketTLS.AfterHandshake(ssl, TLSPeerAE, host))
		{
		TLSFree(TRUE);
		return ( FALSE );
		}

	SocketTLSPrint(1, "TLS: connected to %s (%s) with %s %s", TLSPeerAE, host, SSL_get_version(ssl), SSL_get_cipher_name(ssl));
	return ( TRUE );
	}

// Server side TLS handshake on the accepted socket fd (incoming TLS port), with
// the same time limit as TLSOpen: 30 s, or TimeOut if that is shorter. Nothing
// is read from the connection before it succeeds. On success the SSL object is
// kept in TLSHandle and Socketfd = fd (Connected is not changed); on failure the
// SSL object is freed, fd is closed and FALSE returned.
BOOL	Socket	::	TLSAccept(int fd, const char *peer)
	{
	SSL_CTX	*ctx;
	SSL	*ssl;
	char	why[256];
	int	limit, r, e, w, left;
	time_t	end;

	if ( TLSHandle )
		TLSFree(FALSE);
	TLSMode = 0;			// TLSMode only steers Open (outgoing)
	TLSPeerAE[0] = 0;		// calling AE: not known before the A-ASSOCIATE-RQ
	Socketfd = fd;

	ctx = SocketTLS.ServerContext ? (SSL_CTX *) SocketTLS.ServerContext() : NULL;
	ssl = ctx ? SSL_new(ctx) : NULL;
	if (!ssl)
		{
		SocketTLSPrint(0, "*** TLS: incoming connection from %s refused: no usable TLS server configuration", peer);
		closesocket(fd);
		Socketfd = 0;
		return ( FALSE );
		}

	// non-blocking from here: every wait below has a time limit
	if (!SocketSetNonBlocking(Socketfd))
		{
		SocketTLSPrint(0, "*** TLS: incoming connection from %s refused: the socket cannot be made non-blocking", peer);
		SSL_free(ssl);
		closesocket(fd);
		Socketfd = 0;
		return ( FALSE );
		}
	SSL_set_fd(ssl, Socketfd);
	SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	TLSHandle = ssl;

	limit = TLS_HANDSHAKE_SECONDS;
	if (TimeOut > 0 && TimeOut < limit)
		limit = TimeOut;
	end = time(NULL) + limit;
	for (;;)
		{
		ERR_clear_error();
		r = SSL_accept(ssl);
		if (r == 1)
			break;
		e = SSL_get_error(ssl, r);
		if (e == SSL_ERROR_WANT_READ) w = 0;
		else if (e == SSL_ERROR_WANT_WRITE) w = 1;
		else
			{
			SocketTLSErrorText(ssl, e, why, sizeof(why));
			SocketTLSPrint(0, "*** TLS: incoming handshake from %s failed: %s", peer, why);
			SSL_set_quiet_shutdown(ssl, 1);
			TLSFree(FALSE);
			closesocket(fd);
			Socketfd = 0;
			return ( FALSE );
			}
		left = (int)(end - time(NULL));
		if (left <= 0 || TLSWait(w, left) <= 0)
			{
			SocketTLSPrint(0, "*** TLS: incoming handshake from %s timed out after %d s", peer, limit);
			SSL_set_quiet_shutdown(ssl, 1);
			TLSFree(FALSE);
			closesocket(fd);
			Socketfd = 0;
			return ( FALSE );
			}
		}

	SocketTLSPrint(1, "TLS: incoming connection from %s with %s %s", peer, SSL_get_version(ssl), SSL_get_cipher_name(ssl));
	return ( TRUE );
	}

// Free the TLS state; optionally send one close_notify first, waiting at most
// TLS_CLOSE_SECONDS and never for the peer's answer. The socket stays open.
void	Socket	::	TLSFree(BOOL SendCloseNotify)
	{
	SSL	*ssl = (SSL *) TLSHandle;
	int	r;

	if (!ssl)
		return;
	if (SendCloseNotify && Socketfd)
		{
		ERR_clear_error();
		r = SSL_shutdown(ssl);
		if (r < 0 && SSL_get_error(ssl, r) == SSL_ERROR_WANT_WRITE && TLSWait(1, TLS_CLOSE_SECONDS) > 0)
			{
			ERR_clear_error();
			SSL_shutdown(ssl);
			}
		}
	SSL_free(ssl);
	TLSHandle = NULL;
	ERR_clear_error();
	}

// Send all count bytes over TLS. Returns FALSE when the connection fails or
// makes no progress for TimeOut seconds (300 when not set).
BOOL	Socket	::	TLSSend(BYTE *s, UINT count)
	{
	SSL	*ssl = (SSL *) TLSHandle;
	UINT	done = 0, part;
	int	limit, n, e, w, left;
	time_t	end;
	char	why[256];

	limit = TimeOut > 0 ? TimeOut : 300;
	end = time(NULL) + limit;
	while (done < count)
		{
		part = count - done;
		if (part > 0x40000000) part = 0x40000000;
		ERR_clear_error();
		n = SSL_write(ssl, s + done, (int) part);
		if (n > 0)
			{
			done += n;
			end = time(NULL) + limit;
			continue;
			}
		e = SSL_get_error(ssl, n);
		if (e == SSL_ERROR_WANT_WRITE) w = 1;
		else if (e == SSL_ERROR_WANT_READ) w = 0;
		else
			{
			SocketTLSErrorText(ssl, e, why, sizeof(why));
			SocketTLSPrint(0, "*** TLS: send to %s failed: %s", TLSPeerAE, why);
			SSL_set_quiet_shutdown(ssl, 1);
			return ( FALSE );
			}
		left = (int)(end - time(NULL));
		if (left <= 0 || TLSWait(w, left) <= 0)
			{
			SocketTLSPrint(0, "*** TLS: send to %s timed out", TLSPeerAE);
			SSL_set_quiet_shutdown(ssl, 1);
			return ( FALSE );
			}
		}
	return ( TRUE );
	}

// Read up to count bytes over TLS, with the same results as the plain
// ReadBinary: >0 bytes read, -1 closed/error/timeout, 0 nothing (TimeOut 0).
// SSL_read is tried first, so data that OpenSSL already holds (SSL_pending)
// is returned without waiting in select().
INT	Socket	::	TLSRead(BYTE *s, UINT count)
	{
	SSL	*ssl = (SSL *) TLSHandle;
	int	n, e, w, left;
	time_t	end = time(NULL) + TimeOut;
	char	why[256];

	if (count > 0x40000000) count = 0x40000000;
	for (;;)
		{
		ERR_clear_error();
		n = SSL_read(ssl, s, (int) count);
		if (n > 0)
			return ( n );
		e = SSL_get_error(ssl, n);
		if (e == SSL_ERROR_WANT_READ) w = 0;
		else if (e == SSL_ERROR_WANT_WRITE) w = 1;
		else
			{
			// closed by the peer or TLS error: as the plain code, close
			if (e != SSL_ERROR_ZERO_RETURN)
				{
				SocketTLSErrorText(ssl, e, why, sizeof(why));
				SocketTLSPrint(1, "TLS: read from %s ended: %s", TLSPeerAE, why);
				SSL_set_quiet_shutdown(ssl, 1);
				}
			Close ();
			return ( -1 );
			}
		if ( !TimeOut )
			return ( 0 );
		left = (int)(end - time(NULL));
		if (left <= 0 || TLSWait(w, left) <= 0)
			return ( -1 );
		}
	}

#else

// builds without HAVE_OPENSSL: TLS is never active (Open refuses TLSMode)
BOOL	Socket	::	TLSOpen(char *host)		{ return ( FALSE ); }
int	Socket	::	TLSConnect(long end)		{ return ( 1 ); }
BOOL	Socket	::	TLSAccept(int fd, const char *peer)
	{
	SocketTLSPrint(0, "*** TLS: incoming connection from %s refused: this dgate was built without TLS support", peer);
	closesocket(fd);
	Socketfd = 0;
	return ( FALSE );
	}
void	Socket	::	TLSFree(BOOL SendCloseNotify)	{ TLSHandle = NULL; }
int	Socket	::	TLSWait(int ForWrite, int Seconds)	{ return ( -1 ); }
BOOL	Socket	::	TLSSend(BYTE *s, UINT count)	{ return ( FALSE ); }
INT	Socket	::	TLSRead(BYTE *s, UINT count)	{ return ( -1 ); }

#endif
	
/*#endif*/


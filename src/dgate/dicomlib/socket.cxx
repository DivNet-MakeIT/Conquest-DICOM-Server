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
20261026	mmd	SetTLS before Open: a node that needs DICOM TLS (T in acrnema.map) is
			refused (this variant has no TLS; never a plain connection instead)
20261026	mmd	ConnectTimeOut (0 = as before): when set, all connect attempts end at that
			limit (non-blocking connect, then blocking again), and ConnectTimedOut
			tells a timeout
20261027	mmd	Deadline (0 = as before): an absolute end for the connect attempts, reads
			and writes; past it they fail at once
20261027	mmd	Unix: the ConnectTimeOut connect waits with poll() (no FD_SETSIZE limit); Poll
			and ReadBinary never put a descriptor >= FD_SETSIZE in an fd_set (Poll: poll();
			ReadBinary: console line, closed)
20261027	mmd	Plain connect: bounded (non-blocking, SocketConnectBefore) also when only Deadline is set
20261027	mmd	Deadline bounds plain sends: Unix SocketSendBefore (MSG_DONTWAIT, poll() for writability,
			partial writes counted, FALSE at the deadline); Windows checks the deadline before the send
20261027	mmd	Windows: Deadline bounds plain sends too (SocketSendBefore: FIONBIO for the send, partial
			writes counted, select() for writability cut to the deadline, blocking again afterwards)

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
#ifndef	WINDOWS
#	include	<poll.h>
#endif

// message hook, set by the application (NULL: operator messages go to stderr)
void	(*SocketLog)(int level, const char *text) = NULL;


// message to the application log (level 0 operator, 1 debug)
static	void	SocketPrint(int level, const char *fmt, ...)
	{
	char	text[512];
	va_list	args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	if (SocketLog)
		SocketLog(level, text);
	else if (level==0)
		fprintf(stderr, "%s\n", text);
	}

// Wait at most Seconds for fd: readable (ForWrite=0), or writable or failed (ForWrite=1, e.g. a
// non-blocking connect). Unix: poll(), which has no limit on the descriptor number (select()
// with an fd_set cannot take a descriptor >= FD_SETSIZE); Windows: select() (its fd_set is a
// list of sockets, not a bitmap). Returns 1 ready, 0 timeout, -1 error (errno / WSA error).
static	int	SocketWaitFd(int fd, int ForWrite, int Seconds)
	{
	if (Seconds < 0) Seconds = 0;
	if (Seconds > 2000000) Seconds = 2000000;
#ifdef	WINDOWS
	fd_set		fds, efds;
	struct	timeval	tv;
	int		r;
	tv.tv_sec = Seconds;
	tv.tv_usec = 0;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	FD_ZERO(&efds);
	FD_SET(fd, &efds);		// Windows reports a failed connect here
	r = select(fd + 1, ForWrite ? NULL : &fds, ForWrite ? &fds : NULL, ForWrite ? &efds : NULL, &tv);
	return r > 0 ? 1 : r;
#else
	struct	pollfd	p;
	int		r;
	p.fd = fd;
	p.events = ForWrite ? POLLOUT : POLLIN;
	p.revents = 0;
	r = poll(&p, 1, Seconds * 1000);
	return r > 0 ? 1 : r;		// POLLERR/POLLHUP count as ready: the next call reports them
#endif
	}

// Upstream select() paths: TRUE when fd cannot be put in an fd_set (Unix, fd >= FD_SETSIZE;
// FD_SET would write outside the set)
static	BOOL	SocketBeyondFdSet(int fd)
	{
#ifdef	WINDOWS
	return ( FALSE );
#else
	return ( fd < 0 || fd >= FD_SETSIZE );
#endif
	}


#ifdef	WINDOWS
#define SD_BOTH         0x02		/* From winsock2.h */
static	WORD	WinSockUsed	=	0;
static	WSADATA	wsaData;



Socket	::	Socket ()
	{
	WORD		VersionNeeded;

	TLSMode = 0;
	TLSPeerAE[0] = 0;
	ConnectTimeOut = 0;
	ConnectTimedOut = FALSE;
	Deadline = 0;
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
	TLSMode = 0;
	TLSPeerAE[0] = 0;
	ConnectTimeOut = 0;
	ConnectTimedOut = FALSE;
	Deadline = 0;
	}

Socket	::	~Socket()
	{
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
	

#ifndef	WINDOWS
#	include	<fcntl.h>
#	include	<errno.h>
#endif
#	include	<time.h>

// Plain TCP connect that ends at time end (seconds since the epoch); only used with
// ConnectTimeOut or Deadline. Non-blocking connect, then the socket is blocking again (the plain
// reads and writes expect that). Returns 0 connected, 1 failed (e.g. refused; the
// caller may retry), 2 timed out.
static	int	SocketConnectBefore(int fd, struct sockaddr_in *sa, long end)
	{
	int		r, err, left, result;
#ifdef	WINDOWS
	int		len = sizeof(err);
	u_long		nb = 1;
	if (ioctlsocket(fd, FIONBIO, &nb) != 0)
		return 1;
#else
	socklen_t	len = sizeof(err);
	int		fl = fcntl(fd, F_GETFL, 0);
	if (fl == -1 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) == -1)
		return 1;
#endif
	r = connect(fd, (struct sockaddr *) sa, sizeof(struct sockaddr_in));
	result = 0;
#ifdef	WINDOWS
	if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK)
		result = 1;
#else
	if (r != 0 && errno != EINPROGRESS && errno != EINTR)
		result = 1;
#endif
	while (r != 0 && result == 0)
		{
		left = (int)(end - (long) time(NULL));
		if (left <= 0)
			{ result = 2; break; }
		r = SocketWaitFd(fd, 1, left);
		if (r == 0)
			{ result = 2; break; }
		if (r < 0)
			{
#ifndef	WINDOWS
			if (errno == EINTR) { r = -1; continue; }
#endif
			result = 1;
			break;
			}
		err = 0;
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &err, &len) != 0 || err != 0)
			result = 1;
		break;
		}
#ifdef	WINDOWS
	nb = 0;
	if (ioctlsocket(fd, FIONBIO, &nb) != 0 && result == 0)
		result = 1;
#else
	if (fcntl(fd, F_SETFL, fl) == -1 && result == 0)
		result = 1;
#endif
	return result;
	}

BOOL	Socket	::	Open ( char	*ip, char	*port)
	{
	struct	hostent		*he;
	struct	hostent		hecopy;
	struct	servent		*se;
	struct	servent		secopy;
	struct	linger		Linger;

	if ( Socketfd )
		closesocket(Socketfd);

	Connected = 0;
	ConnectTimedOut = FALSE;

	if ( TLSMode )
		{
		// the node needs DICOM TLS (T in acrnema.map): never connect plain instead
		Socketfd = 0;
		SocketPrint(0, "*** connection to %s (%s) refused: T in acrnema.map (DICOM TLS), and this dgate has no TLS", TLSPeerAE, ip);
		return ( FALSE );
		}

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

	// all connect attempts together end at the deadline, only with ConnectTimeOut or Deadline
	// (a node that does not answer would otherwise block for the kernel's connect timeout,
	// per attempt)
	int	ConnectLimit = ConnectTimeOut > 0 ? ConnectTimeOut : (TimeOut > 0 ? TimeOut : 300);
	long	ConnectEnd = (long) time(NULL) + ConnectLimit;
	if ( Deadline > 0 && Deadline < ConnectEnd )
		ConnectEnd = Deadline;		// the overall deadline comes first
	for (int i=0; i<5; i++)
		{
		if ( ConnectTimeOut > 0 || Deadline > 0 )
			{
			int r = SocketConnectBefore(Socketfd, &sa, ConnectEnd);
			if ( r == 2 )
				{
				ConnectTimedOut = TRUE;
				break;
				}
			Error = r ? -1 : 0;
			}
		else
		Error = connect(Socketfd, (struct sockaddr *) &sa, sizeof(struct sockaddr_in));
		if( ! Error )
			{
			Connected = TRUE;
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
	if ( SocketBeyondFdSet(Socketfd) )
		return ( Socketfd >= 0 && SocketWaitFd(Socketfd, 0, 0) > 0 );	// cannot be in an fd_set
#ifdef	MAC
	memset((void*)&fds, 0, sizeof(struct fd_set));
#else
	FD_ZERO(&fds);
#endif
	FD_SET(Socketfd, &fds);
	return (select(Socketfd + 1, &fds, NULL, NULL, &tv)==1);
	}

#ifdef	WINDOWS
// Windows, plain, Deadline set: send all count bytes by the deadline (as SocketSendBefore on Unix).
// Windows has no MSG_DONTWAIT: the socket is non-blocking (ioctlsocket FIONBIO) for this send only
// and blocking again afterwards (the other plain paths expect that; one association is used by one
// thread at a time). Partial writes are counted; while the send buffer is full (WSAEWOULDBLOCK) the
// wait for writability is select() cut to the deadline (SocketWaitFd; a Windows fd_set is a list of
// sockets, no descriptor-value limit). FALSE when the deadline passes first, the connection fails,
// or the mode cannot be set or restored.
static	BOOL	SocketSendBefore(int fd, BYTE *s, UINT count, long end)
	{
	u_long	nb = 1;
	UINT	done = 0;
	BOOL	ok = TRUE;
	if (ioctlsocket(fd, FIONBIO, &nb) != 0)
		return ( FALSE );
	while (done < count)
		{
		long left = end - (long) time(NULL);
		if (left <= 0)
			{ ok = FALSE; break; }
		int put = send(fd, (char *) s + done, (int) (count - done), 0);
		if (put > 0)
			{
			done += (UINT) put;
			continue;
			}
		if (put == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
			{ ok = FALSE; break; }
		if (SocketWaitFd(fd, 1, (int) left) < 0)
			{ ok = FALSE; break; }
		}
	nb = 0;
	if (ioctlsocket(fd, FIONBIO, &nb) != 0)
		ok = FALSE;
	return ( ok );
	}

// Windows: with Deadline set the send is bounded by it (SocketSendBefore); without, as upstream
// (blocking send).
BOOL		Socket	::	SendBinary(BYTE *s, UINT	count)
	{
	if ( !Connected)
		return ( 0 );
	if ( Deadline > 0 )
		return ( SocketSendBefore(Socketfd, s, count, Deadline) );

	if(send ( Socketfd, (char *) s, count, 0)<0)
		{
//		fprintf(stderr, "SOCKET END ERROR: %d\n", GetLastError());
		}
	return ( TRUE );
	}
#else
// Unix, plain, Deadline set: send all count bytes by the deadline. Each send() is non-blocking
// (MSG_DONTWAIT; the socket itself stays blocking for the other paths), partial writes are
// counted, and while the send buffer is full the wait for writability is poll() cut to the
// deadline. FALSE when the deadline passes first or the connection fails: a peer that stops
// reading cannot hold the sender past the deadline.
static	BOOL	SocketSendBefore(int fd, BYTE *s, UINT count, long end)
	{
	UINT	done = 0;
	while (done < count)
		{
		long left = end - (long) time(NULL);
		if (left <= 0)
			return ( FALSE );
		long put = send(fd, (char *) s + done, count - done, MSG_DONTWAIT);
		if (put > 0)
			{
			done += (UINT) put;
			continue;
			}
		int err = errno;
		if (put == 0 || err == EPIPE)
			return ( FALSE );
		if (err == EINTR)
			continue;
		if (err != EAGAIN && err != EWOULDBLOCK)
			return ( FALSE );
		if (SocketWaitFd(fd, 1, (int) left) < 0 && errno != EINTR)
			return ( FALSE );
		}
	return ( TRUE );
	}

BOOL		Socket	::	SendBinary(BYTE *s, UINT	count)
	{
        struct timespec t;
	if ( !Connected)
		return ( 0 );
	if ( Deadline > 0 )
		return ( SocketSendBefore(Socketfd, s, count, Deadline) );
		
        t.tv_sec =0;
        t.tv_nsec=1000000;

	for (int i=1; i<200; i++)
		{
		if ( Deadline > 0 && (long) time(NULL) >= Deadline )
			return FALSE;		// past the overall deadline
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
	if ( SocketBeyondFdSet(Socketfd) )
		{
		SocketPrint(0, "*** socket descriptor %d is not below FD_SETSIZE (%d): connection closed", Socketfd, (int) FD_SETSIZE);
		Close ();
		return ( -1 );
		}
	if ( Deadline > 0 && TimeOut )
		{
		long left = Deadline - (long) time(NULL);
		if ( left <= 0 )
			return ( -1 );		// past the overall deadline: no more waiting
		if ( left < (long) tv.tv_sec )
			tv.tv_sec = left;
		}
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

// Mark the node of the next Open as one that needs DICOM TLS (on=1, on=2 with L) or not (on=0);
// AE is the node being connected (for the message). This variant has no TLS: Open refuses a
// node that needs it. Returns FALSE when on is set (the connection will be refused).
BOOL	Socket	::	SetTLS(int on, const char *AE)
	{
	TLSMode = on == 2 ? 2 : on ? 1 : 0;
	memset(TLSPeerAE, 0, sizeof(TLSPeerAE));
	if (AE)
		strncpy(TLSPeerAE, AE, sizeof(TLSPeerAE)-1);
	return ( !TLSMode );
	}

	
/*#endif*/


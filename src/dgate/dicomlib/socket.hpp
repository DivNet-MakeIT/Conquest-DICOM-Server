// 20091231 bcb     Changed char* to const char* for gcc4.2 warnings
// 20100111 mvh     Merged
// 20100122 mvh     Added Poll function
// 20100619 bcb     Added no-copy to the Socket class.
// 20100707 mvh     Merged
// 20121214 mvh     Mode sa public
// 20140528 lsp     Private copy constructor is not GNUC specific
// 20261026 mmd     Outgoing TLS members and hooks (used only with HAVE_OPENSSL)
// 20261026 mmd     Incoming TLS: TLSAccept and the ServerContext hook
// 20261026 mmd     TLSMode 2 (legacy profile) and the ClientContextLegacy hook
// 20261026 mmd     TLSAccept with the legacy server context (ServerContextLegacy hook)
// 20261026 mmd     ConnectTimeOut / ConnectTimedOut (connect time limit, 0 = as before)

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

/*********************************************************************
 *
 * Connection Class
 *
 *
 *********************************************************************/

// The library used to link with the DICOM Lib has a naming conflict with
// socket.  Therefor we simply remap our socket class into MacSocket.
 
#ifdef	MAC
#	define	Socket	MacSocket
#endif

class	Socket
	{
	int					Error;
	unsigned	long	tulong,*tulongptr;
	public:
		int					Socketfd;
		int					ListenSocketfd;
		BOOL				UDP;
		BOOL				Connected;
		BOOL				Listened;
	private:
	struct	hostent		hes;
	struct	servent		servs;

		int					TimeOut;					
	public:

	  struct	sockaddr_in	sa;
		virtual	BOOL	SendBinary(BYTE *, UINT);
		virtual	INT	ReadBinary(BYTE *, UINT);
		virtual	BOOL	Poll(void);
		struct	hostent	*Gethostbyname(char	*);
		struct	servent	*Getservbyname(const char	*, char *);
		int		GetLinkError();
				Socket();
		virtual	~Socket();		
		virtual	BOOL	Open ( char *ip, char *port );
		BOOL	Close ();
		int		GetLastError()	{ return ( Error ); };
		BOOL	Listen (char *port);
		BOOL	Accept ();
		BOOL	SetTimeOut(int);
		// Connect time limit (20261026 mmd). 0 (the default) = as before. > 0: all connect
		// attempts of Open together end after this many seconds, plain or TLS, and the TLS
		// handshake ends after at most this many seconds. Set only by the outgoing storage
		// commitment worker. ConnectTimedOut: the last Open failed because one of these limits
		// ran out (a node that does not answer).
		int	ConnectTimeOut;
		BOOL	ConnectTimedOut;
		
		// UDP Extensions
		BOOL	BindUDPServer(char *port);
		BOOL	BindUDPClient(char *host, const char *port);

		// Outgoing TLS (20261026 mmd). TLSMode 0 = plain, exactly as before.
		// TLSMode 1 = the next Open makes a TLS client connection, or fails:
		// it never falls back to plain. Without HAVE_OPENSSL Open refuses it.
		// TLSMode 2 = as 1, with the legacy client context (map option L).
		void	*TLSHandle;		// SSL* while TLS is active, else NULL (void*: no OpenSSL headers needed here)
		int	TLSMode;
		char	TLSPeerAE[20];		// node being connected, for the certificate checks
		BOOL	SetTLS(int on, const char *AE);
		// Incoming TLS (20261026 mmd): server side handshake on an accepted socket fd,
		// with a time limit, before any DICOM data is read. On success the socket
		// becomes this object's TLS connection (Connected is left as it was, so
		// PDU_Service::Multiplex(fd) can take it over); on failure fd is closed.
		// peer = text for messages (the caller's IP). Without HAVE_OPENSSL: FALSE.
		// Legacy = 1: the legacy server context (TLSLegacyPort).
		BOOL	TLSAccept(int fd, const char *peer, int Legacy = 0);
	private:
		BOOL	TLSOpen(char *host);
		int	TLSConnect(long end);
		void	TLSFree(BOOL SendCloseNotify);
		int	TLSWait(int ForWrite, int Seconds);
		BOOL	TLSSend(BYTE *, UINT);
		INT	TLSRead(BYTE *, UINT);
	private:// This will prevent it from being copied (it has a pointer)
		Socket(const Socket&);
		const	Socket & operator = (const Socket&);
	};

// Hooks for outgoing TLS, installed by the application (dgate). With no
// ClientContext hook a TLS connection is refused. Only used with HAVE_OPENSSL.
//   ClientContext:   returns the SSL_CTX* for client connections, or NULL
//   BeforeHandshake: called with the new SSL* (e.g. to set the host name check)
//   AfterHandshake:  called after a good handshake (e.g. certificate pin); FALSE aborts
//   Log:             level 0 = operator message, 1 = debug
//   ServerContext:   returns the SSL_CTX* for incoming TLS connections, or NULL
//   ClientContextLegacy: the SSL_CTX* for TLSMode 2 (legacy profile), or NULL
//   ServerContextLegacy: the SSL_CTX* for the incoming legacy port, or NULL
typedef struct	_SocketTLSHooks
	{
	void	*(*ClientContext)(void);
	BOOL	(*BeforeHandshake)(void *ssl, const char *AE, const char *host);
	BOOL	(*AfterHandshake)(void *ssl, const char *AE, const char *host);
	void	(*Log)(int level, const char *text);
	void	*(*ServerContext)(void);
	void	*(*ClientContextLegacy)(void);
	void	*(*ServerContextLegacy)(void);
	}	SocketTLSHooks;
extern	SocketTLSHooks	SocketTLS;


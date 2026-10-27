// 20091231 bcb     Changed char* to const char* for gcc4.2 warnings
// 20100111 mvh     Merged
// 20100122 mvh     Added Poll function
// 20100619 bcb     Added no-copy to the Socket class.
// 20100707 mvh     Merged
// 20121214 mvh     Mode sa public
// 20140528 lsp     Private copy constructor is not GNUC specific
// 20261026 mmd     SetTLS: a node that needs DICOM TLS (T in acrnema.map) is refused by Open
//                  (this variant has no TLS: never a plain connection instead)
// 20261026 mmd     ConnectTimeOut / ConnectTimedOut (connect time limit, 0 = as before)
// 20261027 mmd     Deadline (absolute end of connect, reads and writes; 0 = none)

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
		// attempts of Open together end after this many seconds. Set only by the outgoing storage
		// commitment worker. ConnectTimedOut: the last Open failed because one of these limits
		// ran out (a node that does not answer).
		int	ConnectTimeOut;
		BOOL	ConnectTimedOut;
		// Overall deadline (20261027 mmd). 0 (the default) = none, as before. Otherwise an
		// absolute time (seconds, time()): the connect attempts of Open and
		// every read and write end by then, and once it has passed every read and write fails at
		// once (so a peer that sends a message slowly, byte by byte, cannot hold a reader that
		// ignores single failed reads). Plain writes are non-blocking and end at the deadline (a
		// peer that stops reading cannot hold the sender): Unix MSG_DONTWAIT with poll(), Windows
		// FIONBIO for the send with select(). Set only by the outgoing storage commitment code around a
		// connect and its association setup.
		long	Deadline;
		
		// UDP Extensions
		BOOL	BindUDPServer(char *port);
		BOOL	BindUDPClient(char *host, const char *port);

		// Node that needs DICOM TLS (20261026 mmd). TLSMode 0 = plain, exactly as before.
		// TLSMode 1 or 2 = the node has T (or T and L) in acrnema.map: this variant has no
		// TLS, so the next Open refuses the connection (never a plain connection instead).
		int	TLSMode;
		char	TLSPeerAE[20];		// node being connected, for the refusal message
		BOOL	SetTLS(int on, const char *AE);
	private:// This will prevent it from being copied (it has a pointer)
		Socket(const Socket&);
		const	Socket & operator = (const Socket&);
	};

// Operator and debug messages of the socket layer, installed by the application (dgate):
// level 0 = operator message, 1 = debug. NULL: level 0 goes to stderr.
extern	void	(*SocketLog)(int level, const char *text);


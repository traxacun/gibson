/*
 * Copyright (c) 2013, Simone Margaritelli <evilsocket at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Gibson nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "configure.h"
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>
#if HAVE_BACKTRACE
#include <execinfo.h>
#endif
#include "log.h"
#include "net.h"
#include "atree.h"
#include "query.h"
#include "config.h"
#include "default.h"

extern char *aeApiName();

static gbServer server;

void gbReadQueryHandler( gbEventLoop *el, int fd, void *privdata, int mask );
void gbWriteReplyHandler( gbEventLoop *el, int fd, void *privdata, int mask );
void gbAcceptHandler(gbEventLoop *e, int fd, void *privdata, int mask);
void gbMemoryFreeHandler( anode_t *elem, size_t level, void *data );
int  gbServerCronHandler(struct gbEventLoop *eventLoop, long long id, void *data);
void gbDaemonize();
void gbProcessInit();
void gbServerDestroy( gbServer *server );
void gbOOM(size_t size);

void gbHelpMenu( char **argv, int exitcode ){
	printf( "Gibson cache server v%s %s ( built %s )\nCopyright %s\nReleased under %s\n\n", VERSION, BUILD_GIT_BRANCH, BUILD_DATETIME, AUTHOR, LICENSE );

	printf( "%s [-h|--help] [-c|--config FILE]\n\n", argv[0] );

	printf("  -h, --help          Print this help and exit.\n");
	printf("  -c, --config FILE   Set configuration file to load, default %s.\n\n", GB_DEFAULT_CONFIGURATION );

	exit(exitcode);
}

static void gbMemFormat( unsigned long used, char *buffer, size_t size ){
	memset( buffer, 0x00, size );
	char *suffix[] = { "B", "KB", "MB", "GB", "TB" };
	size_t i = 0;
	double d = used;
    
    for( ; i < 5 && d >= 1024; ++i ){
        d /= 1024.0;
    }

	sprintf( buffer, "%.1f%s", d, suffix[i] );
}

int main( int argc, char **argv)
{
	int c, option_index = 0;

	static struct option long_options[] =
	{
		{"help",    no_argument,       0, 'h'},
		{"config",  required_argument, 0, 'c'},
		{0, 0, 0, 0}
	};

    char *configuration = GB_DEFAULT_CONFIGURATION;

    while(1){
    	c = getopt_long( argc, argv, "hc:", long_options, &option_index );
    	if( c == -1 )
    		break;

    	switch (c)
		{
			case 0:
				/* If this option set a flag, do nothing else now. */
				if (long_options[option_index].flag != 0)
					break;

			break;

			case 'h':

				gbHelpMenu(argv,0);

			break;

			case 'c':

				configuration = optarg;

			break;

			default:
				gbHelpMenu(argv,1);
		}
    }

    zmem_set_oom_handler(gbOOM);

	memset( &server, 0x00, sizeof(gbServer) );

    gbConfigLoad( &server.config, configuration );

	gbLogInit
	(
	  gbConfigReadString( &server.config, "logfile", GB_DEAFULT_LOG_FILE ),
	  gbConfigReadInt( &server.config, "loglevel", GB_DEFAULT_LOG_LEVEL ),
	  gbConfigReadInt( &server.config, "logflushrate", GB_DEFAULT_LOG_FLUSH_LEVEL )
	);

	const char *sock = gbConfigReadString( &server.config, "unix_socket", NULL );
	if( sock != NULL ){
		gbLog( INFO, "Creating unix server socket on %s ...", sock );

		strncpy( server.address, sock, 0xFF );
		unlink( server.address );

		server.type	= UNIX;
		server.fd   = gbNetUnixServer( server.error, server.address, 0777 );
	}
	else {
		const char *address = gbConfigReadString( &server.config, "address", GB_DEFAULT_ADDRESS );
		int port = gbConfigReadInt( &server.config, "port", GB_DEFAULT_PORT );

		gbLog( INFO, "Creating tcp server socket on %s:%d ...", address, port );

		strncpy( server.address, address, 0xFF );

		server.type	= TCP;
		server.port	= port;
		server.fd   = gbNetTcpServer( server.error, server.port, server.address );
	}

	if( server.fd == GBNET_ERR ){
		gbLog( ERROR, "Error creating server : %s", server.error );
		exit(1);
	}

	// read server limit values from config
	server.limits.maxidletime     = gbConfigReadInt( &server.config, "max_idletime",       GBNET_DEFAULT_MAX_IDLE_TIME );
	server.limits.maxclients      = gbConfigReadInt( &server.config, "max_clients",        GBNET_DEFAULT_MAX_CLIENTS );
	server.limits.maxrequestsize  = gbConfigReadSize( &server.config, "max_request_size",  GBNET_DEFAULT_MAX_REQUEST_BUFFER_SIZE );
	server.limits.maxitemttl	  = gbConfigReadInt( &server.config, "max_item_ttl",       GB_DEFAULT_MAX_ITEM_TTL );
	server.limits.maxmem		  = gbConfigReadSize( &server.config, "max_memory",        GB_DEFAULT_MAX_MEMORY );
	server.limits.maxkeysize	  = gbConfigReadSize( &server.config, "max_key_size",      GB_DEFAULT_MAX_QUERY_KEY_SIZE );
	server.limits.maxvaluesize	  = gbConfigReadSize( &server.config, "max_value_size",    GB_DEFAULT_MAX_QUERY_VALUE_SIZE );
	server.limits.maxresponsesize = gbConfigReadSize( &server.config, "max_response_size", GB_DEFAULT_MAX_RESPONSE_SIZE );

	// initialize server statistics
	server.stats.started     =
	server.stats.time	     = time(NULL);
	server.stats.memused     =
	server.stats.mempeak     =
	server.stats.firstin     =
	server.stats.lastin      =
	server.stats.crondone    =
	server.stats.nclients    =
	server.stats.nitems	     =
	server.stats.ncompressed =
	server.stats.sizeavg	 = 
    server.stats.compravg    = 0;
	server.stats.memavail    = zmem_available();

	if( server.limits.maxmem > server.stats.memavail ){
		char drop[0xFF] = {0};

		gbMemFormat( server.stats.memavail / 2, drop, 0xFF );

		gbLog( WARNING, "max_memory setting is higher than total available memory, dropping to %s.", drop );

		server.limits.maxmem = server.stats.memavail / 2;
	}

	server.compression = gbConfigReadSize( &server.config, "compression",	   GB_DEFAULT_COMPRESSION );
	server.daemon	   = gbConfigReadInt( &server.config, "daemonize", 		   0 );
	server.cronperiod  = gbConfigReadInt( &server.config, "cron_period", 	   GB_DEFAULT_CRON_PERIOD );
	server.pidfile	   = gbConfigReadString( &server.config, "pidfile",        GB_DEFAULT_PID_FILE );
    server.gc_ratio    = gbConfigReadTime( &server.config, "gc_ratio",         GB_DEFAULT_GC_RATIO );
	server.events 	   = gbCreateEventLoop( server.limits.maxclients + 1024 );
	server.clients 	   = ll_prealloc( server.limits.maxclients );
	server.m_keys	   = ll_prealloc( 255 );
	server.m_values	   = ll_prealloc( 255 );
	server.idlecron	   = server.limits.maxidletime * 1000;
	server.lzf_buffer  = zcalloc( server.limits.maxrequestsize );
	server.m_buffer	   = zcalloc( server.limits.maxresponsesize );
	server.shutdown	   = 0;

	at_init_tree( server.tree );

	char reqsize[0xFF] = {0},
		 maxmem[0xFF] = {0},
		 maxkey[0xFF] = {0},
		 maxvalue[0xFF] = {0},
		 maxrespsize[0xFF] = {0},
		 compr[0xFF] = {0};

	gbMemFormat( server.limits.maxrequestsize, reqsize, 0xFF );
	gbMemFormat( server.limits.maxmem, maxmem, 0xFF );
	gbMemFormat( server.limits.maxkeysize, maxkey, 0xFF );
	gbMemFormat( server.limits.maxvaluesize, maxvalue, 0xFF );
	gbMemFormat( server.limits.maxresponsesize, maxrespsize, 0xFF );
	gbMemFormat( server.compression, compr, 0xFF );

	gbLog( INFO, "Server starting ..." );
	gbLog( INFO, "Git Branch       : '%s'", BUILD_GIT_BRANCH );
	gbLog( INFO, "Multiplexing API : '%s'", aeApiName() );
#if HAVE_JEMALLOC == 1
	const char *p;
	size_t s = sizeof(p);
	mallctl("version", &p,  &s, NULL, 0);

	gbLog( INFO, "Memory allocator : 'jemalloc %s'", p );
#else
	gbLog( INFO, "Memory allocator : 'malloc'" );
#endif
	gbLog( INFO, "Max idle time    : %ds", server.limits.maxidletime );
	gbLog( INFO, "Max clients      : %d", server.limits.maxclients );
	gbLog( INFO, "Max request size : %s", reqsize );
	gbLog( INFO, "Max memory       : %s", maxmem );
    gbLog( INFO, "GC Ratio         : %ds", server.gc_ratio );
	gbLog( INFO, "Max key size     : %s", maxkey );
	gbLog( INFO, "Max value size   : %s", maxvalue );
	gbLog( INFO, "Max resp. size   : %s", maxrespsize );
	gbLog( INFO, "Data LZF compr.  : %s", compr );
	gbLog( INFO, "Cron period      : %dms", server.cronperiod );

	gbProcessInit();

	server.cron_id = gbCreateTimeEvent( server.events, 1, gbServerCronHandler, &server, NULL );

	gbCreateFileEvent( server.events, server.fd, GB_READABLE, gbAcceptHandler, &server );

	gbEventLoopMain( server.events );
	gbDeleteEventLoop( server.events );

	return 0;
}

void gbOOM(size_t size){
    char used[0xFF] = {0},
         max[0xFF] = {0},
         uptime[0xFF] = {0};

    gbMemFormat( server.stats.memused, used, 0xFF );
    gbMemFormat( server.limits.maxmem, max,  0xFF );
    gbServerFormatUptime( &server, uptime );

    gbLog( CRITICAL, "Out of memory trying to allocate %zu bytes.", size );

    gbLog( CRITICAL, "" );
    gbLog( CRITICAL, "INFO:" );
    gbLog( CRITICAL, "" );

    gbLog( CRITICAL, "  Git Branch      : %s", BUILD_GIT_BRANCH );
    gbLog( CRITICAL, "  Git HEAD Rev.   : %s", BUILD_GIT_SHA1 );
    gbLog( CRITICAL, "  Uptime          : %s", uptime );
    gbLog( CRITICAL, "  Memory Used     : %s/%s", used, max );
    gbLog( CRITICAL, "  Current Items   : %d", server.stats.nitems );
    gbLog( CRITICAL, "  Current Clients : %d", server.stats.nclients );
   
    gbLogFinalize();
   
    abort();
}

void gbWriteReplyHandler( gbEventLoop *el, int fd, void *privdata, int mask ) {
    gbClient *client = privdata;
    size_t nwrote = 0, towrite = 0;

    if( client->status == STATUS_SENDING_REPLY ){
		towrite = client->buffer_size - client->wrote;
		nwrote  = write( client->fd, client->buffer + client->wrote, towrite );

		if (nwrote == -1){
			if (errno == EAGAIN){
				nwrote = 0;
			}
			else{
				gbLog( DEBUG, "Error writing to client: %s",strerror(errno));
				gbClientDestroy(client);
				return;
			}
		}
		else if (nwrote == 0){
			gbLog( DEBUG, "Client closed connection.");
			gbClientDestroy(client);
			return;
		}
		else{
			client->wrote += nwrote;
			client->seen = client->server->stats.time;

			if( client->wrote == client->buffer_size ){
				if( client->shutdown )
					gbClientDestroy(client);

				else{
					gbClientReset(client);
					gbDeleteFileEvent( client->server->events, client->fd, GB_WRITABLE );
				}
			}

		}
    }
    else {
    	gbLog( WARNING, "Unexpected status %d for client while sending response.", client->status );
    	gbClientDestroy(client);
    }
}

void gbReadQueryHandler( gbEventLoop *el, int fd, void *privdata, int mask ) {
	gbClient *client = ( gbClient * )privdata;
	gbServer *server = client->server;
	byte_t   *p = NULL;
	int nread, toread;

	// we're still readying the buffer size from the socket
	if( client->status == STATUS_WAITING_SIZE ){
		toread = sizeof(int) - client->read;
		// keep reading it
		if( toread > 0 ){
			p = (byte_t *)( &client->buffer_size + client->read );
		}
		// we're done, start reading the buffer
		else {
			client->read   = 0;
			client->status = STATUS_WAITING_BUFFER;
			// make sure the buffer is not too big or too small ( must be at least 2 bytes to contain the opcode )
			if( client->buffer_size > server->limits.maxrequestsize || client->buffer_size < sizeof(short) ){
				gbLog( WARNING, "Client request size %d invalid.", client->buffer_size );
				gbClientDestroy(client);
				return;
			}
            // allocate buffer for the incoming request
            else
                client->buffer = zmalloc( client->buffer_size );
		}
	}

	// we are reading the buffer ( of client->buffer_size total bytes )
	if( client->status == STATUS_WAITING_BUFFER ){
		toread = client->buffer_size - client->read;
		if( toread > 0 ){
			p = client->buffer + client->read;
		}
        // nothing left to read
		else {
			client->read   = 0;
			client->status = STATUS_SENDING_REPLY;
		}
	}

	// if we still have something to read
	if( client->status != STATUS_SENDING_REPLY ){
		nread = read( fd, p, toread );
		if (nread == -1){
			// try again, operation failed
			if (errno == EAGAIN){
				nread = 0;
			}
			else{
				gbLog( WARNING, "Error reading from client: %s",strerror(errno));
				gbClientDestroy(client);
				return;
			}
		// bye bye dear client ^_^
		}
		else if (nread == 0){
			gbLog( DEBUG, "Client closed connection.");
			gbClientDestroy(client);
			return;
		}
	}

	client->read += nread;
	client->seen = client->server->stats.time;

	// process the query only if we were reading it and the request is complete
	if( client->status == STATUS_WAITING_BUFFER && client->read == client->buffer_size ){
		client->status = STATUS_SENDING_REPLY;
        
		if( gbProcessQuery(client) != GB_OK ){
			size_t sz = client->buffer_size < 255 ? client->buffer_size : 255;

			gbLog( WARNING, "Malformed query, dropping client." );
			gbLog( WARNING, "  Buffer size: %d opcode:%d - First %d bytes:", client->buffer_size, *(short *)&client->buffer[0], sz );
			gbLogDumpBuffer( WARNING, client->buffer, sz );

			gbClientDestroy(client);
		}
	}
}

void gbAcceptHandler(gbEventLoop *e, int fd, void *privdata, int mask) {
    int client_port = 0, client_fd;
    char client_ip[128] = {0};
    gbServer *server = (gbServer *)privdata;

    if( server->type == TCP )
    	client_fd = gbNetTcpAccept( server->error, fd, client_ip, &client_port );
    else
    	client_fd = gbNetUnixAccept( server->error, fd );

    if (client_fd == GB_ERR) {
    	gbLog( WARNING, "Error accepting client connection: %s", server->error );
        return;
    }
    else if( server->stats.nclients >= server->limits.maxclients ) {
    	close(client_fd);
    	gbLog( WARNING, "Dropping connection, current clients = %d, max = %d.", server->stats.nclients, server->limits.maxclients );
    }

    gbLog( DEBUG, "New connection from %s:%d", *client_ip ? client_ip : server->address, client_port );

	if (client_fd != -1) {
		gbNetNonBlock(NULL,client_fd);
		gbNetEnableTcpNoDelay(NULL,client_fd);
        gbNetKeepAlive(NULL,client_fd,server->limits.maxidletime);        

	    gbClient *client = gbClientCreate(client_fd,server);

		if( gbCreateFileEvent( e, client_fd, GB_READABLE, gbReadQueryHandler, client ) == GB_ERR ) {
			gbLog( WARNING, "Unable to wait for client readable state." );
			gbClientDestroy( client );
			close(client_fd);
			return;
		}
	}
}

#define GB_DEL_ITEM(s,n,i) (n)->marker = NULL; gbDestroyItem( (s), (i) )

void gbMemoryFreeHandler( anode_t *node, size_t level, void *data ) {
	gbServer *server = data;
	gbItem	 *item = node->marker;
	time_t	  eta = item ? ( server->stats.time - item->last_access_time ) : 0;

	// item is older enough to be deleted
	if( eta && eta >= server->gc_ratio ) {
	    gbLog( DEBUG, "[OOM] Removing item %p since wasn't accessed from %lus.", item, eta );
        
        GB_DEL_ITEM( server, node, item );
	}
}

void gbHandleDeadTTLHandler( anode_t *node, size_t level, void *data ){
	gbServer *server = data;
	gbItem	 *item = node->marker;
	time_t	  eta = item ? ( server->stats.time - item->time ) : 0;

	// item is older enough to be deleted
	if( item && item->ttl > 0 && eta >= item->ttl ) {
		gbLog( DEBUG, "[CRON] TTL of %ds expired for item at %p.", item->ttl, item );
        
        GB_DEL_ITEM( server, node, item );
	}
}

#define CRON_EVERY(_ms_) if ((_ms_ <= server->cronperiod) || !(server->stats.crondone % ((_ms_)/server->cronperiod)))

int gbServerCronHandler(struct gbEventLoop *eventLoop, long long id, void *data) {
	gbServer *server = data;
	time_t now = time(NULL);
	char used[0xFF] = {0},
		 max[0xFF] = {0},
		 freed[0xFF] = {0},
		 uptime[0xFF] = {0},
		 avgsize[0xFF] = {0};
	unsigned long before = 0;
	long deleted = 0;

	server->stats.time = now;

	// shutdown requested
	if( server->shutdown )
		gbServerDestroy( server );

	CRON_EVERY( 15000 ) {
		before = server->stats.memused;

		at_recurse( &server->tree, gbHandleDeadTTLHandler, server, 0 );

		deleted = before - server->stats.memused;

		if( deleted > 0 ){
			gbMemFormat( deleted, freed, 0xFF );

			gbLog( INFO, "Freed %s of expired data, left %d items.", freed, server->stats.nitems );
		}
	}

	CRON_EVERY( 5000 ) {
		if( server->stats.memused > server->limits.maxmem ){

			before = server->stats.memused;

			gbLog( WARNING, "Max memory exhausted, trying to free data that was accessed not in the last %ds.", server->gc_ratio );

			at_recurse( &server->tree, gbMemoryFreeHandler, server, 0 );

			gbMemFormat( before - server->stats.memused, freed,  0xFF );

			gbLog( INFO, "Freed %s, left %d items.", freed, server->stats.nitems );
		}
	}

	CRON_EVERY( 15000 ){
		gbMemFormat( server->stats.memused, used, 0xFF );
		gbMemFormat( server->limits.maxmem, max,  0xFF );
		gbMemFormat( server->stats.sizeavg, avgsize, 0xFF );

		gbServerFormatUptime( server, uptime );

		gbLog
		(
		  INFO,
		  "MEM %s/%s - CLIENTS %d - OBJECTS %d ( %d COMPRESSED ) - AVERAGE SIZE %s - UPTIME %s",
		  used,
		  max,
		  server->stats.nclients,
		  server->stats.nitems,
		  server->stats.ncompressed,
		  avgsize,
		  uptime
		);
	}

	++server->stats.crondone;

	return server->cronperiod;
}

void gbDaemonize(){
    int fd;

	// parent exits
    if (fork() != 0) exit(0);
	// create a new session
    setsid();

    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

static char *gbSignalDescription(int sig){
	switch(sig){
		case SIGABRT : return "ABNORMAL TERMINATION";
		case SIGFPE	 : return "FLOATING POINT EXCEPTION";
		case SIGILL	 : return "ILLEGAL INSTRUCTION";
		case SIGINT	 : return "INTERRUPT SIGNAL";
		case SIGSEGV : return "SEGMENTATION VIOLATION";
		case SIGTERM : return "TERMINATION REQUEST";
		default      : return "UNKNOWN SIGNAL";
	}
}

static void gbSignalHandler(int sig) {
	if( sig == SIGTERM ){
		gbLog( WARNING, "Received SIGTERM, scheduling shutdown..." );
		server.shutdown = 1;
	}
	else {
		gbLog( CRITICAL, "" );
		gbLog( CRITICAL, "********* %s *********", gbSignalDescription(sig) );
		gbLog( CRITICAL, "" );

		void *trace[32];
		size_t size, i;
		char **strings;
#if HAVE_BACKTRACE
		size    = backtrace( trace, 32 );
		strings = backtrace_symbols( trace, size );
#endif
		char used[0xFF] = {0},
			 max[0xFF] = {0},
			 uptime[0xFF] = {0};

		gbMemFormat( server.stats.memused, used, 0xFF );
		gbMemFormat( server.limits.maxmem, max,  0xFF );
		gbServerFormatUptime( &server, uptime );

		gbLog( CRITICAL, "INFO:" );
		gbLog( CRITICAL, "" );

		gbLog( CRITICAL, "  Git Branch      : %s", BUILD_GIT_BRANCH );
		gbLog( CRITICAL, "  Git HEAD Rev.   : %s", BUILD_GIT_SHA1 );
		gbLog( CRITICAL, "  Uptime          : %s", uptime );
		gbLog( CRITICAL, "  Memory Used     : %s/%s", used, max );
		gbLog( CRITICAL, "  Current Items   : %d", server.stats.nitems );
		gbLog( CRITICAL, "  Current Clients : %d", server.stats.nclients );
#if HAVE_BACKTRACE
		gbLog( CRITICAL, "" );
		gbLog( CRITICAL, "BACKTRACE:" );
		gbLog( CRITICAL, "" );

		for( i = 0; i < size; i++ ){
			gbLog( CRITICAL, "  %s", strings[i] );
		}
#endif
		gbLog( CRITICAL, "" );
		gbLog( CRITICAL, "***************************************" );

        gbLogFinalize();
		
        exit(-1);
	}
}

void gbProcessInit(){
	struct sigaction act;

	if( server.daemon )
		gbDaemonize();

	// ignore SIGHUP and SIGPIPE since we're gonna handle dead clients
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	// set SIGTERM and SIGSEGV custom handler
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = gbSignalHandler;

	sigaction( SIGTERM, &act, NULL );
	sigaction( SIGSEGV, &act, NULL );
	sigaction( SIGILL,  &act, NULL );
	sigaction( SIGFPE,  &act, NULL );
	sigaction( SIGABRT, &act, NULL );

	FILE *fp = fopen(server.pidfile,"w+t");
	if (fp) {
		fprintf(fp,"%d\n",(int)getpid());
		fclose(fp);
	}
	else
		gbLog( WARNING, "Error creating pid file %s.", server.pidfile );
}

void gbObjectDestroyHandler( anode_t *elem, size_t level, void *data ){
	gbItem *item = elem->marker;
	if( item )
		gbDestroyItem( data, item );
}

void gbConfigDestroyHandler( anode_t *elem, size_t level, void *data ){
	char *item = elem->marker;
	if( item )
		zfree( item );
}

void gbServerDestroy( gbServer *server ){
	if( server->clients ){
		ll_foreach( server->clients, citem ){
			gbClient *client = citem->data;
			if( client ){
				gbClientDestroy(client);
			}
		}

		ll_destroy( server->clients );
	}

	ll_destroy( server->m_keys );
	ll_destroy( server->m_values );

	zfree( server->m_buffer );
	zfree( server->lzf_buffer );

	at_recurse( &server->tree, gbObjectDestroyHandler, server, 0 );
	at_recurse( &server->config, gbConfigDestroyHandler, NULL, 0 );

	at_free( &server->tree );
	at_free( &server->config );

	gbDeleteTimeEvent( server->events, server->cron_id );
	gbDeleteEventLoop( server->events );
	gbLogFinalize();

	exit( 0 );
}

/*
 Copyright (c) 2017-2019 (c) Project "DeM Labs Inc" https://github.com/demlabsinc
  All rights reserved.

 This file is part of DAP (Deus Applications Prototypes) the open source project

    DAP (Deus Applicaions Prototypes) is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    DAP is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with any DAP based project.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#ifndef _WIN32
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/ioctl.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <linux/if.h>
#include <linux/if_tun.h>
#else
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <io.h>
#include <pthread.h>
#include "win32/iphdr.h"
#include "win32/ip.h"
//#include "win32/tap-windows.h"

#endif

#include "uthash.h"
#include "utlist.h"

#include "dap_common.h"

#include "dap_client_remote.h"
#include "dap_http_client.h"

#include "dap_stream.h"
#include "dap_stream_ch.h"
#include "dap_stream_ch_proc.h"
#include "dap_stream_ch_pkt.h"

#define LOG_TAG "stream_ch_vpn"

#define VPN_PACKET_OP_CODE_CONNECTED        0x000000a9
#define VPN_PACKET_OP_CODE_CONNECT          0x000000aa
#define VPN_PACKET_OP_CODE_DISCONNECT       0x000000ab
#define VPN_PACKET_OP_CODE_SEND             0x000000ac
#define VPN_PACKET_OP_CODE_RECV             0x000000ad
#define VPN_PACKET_OP_CODE_PROBLEM          0x000000ae

#define VPN_PROBLEM_CODE_NO_FREE_ADDR       0x00000001
#define VPN_PROBLEM_CODE_TUNNEL_DOWN        0x00000002
#define VPN_PROBLEM_CODE_PACKET_LOST        0x00000003

#define VPN_PACKET_OP_CODE_VPN_METADATA     0x000000b0
#define VPN_PACKET_OP_CODE_VPN_RESERVED     0x000000b1
#define VPN_PACKET_OP_CODE_VPN_ADDR_REQUEST 0x000000b2
#define VPN_PACKET_OP_CODE_VPN_ADDR_REPLY   0x000000b3

#define VPN_PACKET_OP_CODE_VPN_SEND         0x000000bc
#define VPN_PACKET_OP_CODE_VPN_RECV         0x000000bd

#define SF_MAX_EVENTS 256

#ifdef _WIN32

  typedef uint32_t in_addr_t;

  #define _TAP_IOCTL(nr) CTL_CODE(FILE_DEVICE_UNKNOWN, nr, METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

  #define TAP_IOCTL_GET_MAC               _TAP_IOCTL(1)
  #define TAP_IOCTL_GET_VERSION           _TAP_IOCTL(2)
  #define TAP_IOCTL_GET_MTU               _TAP_IOCTL(3)
  #define TAP_IOCTL_GET_INFO              _TAP_IOCTL(4)
  #define TAP_IOCTL_CONFIG_POINT_TO_POINT _TAP_IOCTL(5)
  #define TAP_IOCTL_SET_MEDIA_STATUS      _TAP_IOCTL(6)
  #define TAP_IOCTL_CONFIG_DHCP_MASQ      _TAP_IOCTL(7)
  #define TAP_IOCTL_GET_LOG_LINE          _TAP_IOCTL(8)
  #define TAP_IOCTL_CONFIG_DHCP_SET_OPT   _TAP_IOCTL(9)
  #define TAP_IOCTL_CONFIG_TUN            _TAP_IOCTL(10)

  #define TAP_COMPONENT_ID "tap0901"

  #define DEVTEMPLATE "\\\\.\\Global\\%s.tap"

  #define NETDEV_GUID "{4D36E972-E325-11CE-BFC1-08002BE10318}"
  #define CONTROL_KEY "SYSTEM\\CurrentControlSet\\Control\\"

  #define ADAPTERS_KEY CONTROL_KEY "Class\\" NETDEV_GUID
  #define CONNECTIONS_KEY CONTROL_KEY "Network\\" NETDEV_GUID

  typedef intptr_t (tap_callback)( char *idxname, char *name );

#endif

typedef struct ch_vpn_pkt {

  struct {

    int      sock_id; // Client's socket id
    uint32_t op_code; // Operation code

    union {
      struct { // L4 connect operation
        uint32_t addr_size;
        uint16_t port;
        uint16_t padding;
      } op_connect;

      struct { // For data transmission, usualy for I/O functions
        uint32_t data_size;
        uint32_t padding;
      } op_data;

      struct { // We have a problem and we know that!
        uint32_t code; // I hope we'll have no more than 4B+ problems, not I??
        uint32_t padding;
      } op_problem;

      struct {
        uint32_t padding1;
        uint32_t padding2;
      } raw; // Raw access to OP bytes

      uint8_t pkt_data[ 8 ];
    };
  } __attribute__((packed)) header;

  uint8_t data[]; // Binary data nested by packet

}  __attribute__((packed)) ch_vpn_pkt_t;

/**
  * @struct ch_vpn_socket_proxy
  * @brief Internal data storage for single socket proxy functions. Usualy helpfull for\
  *        port forwarding or for protecting single application's connection
  *
  **/

#define PROXY_PKT_BUFFER_SIZE 100

typedef struct ch_vpn_socket_proxy {

  int id;
  int sock;

  struct in_addr client_addr; // Used in raw L3 connections

  pthread_mutex_t mutex;
  dap_stream_ch_t *ch;

  bool signal_to_delete;

  ch_vpn_pkt_t *pkt_out[ PROXY_PKT_BUFFER_SIZE ];
  size_t pkt_out_size;

  uint64_t bytes_sent;
  uint64_t bytes_recieved;

  time_t time_created;
  time_t time_lastused;

  UT_hash_handle hh;
  UT_hash_handle hh2;
  UT_hash_handle hh_sock;

} ch_vpn_socket_proxy_t;

/**
  * @struct dap_stream_ch_vpn
  * @brief Object that creates for every remote channel client
  *
  **/
typedef struct dap_stream_ch_vpn {

  pthread_mutex_t mutex;
  ch_vpn_socket_proxy_t *socks;
  int raw_l3_sock;

} dap_stream_ch_vpn_t;

typedef struct dap_stream_ch_vpn_remote_single {

  in_addr_t addr;
//  pthread_mutex_t mutex;
  dap_stream_ch_t *ch;

  uint64_t bytes_sent;
  uint64_t bytes_recieved;

  UT_hash_handle hh;

} dap_stream_ch_vpn_remote_single_t;

//#ifdef _WIN32
//  typedef HANDLE t_tun;
//#else
//  typedef int t_tun;
//# endif

#define VPN_PKT_BUFFER_SIZE 400

typedef struct vpn_local_network {

  struct in_addr client_addr_last;
  struct in_addr client_addr_mask;
  struct in_addr client_addr_host;
  struct in_addr client_addr;

  int tun_ctl_fd;

  #ifndef _WIN32
    int tun_fd;
  #else
    HANDLE tun_fd;
    OVERLAPPED tun_read_overlap, tun_write_overlap;
    int tun_index, tun_read_pending;
  #endif

  int flags;

  #ifndef _WIN32
    struct ifreq ifr;
  #endif

  dap_stream_ch_vpn_remote_single_t *clients; // Remote clients identified by destination address

  ch_vpn_pkt_t *pkt_out[ VPN_PKT_BUFFER_SIZE ];

  size_t pkt_out_size;
  size_t pkt_out_rindex;
  size_t pkt_out_windex;

  pthread_mutex_t pkt_out_mutex;
  pthread_mutex_t clients_mutex;

} vpn_local_network_t;

typedef struct list_addr_element {

  struct in_addr addr;
  struct list_addr_element *next;

} list_addr_element;

static list_addr_element *list_addr_head = NULL;

static ch_vpn_socket_proxy_t *sf_socks = NULL;
static ch_vpn_socket_proxy_t *sf_socks_client = NULL;

static pthread_mutex_t sf_socks_mutex;
static pthread_cond_t  sf_socks_cond;

static EPOLL_HANDLE sf_socks_epoll_fd;

static pthread_t sf_socks_pid;
static pthread_t sf_socks_raw_pid;

static vpn_local_network_t *raw_server;

#ifdef _WIN32
  HANDLE hTerminateEvent = NULL;
  HANDLE hTunWriteEvent  = NULL;
#endif

bool   bQuitSignal = false;

#define DAP_STREAM_CH_VPN(a) ((dap_stream_ch_vpn_t *) ((a)->internal) )

void  *ch_sf_thread( void *arg );
void  *ch_sf_thread_raw( void *arg );

void  ch_sf_tun_create( );
void  ch_sf_tun_destroy( );

void  ch_sf_client_new( dap_stream_ch_t *ch , void *arg );
void  ch_sf_delete( dap_stream_ch_t *ch , void *arg );

void  ch_sf_packet_in( dap_stream_ch_t *ch , void *arg );
void  ch_sf_packet_out( dap_stream_ch_t *ch , void *arg );

int   ch_sf_raw_write( uint8_t op_code, const void *data, size_t data_size );
void  stream_sf_disconnect( ch_vpn_socket_proxy_t *sf_sock );

static const char *l_vpn_addr, *l_vpn_mask;

/**
 * @brief dap_stream_ch_vpn_init Init actions for VPN stream channel
 * @param vpn_addr Zero if only client mode. Address if the node shares its local VPN
 * @param vpn_mask Zero if only client mode. Mask if the node shares its local VPN
 * @return 0 if everything is okay, lesser then zero if errors
 */
int dap_stream_ch_vpn_init( const char *vpn_addr, const char *vpn_mask )
{
  if ( !vpn_addr || !vpn_mask ) {
    return 0;
  }

  l_vpn_addr = strdup( vpn_addr );
  l_vpn_mask = strdup( vpn_mask );

  raw_server = calloc( 1, sizeof(vpn_local_network_t) );

  pthread_mutex_init( &raw_server->clients_mutex, NULL );
  pthread_mutex_init( &raw_server->pkt_out_mutex, NULL );

  pthread_mutex_init( &sf_socks_mutex, NULL );
  pthread_cond_init(  &sf_socks_cond, NULL );

  pthread_create( &sf_socks_raw_pid, NULL, ch_sf_thread_raw, NULL );
  pthread_create( &sf_socks_pid,     NULL, ch_sf_thread,     NULL );

  dap_stream_ch_proc_add( 's', ch_sf_client_new,
                               ch_sf_delete, 
                               ch_sf_packet_in, 
                               ch_sf_packet_out 
  );

  #ifdef _WIN32
    hTerminateEvent = CreateEventA( NULL, true, false, NULL );
    hTunWriteEvent  = CreateEventA( NULL, false, false, NULL );
  #endif

  return 0;
}


/**
 * @brief ch_sf_deinit
 */
void dap_stream_ch_vpn_deinit( )
{
  #ifdef _WIN32
    if ( hTunWriteEvent )
      CloseHandle( hTunWriteEvent );

    if ( hTerminateEvent )
      CloseHandle( hTerminateEvent );
  #endif

  pthread_mutex_destroy( &sf_socks_mutex );
  pthread_cond_destroy( &sf_socks_cond );

  free( (char*)l_vpn_addr );
  free( (char*)l_vpn_mask );

  if ( raw_server )
    free( raw_server );
}

#ifdef _WIN32

static intptr_t SearchTapsWIN32( tap_callback *cb, int all )
{
  LONG status;
  HKEY adapters_key, hkey;
  DWORD len, type;
  char buf[40];
//  wchar_t name[40];
  char name[128];
  char keyname[strlen(CONNECTIONS_KEY) + sizeof(buf) + 1 + strlen("\\Connection")];
  int i = 0, found = 0;
  intptr_t ret = -1;

  status = RegOpenKeyExA( HKEY_LOCAL_MACHINE, ADAPTERS_KEY, 0,
             KEY_READ, &adapters_key );

  if ( status ) {
    log_it( L_ERROR,"Error accessing registry key for network adapters" );
    return -1;
  }

  while ( 1 ) {

    len = sizeof( buf );
    status = RegEnumKeyExA( adapters_key, i++, buf, &len, NULL, NULL, NULL, NULL );
    if ( status ) {
      if ( status != ERROR_NO_MORE_ITEMS )
        ret = -1;
      break;
    }

    dap_snprintf( keyname, sizeof(keyname), "%s\\%s", ADAPTERS_KEY, buf );

    status = RegOpenKeyExA( HKEY_LOCAL_MACHINE, keyname, 0, KEY_QUERY_VALUE, &hkey );
    if ( status )
      continue;

    len = sizeof( buf) ;
    status = RegQueryValueExA( hkey, "ComponentId", NULL, &type, (unsigned char *)buf, &len );
    if ( status || type != REG_SZ || strcmp(buf, TAP_COMPONENT_ID) ) {
      RegCloseKey( hkey );
      continue;
    }

    len = sizeof( buf );
    status = RegQueryValueExA( hkey, "NetCfgInstanceId", NULL, &type, (unsigned char *)buf, &len );
    RegCloseKey( hkey );
    if ( status || type != REG_SZ )
      continue;

    dap_snprintf( keyname, sizeof(keyname), "%s\\%s\\Connection", CONNECTIONS_KEY, buf );

    status = RegOpenKeyExA( HKEY_LOCAL_MACHINE, keyname, 0, KEY_QUERY_VALUE, &hkey );
    if ( status )
      continue;

    len = sizeof( name );
    status = RegQueryValueExW( hkey, L"Name", NULL, &type, (unsigned char *)name, &len );
    RegCloseKey( hkey );
    if ( status || type != REG_SZ )
      continue;

    ++ found;

    ret = cb( buf, name );

    if ( !all )
      break;
  }

  RegCloseKey( adapters_key );

  if ( !found ) {
    log_it( L_ERROR,"Not found Windows-TAP adapters. Is the driver installed?" );
  }

  return ret;
}

static intptr_t tun_create_WIN32( char *guid, char *name )
{
  char devname[80];
  HANDLE tun_fd;
  ULONG data[3];
  uint8_t cdata[64];
  DWORD len;

  dap_snprintf( devname, sizeof(devname), DEVTEMPLATE, guid );

  tun_fd = CreateFileA( devname, GENERIC_WRITE | GENERIC_READ, 0, 0,
           OPEN_EXISTING, FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, 0 );

  if (tun_fd == INVALID_HANDLE_VALUE) {
    log_it( L_ERROR,"Failed to open %s", devname );
    return -1;
  }

  log_it( L_INFO,"opened tun device %s", devname);

  if ( !DeviceIoControl(tun_fd, TAP_IOCTL_GET_VERSION,
           data, sizeof(&data), data, sizeof(data), &len, NULL)) {

    log_it( L_ERROR,"Failed to obtain TAP driver version" );
    return -1;
  }
  if ( data[0] < 9 || (data[0] == 9 && data[1] < 9) ) {
    log_it( L_ERROR,"TAP-Windows driver v9.9 or greater is required (found %ld.%ld)", data[0], data[1] );
    return -1;
  }

  log_it( L_INFO,"TAP Windows driver v%ld.%ld (%ld)", data[0], data[1], data[2] );

  uint32_t  MTU;
  if ( !DeviceIoControl(tun_fd, TAP_IOCTL_GET_MTU,
           &MTU, sizeof(MTU), &MTU, sizeof(MTU), &len, NULL)) {

    log_it( L_WARNING,"Failed to obtain TAP MTU" );
  }

  log_it( L_INFO,"TAP-Windows MTU = %u ", MTU );

  if ( !DeviceIoControl( tun_fd, TAP_IOCTL_GET_MAC,
           cdata, sizeof(cdata), cdata, sizeof(cdata), &len, NULL) ) {

    log_it( L_ERROR, "Failed to get MAC" );
    return -1;
  }

  log_it( L_INFO,"TAP MAC addr %X-%X-%X-%X-%X-%X ", cdata[0], cdata[1], cdata[2], cdata[3], cdata[4], cdata[5] );

//  #define TAP_IOCTL_GET_MAC               _TAP_IOCTL(1)
//  #define TAP_IOCTL_GET_VERSION           _TAP_IOCTL(2)
//  #define TAP_IOCTL_GET_MTU               _TAP_IOCTL(3)
//  #define TAP_IOCTL_GET_INFO              _TAP_IOCTL(4)
//  #define TAP_IOCTL_CONFIG_POINT_TO_POINT _TAP_IOCTL(5)
//  #define TAP_IOCTL_SET_MEDIA_STATUS      _TAP_IOCTL(6)
//  #define TAP_IOCTL_CONFIG_DHCP_MASQ      _TAP_IOCTL(7)
//  #define TAP_IOCTL_GET_LOG_LINE          _TAP_IOCTL(8)
//  #define TAP_IOCTL_CONFIG_DHCP_SET_OPT   _TAP_IOCTL(9)
//  #define TAP_IOCTL_CONFIG_TUN            _TAP_IOCTL(10)

//  data[0] = inet_addr( "0.0.0.0" );

  log_it( L_INFO, "l_vpn_addr = %s, l_vpn_mask = %s", l_vpn_addr, l_vpn_mask );

  data[0] = inet_addr( l_vpn_addr );
  data[1] = 0;
  data[2] = 0;

  if ( !DeviceIoControl( tun_fd, TAP_IOCTL_CONFIG_TUN,
           data, sizeof(data), data, sizeof(data), &len, NULL) ) {

    log_it( L_ERROR, "Failed to set TAP IP addresses" );
    return -1;
  }


  data[0] = 1;
  if ( !DeviceIoControl(tun_fd, TAP_IOCTL_SET_MEDIA_STATUS,
           data, sizeof(data[0]), data, sizeof(data[0]),
           &len, NULL) ) {

    log_it( L_ERROR, "Failed to set TAP media status" );
    return -1;
  }

  {
  ULONG data;
  DWORD len;

  for ( data = 0; data <= 1; data ++ ) {
    if ( !DeviceIoControl((HANDLE)tun_fd, TAP_IOCTL_SET_MEDIA_STATUS,
          &data, sizeof(data), &data, sizeof(data), &len, NULL) ) {
      log_it( L_ERROR, "Failed to set TAP media status" );
      return -1;
    }
  }
  }

  raw_server->tun_fd = (HANDLE)tun_fd;
  raw_server->tun_read_overlap.hEvent = CreateEvent( NULL, FALSE, FALSE, NULL );

  return (intptr_t)tun_fd;
}

int win32_read_tun( uint8_t *buffer, uint32_t size )
{
  DWORD pkt_size;

 reread:

  if ( !raw_server->tun_read_pending && 
       !ReadFile(raw_server->tun_fd, buffer, size, &pkt_size,
          &raw_server->tun_read_overlap)) {
    DWORD err = GetLastError();

    if ( err == ERROR_IO_PENDING )
      raw_server->tun_read_pending = 1;
    else if (err == ERROR_OPERATION_ABORTED) {
      log_it( L_ERROR, "TAP device not active. Disconnecting.");
      return -1;
    } else {
      log_it( L_ERROR, "TAP device: read failed");
    }
    return -1;
  }

  if ( !GetOverlappedResult(raw_server->tun_fd,
          &raw_server->tun_read_overlap, &pkt_size,
          FALSE)) {
    DWORD err = GetLastError();

    if ( err != ERROR_IO_INCOMPLETE ) {
      raw_server->tun_read_pending = 0;
      log_it( L_ERROR, "TAP device: complete read failed");
      goto reread;
    }
    return -1;
  }

  raw_server->tun_read_pending = 0;
  
  return pkt_size;
}

int win32_write_tun( uint8_t *buffer, uint32_t size )
{
  DWORD pkt_size = 0;
  DWORD err;

  if ( WriteFile(raw_server->tun_fd, buffer, size, &pkt_size, &raw_server->tun_write_overlap) ) {
    log_it( L_INFO, "TAP device: wrote %u bytes", pkt_size );
    return pkt_size;
  }

  err = GetLastError();
  if ( err == ERROR_IO_PENDING ) {
    log_it( L_INFO, "TAP device: Waiting for write" );

    if ( GetOverlappedResult(raw_server->tun_fd, &raw_server->tun_write_overlap, &pkt_size, TRUE) ) {
      log_it( L_INFO, "TAP device: wrote %u bytes after waiting", pkt_size );
      return pkt_size;
    }
//    err = GetLastError();
  }

  log_it( L_ERROR, "TAP device: Failed to write" );

  return -1;
}

#endif

void ch_sf_tun_create()
{
  #ifndef _WIN32
    inet_aton( l_vpn_addr, &raw_server->client_addr );
    inet_aton( l_vpn_mask, &raw_server->client_addr_mask );
  #else
    raw_server->client_addr.s_addr = inet_addr( l_vpn_addr );
    raw_server->client_addr_mask.s_addr = inet_addr( l_vpn_mask );
  #endif

  raw_server->client_addr_host.s_addr = (raw_server->client_addr.s_addr | 0x01000000); // grow up some shit here!
  raw_server->client_addr_last.s_addr = raw_server->client_addr_host.s_addr;

  #ifndef _WIN32

  if( (raw_server->tun_ctl_fd = open("/dev/net/tun", O_RDWR)) < 0 ) {
    log_it( L_ERROR,"Opening /dev/net/tun error: '%s'", strerror(errno) );
    return;
  }

  int err;
  memset( &raw_server->ifr, 0, sizeof(raw_server->ifr) );

  raw_server->ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

  if( (err = ioctl(raw_server->tun_ctl_fd, TUNSETIFF, (void *)&raw_server->ifr)) < 0 ) {
    log_it(L_CRITICAL, "ioctl(TUNSETIFF) error: '%s' ",strerror(errno));
    close(raw_server->tun_ctl_fd);
    raw_server->tun_ctl_fd = -1;
  }
  else {
    char buf[256];
    log_it( L_NOTICE,"Bringed up %s virtual network interface (%s/%s)", raw_server->ifr.ifr_name,inet_ntoa(raw_server->client_addr_host), l_vpn_mask );
    raw_server->tun_fd = raw_server->tun_ctl_fd; // Looks yes, its so

    dap_snprintf( buf, sizeof(buf), "ip link set %s up", raw_server->ifr.ifr_name );
    system( buf );
    dap_snprintf( buf, sizeof(buf),"ip addr add %s/%s dev %s ", inet_ntoa(raw_server->client_addr_host),l_vpn_mask, raw_server->ifr.ifr_name );
    system( buf );
  }
  #else

  raw_server->tun_fd = (HANDLE)SearchTapsWIN32( tun_create_WIN32, 0 );

  #endif
}

#ifndef _WIN32
void ch_sf_tun_destroy( )
{
//  close( raw_server->tun_fd );
  raw_server->tun_fd = -1;
}
#else
void ch_sf_tun_destroy( void )
{
  CloseHandle( raw_server->tun_fd );
  raw_server->tun_fd = NULL;

  CloseHandle( raw_server->tun_read_overlap.hEvent );
  raw_server->tun_read_overlap.hEvent = NULL;
}
#endif

/**
 * @brief stream_sf_new Callback to constructor of object of Ch
 * @param ch
 * @param arg
 */
void ch_sf_client_new( dap_stream_ch_t *ch , void *arg )
{
  dap_stream_ch_vpn_t *sf = calloc( 1, sizeof(dap_stream_ch_vpn_t) );

  ch->internal = sf;

  pthread_mutex_init( &sf->mutex, NULL );

  sf->raw_l3_sock = socket( PF_INET, SOCK_RAW, IPPROTO_RAW );
}

/**
 * @brief stream_sf_delete
 * @param ch
 * @param arg
 */
void ch_sf_delete( dap_stream_ch_t *ch , void *arg )
{
  log_it( L_DEBUG, "ch_sf_delete() for %s", ch->stream->conn->hostaddr );

  ch_vpn_socket_proxy_t *cur, *tmp;
  dap_stream_ch_vpn_remote_single_t *raw_client = 0;

  // in_addr_t raw_client_addr = DAP_STREAM_CH_VPN(ch)->tun_client_addr.s_addr;
  in_addr_t raw_client_addr = ch->stream->session->tun_client_addr.s_addr;

  if ( raw_client_addr ) {

    log_it( L_DEBUG,"ch_sf_delete() %s searching in hash table",
            inet_ntoa( ch->stream->session->tun_client_addr) );

    list_addr_element *el = (list_addr_element*)malloc( sizeof(list_addr_element) );

    //el->addr = DAP_STREAM_CH_VPN(ch)->tun_client_addr;
    el->addr = ch->stream->session->tun_client_addr;
    LL_APPEND( list_addr_head, el );
    //LL_FOREACH(list_addr_head,el) log_it(L_INFO,"addr = %s", inet_ntoa(el->addr));

    pthread_mutex_lock( &raw_server->clients_mutex );

    HASH_FIND_INT( raw_server->clients, &raw_client_addr, raw_client );

    if ( raw_client ) {
      HASH_DEL( raw_server->clients, raw_client );
      log_it( L_DEBUG, "ch_sf_delete() %s removed from hash table",
                   inet_ntoa(ch->stream->session->tun_client_addr));
      free( raw_client );
    } 
    else
      log_it( L_DEBUG,"ch_sf_delete() %s is not present in raw sockets hash table",
              inet_ntoa(ch->stream->session->tun_client_addr) );

    pthread_mutex_unlock(& raw_server->clients_mutex );
  }

  HASH_ITER( hh, DAP_STREAM_CH_VPN(ch)->socks ,cur, tmp ) {
    log_it( L_DEBUG, "delete socket: %i", cur->sock );
    HASH_DEL( DAP_STREAM_CH_VPN(ch)->socks, cur );
    if( cur )
      free( cur );
  }

  pthread_mutex_unlock( &(DAP_STREAM_CH_VPN(ch)->mutex) );

  if ( DAP_STREAM_CH_VPN(ch)->raw_l3_sock )
    close( DAP_STREAM_CH_VPN(ch)->raw_l3_sock );
}

void stream_sf_socket_delete( ch_vpn_socket_proxy_t *sf )
{
  if( !sf ) return;

  if ( sf->sock > 0 )
    close( sf->sock );

  pthread_mutex_destroy( &sf->mutex );

  free( sf );
}

void stream_sf_socket_ready_to_write( dap_stream_ch_t *ch, bool is_ready )
{
  pthread_mutex_lock( &ch->mutex );

  ch->ready_to_write = is_ready;

  if ( is_ready )
    ch->stream->conn_http->state_write = DAP_HTTP_CLIENT_STATE_DATA;

  dap_client_remote_ready_to_write( ch->stream->conn, is_ready );

  pthread_mutex_unlock( &ch->mutex );
}

ch_vpn_pkt_t *ch_sf_raw_read( )
{
  ch_vpn_pkt_t *ret = NULL;

  pthread_mutex_lock( &raw_server->pkt_out_mutex );

  if ( raw_server->pkt_out_rindex >= VPN_PKT_BUFFER_SIZE ) {
    raw_server->pkt_out_rindex = 0; // ring the buffer!
  }

  if ( (raw_server->pkt_out_rindex != raw_server->pkt_out_windex) || (raw_server->pkt_out_size == 0) ) {
    ret = raw_server->pkt_out[raw_server->pkt_out_rindex];
    raw_server->pkt_out_rindex ++;
    raw_server->pkt_out_size --;
  }
  else
    log_it( L_WARNING,"ch_sf_raw_read: Packet drop, ring buffer is full" );

  pthread_mutex_unlock( &raw_server->pkt_out_mutex );

  return ret;
}

int ch_sf_raw_write( uint8_t op_code, const void *data, size_t data_size )
{
  pthread_mutex_lock( &raw_server->pkt_out_mutex );

  if ( raw_server->pkt_out_windex >= VPN_PKT_BUFFER_SIZE )
    raw_server->pkt_out_windex = 0; // ring the buffer!

  if ( (raw_server->pkt_out_windex < raw_server->pkt_out_rindex) || !raw_server->pkt_out_size ) {

    ch_vpn_pkt_t *pkt = (ch_vpn_pkt_t *)calloc( 1, data_size + sizeof(pkt->header) );

    pkt->header.op_code = op_code;
    pkt->header.sock_id = (int32_t)raw_server->tun_fd;

    if ( data_size > 0 ) {
      pkt->header.op_data.data_size = data_size;
      memcpy( pkt->data, data, data_size );
    }

    raw_server->pkt_out[raw_server->pkt_out_windex] = pkt;
    raw_server->pkt_out_windex ++;
    raw_server->pkt_out_size ++;

    pthread_mutex_unlock( &raw_server->pkt_out_mutex );
    #ifndef _WIN32
      send_select_break( );
    #else
      SetEvent( hTunWriteEvent );
    #endif

    return raw_server->pkt_out_windex;
  }
  else {
    pthread_mutex_unlock( &raw_server->pkt_out_mutex );
    log_it( L_WARNING, "ch_sf_raw_write: Raw socket buffer overflow" );
    return -1;
  }
}

int stream_sf_socket_write( ch_vpn_socket_proxy_t *sf, uint8_t op_code, const void *data, size_t data_size )
{
  if ( sf->pkt_out_size >= PROXY_PKT_BUFFER_SIZE ) {
    return -1;
  }

  ch_vpn_pkt_t *pkt =(ch_vpn_pkt_t *)calloc( 1, data_size + sizeof(pkt->header) );

  pkt->header.op_code = op_code;
  pkt->header.sock_id = sf->id;

  switch ( op_code ) {

    case VPN_PACKET_OP_CODE_RECV:
    {
      pkt->header.op_data.data_size = data_size;
      memcpy( pkt->data, data, data_size );
    }
    break;
    default:
    {
      log_it( L_ERROR, "Unprocessed opcode %u for write to sf socket", op_code );
      free( pkt );
      return -2;
    }
  }

  sf->pkt_out[sf->pkt_out_size] = pkt;
  sf->pkt_out_size ++;

  return sf->pkt_out_size;
}

static bool client_connected = false;

//  VPN_PACKET_OP_CODE_VPN_ADDR_REQUEST:
static inline void  ch_sf_packet_ADDR_REQUEST( dap_stream_ch_t *ch, ch_vpn_pkt_t *sf_pkt )
{
  int remote_sock_id = sf_pkt->header.sock_id;
  struct in_addr n_addr;

  log_it( L_WARNING, "ch_sf_packet_ADDR_REQUEST dap_stream_ch_t *ch = %X ch_vpn_pkt_t *sf_pkt = %X ", ch, sf_pkt );

  n_addr.s_addr = 0;

  log_it( L_DEBUG, "Got SF packet with id %d op_code 0x%02x", remote_sock_id, sf_pkt->header.op_code );

  if ( n_addr.s_addr ) { // wtf ?

    log_it( L_WARNING, "All the network is filled with clients, can't lease a new address" );

    ch_vpn_pkt_t *pkt_out = (ch_vpn_pkt_t *)calloc( 1, sizeof(pkt_out->header) );

    pkt_out->header.sock_id = (int32_t)raw_server->tun_fd;
    pkt_out->header.op_code = VPN_PACKET_OP_CODE_PROBLEM;
    pkt_out->header.op_problem.code = VPN_PROBLEM_CODE_NO_FREE_ADDR;

    dap_stream_ch_pkt_write( ch, 'd', pkt_out,pkt_out->header.op_data.data_size + sizeof(pkt_out->header) );
    stream_sf_socket_ready_to_write( ch, true );

    return;
  }

  dap_stream_ch_vpn_remote_single_t *n_client = (dap_stream_ch_vpn_remote_single_t *)calloc( 1, sizeof(dap_stream_ch_vpn_remote_single_t) );
  if ( !n_client ) {

    log_it( L_WARNING, "ch_sf_packet_ADDR_REQUEST: out of memory" );
    return;
  }

  n_client->ch = ch;

  pthread_mutex_lock( &raw_server->clients_mutex );

  int count_free_addr = -1;

  list_addr_element *el;
  LL_COUNT( list_addr_head, el, count_free_addr );

  if ( count_free_addr > 0 ) {
    n_addr.s_addr = list_addr_head->addr.s_addr;
    LL_DELETE( list_addr_head, list_addr_head );
  }
  else {
    n_addr.s_addr = ntohl( raw_server->client_addr_last.s_addr );
    n_addr.s_addr ++;
    n_addr.s_addr = ntohl( n_addr.s_addr );
  }

  n_client->addr = n_addr.s_addr;
  raw_server->client_addr_last.s_addr = n_addr.s_addr;

  ch->stream->session->tun_client_addr.s_addr = n_addr.s_addr;

  HASH_ADD_INT( raw_server->clients, addr, n_client );
  pthread_mutex_unlock( &raw_server->clients_mutex );

  log_it( L_NOTICE, "VPN client address %s leased", inet_ntoa(n_addr) );
  log_it( L_INFO, "\tgateway %s", inet_ntoa(raw_server->client_addr_host) );
  log_it( L_INFO, "\tmask %s", inet_ntoa(raw_server->client_addr_mask) );
  log_it( L_INFO, "\taddr %s", inet_ntoa(raw_server->client_addr) );
  log_it( L_INFO, "\tlast_addr %s", inet_ntoa(raw_server->client_addr_last) );

  ch_vpn_pkt_t *pkt_out = (ch_vpn_pkt_t*) calloc( 1, sizeof(pkt_out->header) + sizeof(n_addr) + sizeof(raw_server->client_addr_host) );

  pkt_out->header.sock_id = (int32_t)raw_server->tun_fd;
  pkt_out->header.op_code = VPN_PACKET_OP_CODE_VPN_ADDR_REPLY;
  pkt_out->header.op_data.data_size = sizeof(n_addr) + sizeof( raw_server->client_addr_host );

  memcpy( pkt_out->data, &n_addr, sizeof(n_addr) );
  memcpy( pkt_out->data + sizeof(n_addr), &raw_server->client_addr_host, sizeof(raw_server->client_addr_host) );

  dap_stream_ch_pkt_write( ch, 'd', pkt_out, pkt_out->header.op_data.data_size + sizeof(pkt_out->header) );
  stream_sf_socket_ready_to_write( ch, true );

  log_it( L_WARNING, "ch_sf_packet_ADDR_REQUEST ok" );
#ifdef _WIN32
  Sleep( 3000 );
#else
  sleep( 3 );
#endif

  return;
}

//  VPN_PACKET_OP_CODE_VPN_SEND:
static inline void  ch_sf_packet_VPN_SEND( dap_stream_ch_t *ch, ch_vpn_pkt_t *sf_pkt )
{
//  int remote_sock_id = sf_pkt->header.sock_id;
  struct in_addr in_saddr, in_daddr;

  in_saddr.s_addr = ((struct iphdr*) sf_pkt->data)->saddr;
  in_daddr.s_addr = ((struct iphdr*) sf_pkt->data)->daddr;

  char str_daddr[42], str_saddr[42];

  strncpy( str_saddr, inet_ntoa(in_saddr), sizeof(str_saddr) );
  strncpy( str_daddr, inet_ntoa(in_daddr), sizeof(str_daddr) );

  int ret;
  //if( ch_sf_raw_write(STREAM_SF_PACKET_OP_CODE_RAW_SEND, sf_pkt->data, sf_pkt->op_data.data_size)<0){

  struct sockaddr_in sin = { 0 };
  sin.sin_family = AF_INET;
  sin.sin_port = 0;
  sin.sin_addr.s_addr = in_daddr.s_addr;

  //if((ret=sendto(DAP_STREAM_CH_VPN(ch)->raw_l3_sock , sf_pkt->data,sf_pkt->header.op_data.data_size,0,(struct sockaddr *) &sin, sizeof (sin)))<0){

  #ifndef _WIN32
    ret = write( raw_server->tun_fd, sf_pkt->data, sf_pkt->header.op_data.data_size );
  #else
    ret = win32_write_tun( sf_pkt->data, sf_pkt->header.op_data.data_size );
  #endif

  if ( ret < 0 ) {
    log_it( L_ERROR,"write() returned error %d : '%s'",ret, strerror(errno) );
    //log_it(L_ERROR,"raw socket ring buffer overflowed");

    ch_vpn_pkt_t *pkt_out = (ch_vpn_pkt_t *)calloc( 1, sizeof(pkt_out->header) );

    pkt_out->header.op_code = VPN_PACKET_OP_CODE_PROBLEM;
    pkt_out->header.op_problem.code = VPN_PROBLEM_CODE_PACKET_LOST;
    pkt_out->header.sock_id = (int32_t)raw_server->tun_fd;

    dap_stream_ch_pkt_write( ch, 'd', pkt_out, pkt_out->header.op_data.data_size + sizeof(pkt_out->header) );
    stream_sf_socket_ready_to_write( ch, true );

    return;
  }

  log_it( L_DEBUG, "Raw IP packet daddr:%s saddr:%s  %u from %d bytes sent to tun/tap interface",
          str_saddr, str_daddr, sf_pkt->header.op_data.data_size, ret );
  log_it( L_DEBUG,"Raw IP sent %u bytes ", ret );

  return;
}


//  VPN_PACKET_OP_CODE_SEND:
static inline void  ch_sf_packet_SEND( dap_stream_ch_t *ch, ch_vpn_pkt_t *sf_pkt, ch_vpn_socket_proxy_t *sf_sock )
{
  int remote_sock_id = sf_pkt->header.sock_id;

  if ( !client_connected ) {
    log_it( L_WARNING, "Drop Packet! User not connected!" ); // Client need send
    pthread_mutex_unlock( &sf_socks_mutex );
    return;
  }

  int ret = send( sf_sock->sock, (char *)&sf_pkt->data[0], sf_pkt->header.op_data.data_size, 0 );

  if ( ret < 0 ) {

    log_it( L_INFO, "Disconnected from the remote host" );

    pthread_mutex_unlock( &sf_sock->mutex );

    pthread_mutex_lock( &(DAP_STREAM_CH_VPN(ch)->mutex) );
    HASH_DEL( DAP_STREAM_CH_VPN(ch)->socks, sf_sock );
    pthread_mutex_unlock(& ( DAP_STREAM_CH_VPN(ch)->mutex ));

    pthread_mutex_lock( & sf_socks_mutex );

    HASH_DELETE( hh2, sf_socks, sf_sock );
    HASH_DELETE( hh_sock, sf_socks_client, sf_sock );

    struct epoll_event ev;

    ev.data.fd = sf_sock->sock;
    ev.events = EPOLLIN;

    if ( epoll_ctl( sf_socks_epoll_fd, EPOLL_CTL_DEL, sf_sock->sock, &ev) < 0 ) {
      log_it( L_ERROR, "Can't remove sock_id %d from the epoll fd", remote_sock_id );
      //stream_ch_pkt_write_f(ch,'i',"sock_id=%d op_code=0x%02x result=-2",sf_pkt->sock_id, sf_pkt->op_code);
    }
    else {
      log_it( L_NOTICE, "Removed sock_id %d from the the epoll fd", remote_sock_id );
      //stream_ch_pkt_write_f(ch,'i',"sock_id=%d op_code=0x%02x result=0",sf_pkt->sock_id, sf_pkt->op_code);
    }

    pthread_mutex_unlock( &sf_socks_mutex );
    stream_sf_socket_delete( sf_sock );

    return;
  }

  sf_sock->bytes_sent += ret;
  pthread_mutex_unlock( &sf_sock->mutex );

//  log_it( L_INFO, "Send action from %d sock_id (sf_packet size %lu,  ch packet size %lu, have sent %d)",
//                   sf_sock->id, sf_pkt->header.op_data.data_size, pkt->hdr.size, ret );
  log_it( L_INFO, "Send action from %d sock_id (sf_packet size %lu,  ch packet size ?, have sent %d)",
                   sf_sock->id, sf_pkt->header.op_data.data_size, ret );

//dap_stream_ch_pkt_t *pkt

  return;
}

//  VPN_PACKET_OP_CODE_DISCONNECT:
static inline void  ch_sf_packet_DISCONNECT( dap_stream_ch_t *ch, ch_vpn_pkt_t *sf_pkt, ch_vpn_socket_proxy_t *sf_sock )
{
  int remote_sock_id = sf_pkt->header.sock_id;

  log_it( L_INFO, "Disconnect action from %d sock_id", sf_sock->id );

  pthread_mutex_lock( &(DAP_STREAM_CH_VPN(ch)->mutex) );
  HASH_DEL(DAP_STREAM_CH_VPN(ch)->socks,sf_sock);
  pthread_mutex_unlock( &(DAP_STREAM_CH_VPN(ch)->mutex) );

  pthread_mutex_lock( &sf_socks_mutex );
  HASH_DELETE( hh2, sf_socks, sf_sock );
  HASH_DELETE( hh_sock, sf_socks_client, sf_sock );

  struct epoll_event ev;
  ev.data.fd = sf_sock->sock;
  ev.events = EPOLLIN;

  if ( epoll_ctl(sf_socks_epoll_fd, EPOLL_CTL_DEL, sf_sock->sock, &ev) < 0 ) {
    log_it( L_ERROR, "Can't remove sock_id %d to the epoll fd", remote_sock_id );
    //stream_ch_pkt_write_f(ch,'i',"sock_id=%d op_code=%uc result=-2",sf_pkt->sock_id, sf_pkt->op_code);
  }
  else {
    log_it( L_NOTICE, "Removed sock_id %d from the epoll fd", remote_sock_id );
    //stream_ch_pkt_write_f(ch,'i',"sock_id=%d op_code=%uc result=0",sf_pkt->sock_id, sf_pkt->op_code);
  }

  pthread_mutex_unlock( &sf_socks_mutex );
  pthread_mutex_unlock( &sf_sock->mutex );

  stream_sf_socket_delete( sf_sock );

  return;
}

//  VPN_PACKET_OP_CODE_CONNECT:
static inline void  ch_sf_packet_CONNECT( dap_stream_ch_t *ch, ch_vpn_pkt_t *sf_pkt )
{
  int remote_sock_id = sf_pkt->header.sock_id;
  ch_vpn_socket_proxy_t *sf_sock = NULL;

  HASH_FIND_INT( DAP_STREAM_CH_VPN(ch)->socks, &remote_sock_id, sf_sock );

  if ( sf_sock ) {
    log_it( L_WARNING, "Socket id %d is already used, take another number for socket id", remote_sock_id );
    return;
  }

  struct sockaddr_in remote_addr;
  char addr_str[1024];

  size_t addr_str_size = (sf_pkt->header.op_connect.addr_size > (sizeof(addr_str)-1)) ? (sizeof(addr_str)-1):
                             sf_pkt->header.op_connect.addr_size;

  memset( &remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons( sf_pkt->header.op_connect.port );

  memcpy( addr_str, sf_pkt->data, addr_str_size );
  addr_str[addr_str_size] = 0;

  log_it( L_DEBUG, "Connect action to %s:%u (addr_size %lu)", addr_str, sf_pkt->header.op_connect.port,
          sf_pkt->header.op_connect.addr_size );

  if ( inet_pton(AF_INET,addr_str, &remote_addr.sin_addr) < 0 ) {

    log_it( L_ERROR, "Wrong remote address '%s:%u'", addr_str, sf_pkt->header.op_connect.port );
    return;
  }

  int s = socket( AF_INET, SOCK_STREAM, 0 );

  if ( s < 0 ) {
    log_it( L_ERROR, "Can't create the socket" );
    return;
  }

  log_it( L_DEBUG, "Socket is created (%d)", s );

  if ( connect(s, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) < 0 ) {

    log_it( L_INFO, "Can't connect to the remote server %s", addr_str );

    dap_stream_ch_pkt_write_f( ch, 'i', "sock_id=%d op_code=%c result=-1", sf_pkt->header.sock_id, sf_pkt->header.op_code );
    stream_sf_socket_ready_to_write( ch, true );

    close( s );
    return;
  }

  #ifdef _WIN32
    unsigned long arg = 1;
    ioctlsocket( s, FIONBIO, &arg );
  #else
    fcntl( s, F_SETFL, O_NONBLOCK );
  #endif

  log_it( L_INFO, "Remote address connected (%s:%u) with sock_id %d", addr_str, sf_pkt->header.op_connect.port, remote_sock_id );

//  ch_vpn_socket_proxy_t *sf_sock = NULL;

  sf_sock = DAP_NEW_Z( ch_vpn_socket_proxy_t );

  sf_sock->id = remote_sock_id;
  sf_sock->sock = s;
  sf_sock->ch = ch;

  pthread_mutex_init( &sf_sock->mutex, NULL );

  pthread_mutex_lock( &sf_socks_mutex );
  pthread_mutex_lock( &(DAP_STREAM_CH_VPN(ch)->mutex) );

  HASH_ADD_INT( DAP_STREAM_CH_VPN(ch)->socks, id, sf_sock );
  log_it( L_DEBUG, "Added %d sock_id with sock %d to the hash table", sf_sock->id, sf_sock->sock );

  HASH_ADD( hh2, sf_socks, id,sizeof(sf_sock->id), sf_sock );
  log_it( L_DEBUG, "Added %d sock_id with sock %d to the hash table", sf_sock->id, sf_sock->sock );

  HASH_ADD( hh_sock, sf_socks_client, sock,sizeof(int), sf_sock );
  //log_it(L_DEBUG,"Added %d sock_id with sock %d to the socks hash table",sf->id,sf->sock);

  pthread_mutex_unlock( &sf_socks_mutex );
  pthread_mutex_unlock( &(DAP_STREAM_CH_VPN(ch)->mutex) );

  struct epoll_event ev;
  ev.data.fd = s;
  ev.events = EPOLLIN | EPOLLERR;

  if ( epoll_ctl(sf_socks_epoll_fd, EPOLL_CTL_ADD, s, &ev) == -1 ) {
    log_it( L_ERROR, "Can't add sock_id %d to the epoll fd", remote_sock_id );
    //stream_ch_pkt_write_f(ch,'i',"sock_id=%d op_code=%uc result=-2",sf_pkt->sock_id, sf_pkt->op_code);
  }
  else {
    log_it( L_NOTICE, "Added sock_id %d  with sock %d to the epoll fd", remote_sock_id, s );
    log_it( L_NOTICE, "Send Connected packet to User" );

    ch_vpn_pkt_t *pkt_out = (ch_vpn_pkt_t*) calloc( 1, sizeof(pkt_out->header) );

    pkt_out->header.sock_id = remote_sock_id;
    pkt_out->header.op_code = VPN_PACKET_OP_CODE_CONNECTED;
    dap_stream_ch_pkt_write( ch,'s', pkt_out, pkt_out->header.op_data.data_size + sizeof(pkt_out->header) );

    free( pkt_out );
    client_connected = true;
  }

  stream_sf_socket_ready_to_write( ch, true );

  return;
}


/**
 * @brief stream_sf_packet_in
 * @param ch
 * @param arg
 */
void ch_sf_packet_in( dap_stream_ch_t *ch, void *arg )
{
  dap_stream_ch_pkt_t *pkt = (dap_stream_ch_pkt_t *)arg;

  // log_it(L_DEBUG,"stream_sf_packet_in:  channel packet hdr size %lu ( last bytes 0x%02x 0x%02x 0x%02x 0x%02x ) ", pkt->hdr.size,
  //        *((uint8_t *)pkt->data + pkt->hdr.size-4),*((uint8_t *)pkt->data + pkt->hdr.size-3)
  //        ,*((uint8_t *)pkt->data + pkt->hdr.size-2),*((uint8_t *)pkt->data + pkt->hdr.size-1)
  //        );

  ch_vpn_pkt_t *sf_pkt = (ch_vpn_pkt_t *)pkt->data;

  int remote_sock_id = sf_pkt->header.sock_id;

  //log_it(L_DEBUG,"Got SF packet with id %d op_code 0x%02x",remote_sock_id, sf_pkt->header.op_code );

  if ( sf_pkt->header.op_code >= 0xb0 ) { // Raw packets

    switch( sf_pkt->header.op_code ) {
    case VPN_PACKET_OP_CODE_VPN_ADDR_REQUEST: 
      ch_sf_packet_ADDR_REQUEST( ch, sf_pkt );
    break;
    case VPN_PACKET_OP_CODE_VPN_SEND:
      ch_sf_packet_VPN_SEND( ch, sf_pkt );
    break;
    default:
      log_it( L_WARNING, "Can't process SF type 0x%02x", sf_pkt->header.op_code );
    break;
    }

    return;
  }

  if ( sf_pkt->header.op_code == VPN_PACKET_OP_CODE_CONNECT ) {

    ch_sf_packet_CONNECT( ch, sf_pkt );
    return;
  }

  ch_vpn_socket_proxy_t *sf_sock = NULL;

  pthread_mutex_lock( &(DAP_STREAM_CH_VPN(ch)->mutex) );
  //log_it(L_DEBUG,"Looking in hash table with %d",remote_sock_id);
  HASH_FIND_INT( (DAP_STREAM_CH_VPN(ch)->socks), &remote_sock_id, sf_sock );
  pthread_mutex_unlock( &(DAP_STREAM_CH_VPN(ch)->mutex) );

  if ( !sf_sock ) {

    log_it( L_WARNING, "Packet input: packet with sock_id %d thats not present in current stream channel", remote_sock_id );
    return;
  }

  pthread_mutex_lock( &sf_sock->mutex ); // Unlock it in your case as soon as possible to reduce lock time
  sf_sock->time_lastused = time( NULL );

  switch( sf_pkt->header.op_code ) {

  case VPN_PACKET_OP_CODE_SEND:
    ch_sf_packet_SEND( ch, sf_pkt, sf_sock );
  break;

  case VPN_PACKET_OP_CODE_DISCONNECT:
    ch_sf_packet_DISCONNECT( ch, sf_pkt, sf_sock );
  break;
  default: {
    log_it( L_WARNING, "Unprocessed op code 0x%02x", sf_pkt->header.op_code );
    pthread_mutex_unlock( &sf_sock->mutex );
  }
  }

  return;
}


/**
 * @brief stream_sf_disconnect
 * @param sf
 */
void stream_sf_disconnect( ch_vpn_socket_proxy_t *sf_sock )
{
  struct epoll_event ev;

  ev.data.fd = sf_sock->sock;
  ev.events = EPOLLIN | EPOLLERR;

  if ( epoll_ctl(sf_socks_epoll_fd, EPOLL_CTL_DEL, sf_sock->sock, &ev) == -1 ) {
    log_it(L_ERROR,"Can't del sock_id %d from the epoll fd",sf_sock->id);
      //stream_ch_pkt_write_f(sf->ch,'i',"sock_id=%d op_code=%uc result=-1",sf->id, STREAM_SF_PACKET_OP_CODE_RECV);
  }
  else {
        log_it(L_ERROR,"Removed sock_id %d from the epoll fd",sf_sock->id);
      //stream_ch_pkt_write_f(sf->ch,'i',"sock_id=%d op_code=%uc result=0",sf->id, STREAM_SF_PACKET_OP_CODE_RECV);
  }

    // Compise signal to disconnect to another side, with special opcode STREAM_SF_PACKET_OP_CODE_DISCONNECT
  ch_vpn_pkt_t * pkt_out;

  pkt_out = (ch_vpn_pkt_t*) calloc( 1, sizeof(pkt_out->header) + 1 );

  pkt_out->header.op_code = VPN_PACKET_OP_CODE_DISCONNECT;
  pkt_out->header.sock_id = sf_sock->id;
  sf_sock->pkt_out[sf_sock->pkt_out_size] = pkt_out;
  sf_sock->pkt_out_size ++;
  sf_sock->signal_to_delete = true;
}


/**

Socket forward
**/

void *ch_sf_thread(void * arg)
{
  uint32_t  numfails = 0;
  struct epoll_event ev, events[SF_MAX_EVENTS];
  //pthread_mutex_lock(&sf_socks_mutex);

  memset( &events[0], 0, sizeof(struct epoll_event) * SF_MAX_EVENTS );

  sf_socks_epoll_fd = epoll_create( SF_MAX_EVENTS );

  if ( (intptr_t)sf_socks_epoll_fd == -1 ) {

    log_it( L_ERROR, "epoll_create return -1" );
    return (void *)-1;
  }

  #ifndef _WIN32
    sigset_t sf_sigmask;
    sigemptyset( &sf_sigmask );
    sigaddset( &sf_sigmask, SIGUSR2 );
  #endif

  while( bQuitSignal ) {
    /*pthread_mutex_lock(&sf_socks_mutex);
    if(sf_socks==NULL)
      pthread_cond_wait(&sf_socks_cond,&sf_socks_mutex);
    pthread_mutex_unlock(&sf_socks_mutex);*/

    #ifndef _WIN32
      int nfds = epoll_pwait( sf_socks_epoll_fd, events, SF_MAX_EVENTS, 10000, &sf_sigmask );
      #else
      int nfds = epoll_wait( sf_socks_epoll_fd, events, SF_MAX_EVENTS, 1000 );
    #endif

    if ( nfds < 0 ) {
      log_it( L_CRITICAL,"Can't run epoll_wait: %s", strerror(errno) );
      #ifdef _WIN32
        Sleep( 10000 );
      #else
        sleep( 10 );
      #endif
      if ( numfails ++ >= 3 ) {
        log_it( L_NOTICE, "ch_sf_thread: exit" );
        break;
      }
      continue;
    }

    if( nfds > 0 )
      log_it( L_DEBUG,"Epolled %d fd", nfds );

    int n;

    for ( n = 0; n < nfds; ++ n ) {

      int s = events[n].data.fd;

      ch_vpn_socket_proxy_t *sf = NULL;

      pthread_mutex_lock( &sf_socks_mutex );
      HASH_FIND( hh_sock, sf_socks_client ,&s, sizeof(s), sf );
      pthread_mutex_unlock( &sf_socks_mutex );

      if ( !sf ) {

        if ( epoll_ctl(sf_socks_epoll_fd, EPOLL_CTL_DEL, s, &ev) < 0 )
          log_it(L_ERROR,"Can't remove sock_id %d to the epoll fd",s);
         else
           log_it(L_NOTICE,"Socket id %d is removed from the list",s);

        continue;
      }

      if ( events[n].events & EPOLLERR ) {

          log_it(L_NOTICE,"Socket id %d has EPOLLERR flag on",s);
          pthread_mutex_lock(& (sf->mutex) );
          stream_sf_disconnect(sf);
          pthread_mutex_unlock(& (sf->mutex) );

      } else if ( events[n].events & EPOLLIN ) {

        char buf[1024 * 512];
        size_t buf_size;
        ssize_t ret;

        pthread_mutex_lock( &(sf->mutex) );

        if ( sf->pkt_out_size >= PROXY_PKT_BUFFER_SIZE - 1 ) {
          log_it( L_WARNING, "Can't receive data, full of stack" );
          pthread_mutex_unlock( &(sf->mutex) );
          continue;
        }

        ret = recv( sf->sock, buf, sizeof(buf), 0 );
         //log_it(L_DEBUG,"recv() returned %d",ret);

        if ( ret > 0 ) {

          buf_size = ret;
          ch_vpn_pkt_t *pout;

          pout = sf->pkt_out[sf->pkt_out_size] = (ch_vpn_pkt_t *)calloc( 1, buf_size + sizeof(pout->header) );
          pout->header.op_code = VPN_PACKET_OP_CODE_RECV;
          pout->header.sock_id = sf->id;
          pout->header.op_data.data_size = buf_size;

          memcpy( pout->data, buf, buf_size );
          sf->pkt_out_size ++;

          pthread_mutex_unlock(& (sf->mutex) );
          stream_sf_socket_ready_to_write( sf->ch, true );

        } else {
          log_it( L_NOTICE, "Socket id %d returned error on recv() function - may be host has disconnected", s );
          pthread_mutex_unlock(& (sf->mutex) );
          stream_sf_socket_ready_to_write( sf->ch, true );
          stream_sf_disconnect( sf );
        }
      } // epoll in
      else {
        log_it(L_WARNING,"Unprocessed flags 0x%08X",events[n].events);
      }
    } // for nfds

   //pthread_mutex_unlock(&sf_socks_mutex);
  } // while

  return 0;
}


/**
 *
 *
 **/
void *ch_sf_thread_raw( void *arg )
{
  ch_sf_tun_create( );

  if ( raw_server->tun_fd <= 0 ) {
    log_it( L_CRITICAL,"Tun/tap file descriptor is not initialized" );
    return NULL;
  }

  /*    if (fcntl(raw_server->tun_fd, F_SETFL, O_NONBLOCK) < 0){ ;
          log_it(L_CRITICAL,"Can't switch tun/tap socket into the non-block mode");
          return NULL;
      }
      if (fcntl(raw_server->tun_fd, F_SETFD, FD_CLOEXEC) < 0){;
          log_it(L_CRITICAL,"Can't switch tun/tap socket to not be passed across execs");
          return NULL;
      }
  */

  uint8_t *tmp_buf;
  ssize_t tmp_buf_size ;
  static int tun_MTU = 100000; /// TODO Replace with detection of MTU size

  tmp_buf = (uint8_t *)calloc( 1, tun_MTU );
  tmp_buf_size = 0;
  log_it( L_INFO,"Tun/tap thread starts with MTU = %d", tun_MTU );

  #ifndef _WIN32
    fd_set fds_read, fds_read_active;

    FD_ZERO( &fds_read );
    FD_SET( raw_server->tun_fd, &fds_read );
    FD_SET( get_select_breaker(), &fds_read );
  #else

    HANDLE events[3];
    int num_events = 2;

    events[0] = raw_server->tun_read_overlap.hEvent;
    events[1] = hTunWriteEvent;
    events[2] = hTerminateEvent;

  #endif

  /// Main cycle
  do {

    #ifndef _WIN32
      fds_read_active = fds_read;
      int ret = select( FD_SETSIZE, &fds_read_active, NULL, NULL, NULL );
    #else
      int ret = WaitForMultipleObjects( num_events, events, FALSE, INFINITE );
    #endif

    if ( ret <= 0 ) {
      log_it( L_CRITICAL, "Select returned %d", ret );
      break;
    }

    #ifndef _WIN32
      if ( FD_ISSET(get_select_breaker(), &fds_read_active) ) { // Smth to send
    #else
      if ( ret == WAIT_OBJECT_0 + 1 ) {
    #endif

      ch_vpn_pkt_t *pkt = ch_sf_raw_read( );

      if ( pkt ) {
        #ifndef _WIN32
          int write_ret = write( raw_server->tun_fd, pkt->data, pkt->header.op_data.data_size );
        #else
          int write_ret = win32_write_tun( pkt->data, pkt->header.op_data.data_size );
        #endif

        if ( write_ret > 0 )
          log_it( L_DEBUG, "Wrote out %d bytes to the tun/tap interface", write_ret );
        else
          log_it( L_ERROR,"Tun/tap write %u bytes returned '%s' error, code (%d)", pkt->header.op_data.data_size, strerror(errno), write_ret ) ;
      }
    }

    #ifndef _WIN32
      if ( FD_ISSET(raw_server->tun_fd, &fds_read_active) ) {
    #else
      if ( ret == WAIT_OBJECT_0 ) {
    #endif

      #ifndef _WIN32
        int read_ret = read( raw_server->tun_fd, tmp_buf, tun_MTU );
      #else
        int read_ret = win32_read_tun( tmp_buf, tun_MTU );
      #endif

      if ( read_ret < 0 ) {
        log_it( L_CRITICAL, "Tun/tap read returned '%s' error, code (%d)", strerror(errno), read_ret ) ;
        break;
      }

      struct iphdr *iph = (struct iphdr* ) tmp_buf;
      struct in_addr in_daddr, in_saddr;

      in_daddr.s_addr = iph->daddr;
      in_saddr.s_addr = iph->saddr;

      char str_daddr[42],str_saddr[42];
      strncpy( str_saddr, inet_ntoa(in_saddr), sizeof(str_saddr) );
      strncpy( str_daddr, inet_ntoa(in_daddr), sizeof(str_daddr) );

        /*if(iph->tot_len > (uint16_t) read_ret ){
            log_it(L_INFO,"Tun/Tap interface returned only the fragment (tot_len =%u  read_ret=%d) ",
                iph->tot_len,read_ret);
          }*/
        /*if(iph->tot_len < (uint16_t) read_ret ){
            log_it(L_WARNING,"Tun/Tap interface returned more then one packet (tot_len =%u  read_ret=%d) ",
                  iph->tot_len,read_ret);
        }*/

        //log_it(L_DEBUG,"Read IP packet from tun/tap interface daddr=%s saddr=%s total_size = %d "
        //  ,str_daddr,str_saddr,read_ret);

      dap_stream_ch_vpn_remote_single_t *raw_client = NULL;
      pthread_mutex_lock( &raw_server->clients_mutex );
      HASH_FIND_INT( raw_server->clients, &in_daddr.s_addr, raw_client );

      //                  HASH_ADD_INT(DAP_STREAM_CH_VPN(ch)->socks, id, sf_sock );
      //                  HASH_DEL(DAP_STREAM_CH_VPN(ch)->socks,sf_sock);

      if ( raw_client ) { // Is present in hash table such destination address

        ch_vpn_pkt_t *pkt_out = (ch_vpn_pkt_t *)calloc( 1, sizeof(pkt_out->header) + read_ret );

        pkt_out->header.op_code = VPN_PACKET_OP_CODE_VPN_RECV;
        pkt_out->header.sock_id = (int32_t)raw_server->tun_fd;
        pkt_out->header.op_data.data_size = read_ret;

        memcpy( pkt_out->data, tmp_buf, read_ret );

        dap_stream_ch_pkt_write( raw_client->ch, 'd', pkt_out, pkt_out->header.op_data.data_size + sizeof(pkt_out->header) );
        stream_sf_socket_ready_to_write( raw_client->ch, true );

      }
      else {
          // log_it(L_DEBUG,"No remote client for income IP packet with addr %s",inet_ntoa(in_daddr));
      }
        
      pthread_mutex_unlock(& raw_server->clients_mutex );
    } // fds_read_active
    #ifdef _WIN32
      else if ( ret == WAIT_OBJECT_0 + 2 ) break;
    #endif

  } while( bQuitSignal );

  log_it( L_NOTICE, "Raw sockets listen thread is stopped" );

  ch_sf_tun_destroy( );
  return NULL;
}



/**
 * @brief stream_sf_packet_out Packet Out Ch callback
 * @param ch
 * @param arg
 */
void ch_sf_packet_out( dap_stream_ch_t *ch , void *arg )
{
  ch_vpn_socket_proxy_t * cur, *tmp;
  bool isSmthOut = false;
//    log_it(L_DEBUG,"Socket forwarding packet out callback: %u sockets in hashtable", HASH_COUNT(DAP_STREAM_CH_VPN(ch)->socks) );

  HASH_ITER( hh, DAP_STREAM_CH_VPN(ch)->socks , cur, tmp ) {

    bool signalToBreak = false;

    int i;
    pthread_mutex_lock( &cur->mutex );

    log_it(L_DEBUG,"Socket with id %d has %u packets in output buffer", cur->id, cur->pkt_out_size );

    if ( cur->pkt_out_size ) {

      for( i = 0; i < cur->pkt_out_size; i ++ ) {
        ch_vpn_pkt_t *pout = cur->pkt_out[i];

        if ( !pout ) 
          continue; 

        if ( dap_stream_ch_pkt_write(ch,'d',pout,pout->header.op_data.data_size+sizeof(pout->header)) ) {
          isSmthOut = true;
          free(pout);
          cur->pkt_out[i]=NULL;
        }
        else {
          log_it(L_WARNING, "Buffer is overflowed, breaking cycle to let the upper level cycle drop data to the output socket");
          isSmthOut=true;
          signalToBreak=true;
          break;
        }
      }
    }

    if ( signalToBreak ) {
      pthread_mutex_unlock( &cur->mutex );
      break;
    }

    cur->pkt_out_size = 0;

    if ( cur->signal_to_delete ) {

      log_it( L_NOTICE,"Socket id %d got signal to be deleted", cur->id );

      pthread_mutex_lock( &( DAP_STREAM_CH_VPN(ch)->mutex ) );
      HASH_DEL( DAP_STREAM_CH_VPN(ch)->socks, cur );
      pthread_mutex_unlock(&( DAP_STREAM_CH_VPN(ch)->mutex ));

      pthread_mutex_lock(&(sf_socks_mutex));
      HASH_DELETE(hh2,sf_socks,cur);
      HASH_DELETE(hh_sock,sf_socks_client,cur);
      pthread_mutex_unlock(&(sf_socks_mutex));

      pthread_mutex_unlock(&(cur->mutex));
      stream_sf_socket_delete(cur);
    }
    else
      pthread_mutex_unlock(&(cur->mutex));
  }

  ch->ready_to_write = isSmthOut;

  if ( isSmthOut ) {
    ch->stream->conn_http->state_write=DAP_HTTP_CLIENT_STATE_DATA;
  }

  dap_client_remote_ready_to_write( ch->stream->conn, isSmthOut );
}

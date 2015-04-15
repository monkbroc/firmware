/**
 ******************************************************************************
 * @file    socket_hal.c
 * @author  Matthew McGowan
 * @version V1.0.0
 * @date    09-Nov-2014
 * @brief
 ******************************************************************************
  Copyright (c) 2013-14 Spark Labs, Inc.  All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */

#include "socket_hal.h"
#include "wiced.h"
#include "service_debug.h"
#include "spark_macros.h"
#include <algorithm>

#include <vector>

/**
 * Socket handles 
 * --------------
 * 
 * Each socket handle is a pointer to a dynamically allocated instance of socket_t.
 * This is so we don't impose any additional limits on the number of open sockets.
 * 
 * The golden rule is that the socket_t instance is not deallocated until the caller
 * issues a socket_close() call. Specifically, if a client socket is closed by the other end,
 * the handle remains valid, although attempts to perform any socket IO will fail.
 * The handle isn't deallocated until the caller issues a socket_close() call.
 */

/**
 * int32_t negative values are used for errors.
 * Since all handles are allocated in RAM, they will be in the
 * 0x20xxxxxx range. 
 */
const sock_handle_t SOCKET_MAX = 0x7FFFFFFF;

/**
 * The handle value returned when a socket cannot be created. 
 */
const sock_handle_t SOCKET_INVALID = (sock_handle_t)-1;

/**
 * Manages reading from a tcp packet. 
 */
struct tcp_packet_t
{
    /**
     * Any outstanding packet to retrieve data from.
     */
    wiced_packet_t* packet;
    
    /**
     * The current offset of data already read from the packet.
     */    
    unsigned offset;
    
    tcp_packet_t() {}
    
    ~tcp_packet_t() {
        dispose_packet();
    }
    
    void dispose_packet() {
        if (packet) {
            wiced_packet_delete(packet);
            packet = NULL;
            offset = 0;
        }
    }
        
};

/**
 * The info we maintain for each socket. It wraps a WICED socket. 
 *  
 */
struct tcp_socket_t : wiced_tcp_socket_t {
    tcp_packet_t packet;
    
    void close()
    {
        wiced_tcp_disconnect(this);
        wiced_tcp_delete_socket(this);
    }
};

struct udp_socket_t : wiced_udp_socket_t 
{
    void close()
    {        
        wiced_udp_delete_socket(this);
    }        
};

struct tcp_server_t;

/**
 * The handle that we provide to external clients. This ensures 
 */
struct tcp_server_client_t
{
    wiced_tcp_stream_t* stream;
    wiced_tcp_socket_t* socket;
    tcp_server_t* server;
    
    tcp_server_client_t(tcp_server_t* server, wiced_tcp_socket_t* socket) {
        this->socket = socket;
        this->server = server;
        this->stream = NULL;
    }
    
    int read(void* buffer, size_t len, system_tick_t timeout) {
        if (socket) {
            
        }
        return 0;   // todo
    }
    
    int write(void* buffer, size_t len) {
        return 0; //todo
    }
    
    void close();
            
    void notify_disconnected()
    {
        socket = NULL;
        server = NULL;
    }
    
    ~tcp_server_client_t();
};

struct tcp_server_t : wiced_tcp_server_t
{
    tcp_server_t() {
        wiced_rtos_init_semaphore(&accept_lock);
        memset(clients, 0, sizeof(clients));
    }
    
    ~tcp_server_t() {
        wiced_rtos_deinit_semaphore(&accept_lock);
    }
        
    /**
     * Find the index of the given client socket in our list of client sockets.
     * @param socket The socket to find.
     * @return The index of the socket (>=0) or -1 if not found.
     */       
    int index(wiced_tcp_socket_t* socket) {
        return (is_client(socket)) ? socket-this->socket : -1;
    }
    
    /**
     * Determines if the given socket is a client socket associated with this server
     * socket.
     * @param socket
     * @return {@code true} if the given socket is a client. 
     */
    bool is_client(wiced_tcp_socket_t* socket) {
        // see if the address corresponds to the socket array
        return this->socket<=socket && socket<this->socket+arraySize(this->socket);
    }
    
    wiced_result_t accept(wiced_tcp_socket_t* socket) {
        wiced_result_t result;
        if ((result=wiced_tcp_accept(socket))==WICED_SUCCESS) {
            wiced_rtos_get_semaphore(&accept_lock, WICED_WAIT_FOREVER);
            
            int idx = index(socket); 
            if (idx>=0) {
                clients[idx] = new tcp_server_client_t(this, socket);
                to_accept.insert(to_accept.end(), idx);
            }
            wiced_rtos_set_semaphore(&accept_lock);
        }
        return result;
    }

    /**
     * Fetches the next client socket from the accept queue.
     * @return The next client, or NULL
     */
    tcp_server_client_t* next_accept() {
        wiced_rtos_get_semaphore(&accept_lock, WICED_WAIT_FOREVER);                
        int index = -1;
        if (to_accept.size()) {
            index = *to_accept.begin();
            to_accept.erase(to_accept.begin());
        }
        wiced_rtos_set_semaphore(&accept_lock);        
        return index>=0 ? clients[index] : NULL;
    }
    
    wiced_result_t disconnect(wiced_tcp_socket_t* socket) {
        wiced_rtos_get_semaphore(&accept_lock, WICED_WAIT_FOREVER);        
        int idx = index(socket);
        tcp_server_client_t* client = clients[idx];
        if (client)
            client->notify_disconnected();
        wiced_result_t result = wiced_tcp_server_disconnect_socket(this, socket);
        wiced_rtos_set_semaphore(&accept_lock);
        return result;
    }
    
    void close() {
        // close all clients first
        for (int i=0; i<WICED_MAXIMUM_NUMBER_OF_SERVER_SOCKETS; i++) {
            tcp_server_client_t* client = clients[i];
            if (client)
                client->close();
        }
        wiced_tcp_server_stop(this);
    }
    
    
private:
    // for each server instance, maintain an associated tcp_server_client_t instance
    tcp_server_client_t* clients[WICED_MAXIMUM_NUMBER_OF_SERVER_SOCKETS];

    wiced_semaphore_t accept_lock;
    std::vector<int> to_accept;
};

void tcp_server_client_t::close() {
    if (socket && server) {
        server->disconnect(socket);
    }
}


tcp_server_client_t::~tcp_server_client_t() {
    if (server) {
        server->disconnect(socket);
    }
    if (stream) {

    }
}


struct socket_t
{
    enum socket_type_t {
        NONE, TCP, UDP, TCP_SERVER, TCP_CLIENT
    };
    
    uint8_t type;
    bool closed;
    socket_t* next;
    
    union all {
        tcp_socket_t tcp;
        udp_socket_t udp;
        tcp_server_t* tcp_server;
        tcp_server_client_t* tcp_client;
        
        all() {}
        ~all() {}
    } s;    
    
    socket_t() {
        memset(this, 0, sizeof(*this));
    }
    
    void set_server(tcp_server_t* server) {
        type = TCP_SERVER;
        s.tcp_server = server;
    }
    
    void set_client(tcp_server_client_t* client) {
        type = TCP_CLIENT;
        s.tcp_client = client;
    }
    
   ~socket_t() {
       if (!closed)
            close();
       switch (type) {
           case TCP:
               s.tcp.~tcp_socket_t();
               break;
            case UDP:
           s.udp.~udp_socket_t();
               break;
           case TCP_SERVER:
               delete s.tcp_server;
               break;
           case TCP_CLIENT:
               delete s.tcp_client;
               break;
       }
   }

   void close() {
       switch (type) {
           case TCP:
               s.tcp.close();
               break;
            case UDP:
               s.udp.close();
               break;
           case TCP_SERVER:
               s.tcp_server->close();
               break;
           case TCP_CLIENT:
               s.tcp_client->close();
               break;           
       }       
       closed = true;
   }
};

/**
 * Singly linked lists for servers and clients. Ensures we can completely shutdown
 * the socket layer when entering listening mode. 
 */
socket_t* servers = NULL;
socket_t* clients = NULL;

/**
 * Adds an item to the linked list.
 * @param item
 * @param list
 */
void add_list(socket_t* item, socket_t*& list) {
    item->next = list;
    list = item;
}

/**
 * Removes an item from the linked list.
 * @param item
 * @param list
 */

void remove_list(socket_t* item, socket_t*& list) 
{
    if (list==item) {
        list = item->next;
    }
    else
    {
        socket_t* current = list;
        while (current) {
            if (current->next==item) {
                current->next = item->next;
                break;
            }
        }
    }    
}


socket_t*& list_for_socket(socket_t* socket) {
    return (socket->type==socket_t::TCP_SERVER) ? servers : clients;
}

void add(socket_t* socket) {
    if (socket) {
        add_list(socket, list_for_socket(socket)); 
    }
}

void remove(socket_t* socket) {
    if (socket) {
        remove_list(socket, list_for_socket(socket));
    }
}


inline bool is_udp(socket_t* socket) { return socket && socket->type==socket_t::UDP; }
inline bool is_tcp(socket_t* socket) { return socket && socket->type==socket_t::TCP; }
inline bool is_client(socket_t* socket) { return socket && socket->type==socket_t::TCP_CLIENT; }
inline bool is_server(socket_t* socket) { return socket && socket->type==socket_t::TCP_SERVER; }
inline bool is_open(socket_t* socket) { return socket && !socket->closed; }
inline tcp_socket_t* tcp(socket_t* socket) { return is_tcp(socket) ? &socket->s.tcp : NULL; }
inline udp_socket_t* udp(socket_t* socket) { return is_udp(socket) ? &socket->s.udp : NULL; }
inline tcp_server_client_t* client(socket_t* socket) { return is_client(socket) ? socket->s.tcp_client : NULL; }
inline tcp_server_t* server(socket_t* socket) { return is_server(socket) ? socket->s.tcp_server : NULL; }

wiced_tcp_socket_t* as_wiced_tcp_socket(socket_t* socket) 
{
    if (is_tcp(socket)) {
        return tcp(socket);
    }
    else if (is_client(socket)) {
        return socket->s.tcp_client->socket;
    }
    return NULL;
}

/**
 * Determines if the given socket handle is valid.
 * @param handle    The handle to test
 * @return {@code true} if the socket handle is valid, {@code false} otherwise.
 * Note that this doesn't guarantee the socket can be used, only that the handle
 * is within a valid range. To determine if a handle has an associated socket,
 * use {@link #from_handle}
 */
inline bool is_valid(sock_handle_t handle) {
    return handle<SOCKET_MAX;
}

uint8_t socket_handle_valid(sock_handle_t handle) {
    return is_valid(handle);    
}

/**
 * Fetches the socket_t info from an opaque handle. 
 * @return The socket_t pointer, or NULL if no socket is available for the
 * given handle.
 */
socket_t* from_handle(sock_handle_t handle) {    
    return is_valid(handle) ? (socket_t*)handle : NULL;
}

/**
 * Discards a previously allocated socket. If the socket is already invalid, returns silently.
 * Once a socket has been passed to the client, this is the only time the object is
 * deleted. Since the client initiates this call, the client is aware can the
 * socket is no longer valid.
 * @param handle    The handle to discard.
 * @return SOCKET_INVALID always.
 */
sock_handle_t socket_dispose(sock_handle_t handle) {
    if (socket_handle_valid(handle)) {        
        delete from_handle(handle);
    }
    return SOCKET_INVALID;
}

void close_all_list(socket_t*& list)
{
    socket_t* current = list;
    while (current) {
        current->close();
        current = current->next;
    }
    list = NULL;    // clear the list.
}

void socket_close_all()
{    
    close_all_list(clients);
    close_all_list(servers);
}



#define SOCKADDR_TO_PORT_AND_IPADDR(addr, addr_data, port, ip_addr) \
    const uint8_t* addr_data = addr->sa_data; \
    unsigned port = addr_data[0]<<8 | addr_data[1]; \
    wiced_ip_address_t INITIALISER_IPV4_ADDRESS(ip_addr, MAKE_IPV4_ADDRESS(addr_data[2], addr_data[3], addr_data[4], addr_data[5]));

sock_result_t as_sock_result(wiced_result_t result)
{
    return -result;
}

sock_result_t as_sock_result(socket_t* socket) 
{
    return (sock_result_t)(socket);
}

/**
 * Connects the given socket to the address.
 * @param sd        The socket handle to connect
 * @param addr      The address to connect to
 * @param addrlen   The length of the address details.
 * @return 0 on success.
 */
sock_result_t socket_connect(sock_handle_t sd, const sockaddr_t *addr, long addrlen)
{
    sock_result_t result = SOCKET_INVALID;
    socket_t* socket = from_handle(sd);
    if (is_tcp(socket)) {
        wiced_result_t wiced_result = wiced_tcp_bind(tcp(socket), WICED_ANY_PORT);
        if (wiced_result==WICED_SUCCESS) {
            SOCKADDR_TO_PORT_AND_IPADDR(addr, addr_data, port, ip_addr);
            unsigned timeout = 5*1000;
            result = wiced_tcp_connect(tcp(socket), &ip_addr, port, timeout);
        }
        else
            result = as_sock_result(wiced_result);
    }
    return result;
}

/**
 * Is there any way to unblock a blocking call on WICED? Perhaps shutdown the networking layer?
 * @return 
 */
sock_result_t socket_reset_blocking_call() 
{
    return 0;
}

wiced_result_t read_packet(wiced_packet_t* packet, uint8_t* target, uint16_t target_len, uint16_t* read_len)
{
    uint16_t read = 0;
    wiced_result_t result = WICED_SUCCESS;
    uint16_t fragment;
    uint16_t available;
    uint8_t* data;
    while (target_len!=0 && (result = wiced_packet_get_data(packet, read, &data, &fragment, &available))==WICED_SUCCESS && available!=0) {
        uint16_t to_read = std::min(fragment, target_len);
        memcpy(target+read, data, to_read);
        read += to_read;
        target_len -= to_read;
    }
    if (read_len!=NULL)
        *read_len = read;
    return result;
}

/**
 * Receives data from a socket. 
 * @param sd
 * @param buffer
 * @param len
 * @param _timeout
 * @return The number of bytes read. -1 if the end of the stream is reached.
 */
sock_result_t socket_receive(sock_handle_t sd, void* buffer, socklen_t len, system_tick_t _timeout)
{      
    sock_result_t bytes_read = -1;
    socket_t* socket = from_handle(sd);
    if (is_tcp(socket)) {
        tcp_socket_t* tcp_socket = tcp(socket);
        bytes_read = 0;
        tcp_packet_t& packet = tcp_socket->packet;
        if (!packet.packet) {
            packet.offset = 0;
            wiced_result_t result = wiced_tcp_receive(tcp_socket, &packet.packet, _timeout);
            if (result!=WICED_SUCCESS && result!=WICED_TIMEOUT) {
                DEBUG("Socket %d receive fail %d", (int)sd, int(result));
                return -result;
            }
        }        
        uint8_t* data;
        uint16_t available;
        uint16_t total;    
        bool dispose = true;
        if (packet.packet && (wiced_packet_get_data(packet.packet, packet.offset, &data, &available, &total)==WICED_SUCCESS)) {
            int read = std::min(uint16_t(len), available);
            packet.offset += read;
            memcpy(buffer, data, read);            
            dispose = (total==read);
            bytes_read = read;
            DEBUG("Socket %d receive bytes %d of %d", (int)sd, int(bytes_read), int(available));
        }        
        if (dispose) {            
            packet.dispose_packet();
        }
    }
    else if (is_client(socket)) {
        tcp_server_client_t* server_client = client(socket);
        bytes_read = server_client->read(buffer, len, _timeout);
    }
    return bytes_read;
}

/**
 * Low-level function to find the server that a given wiced tcp client
 * is associated with. The WICED callbacks provide the client socket, but
 * not the server it is associated with. 
 * @param client
 * @return 
 */
tcp_server_t* server_for_socket(wiced_tcp_socket_t* client)
{    
    socket_t* server = servers;
    while (server) {
        if (server->s.tcp_server->is_client(client))
            return server->s.tcp_server;
        server = server->next;
    }
    return NULL;
}

/**
 * Notification from the networking thread that the given client socket connected
 * to the server.
 * @param socket
 */
wiced_result_t server_connected(void* socket)
{
    wiced_tcp_socket_t* s = (wiced_tcp_socket_t*)socket;
    tcp_server_t* server = server_for_socket(s);    
    wiced_result_t result = WICED_ERROR;
    if (server) {
        result = server->accept(s);
    }        
    return result;
}

/**
 * Notification that the client socket has data. 
 * @param socket
 */
wiced_result_t server_received(void* socket)
{   
    return WICED_SUCCESS;
}

/**
 * Notification that the client socket closed the connection.
 * @param socket
 */
wiced_result_t server_disconnected(void* socket)
{
    wiced_tcp_socket_t* s = (wiced_tcp_socket_t*)socket;
    tcp_server_t* server = server_for_socket(s);
    wiced_result_t result = WICED_ERROR;
    if (server) {
        // disconnect the socket from the server, but maintain the client
        // socket handle.
        result = server->disconnect(s);
    }
    return result;
}

sock_result_t socket_create_tcp_server(uint16_t port)
{           
    socket_t* handle = new socket_t();
    tcp_server_t* server = new tcp_server_t();    
    wiced_result_t result = WICED_OUT_OF_HEAP_SPACE;
    if (handle && server) {
        result = wiced_tcp_server_start(server, WICED_STA_INTERFACE,
            port, server_connected, server_received, server_disconnected);        
    }
    if (result!=WICED_SUCCESS) {
        delete handle; handle = NULL;
        delete server; server = NULL;        
    }
    else {
        handle->set_server(server);
        add(handle);
    }
    
    return handle ? as_sock_result(handle) : as_sock_result(result);
}

/**
 * Fetch the next waiting client socket from the server
 * @param sock
 * @return 
 */
sock_result_t socket_accept(sock_handle_t sock) 
{    
    sock_result_t result = SOCKET_INVALID;
    socket_t* socket = from_handle(sock);
    if (is_open(socket) && is_server(socket)) {
        tcp_server_t* server = socket->s.tcp_server;
        tcp_server_client_t* client = server->next_accept();
        if (client) {
            socket_t* socket = new socket_t();
            socket->set_client(client);
            add(socket);
            result = (sock_result_t)socket;
        }        
    }
    return result;
}

/**
 * Determines if a given socket is bound.
 * @param sd    The socket handle to test
 * @return non-zero if bound, 0 otherwise.
 */
uint8_t socket_active_status(sock_handle_t sd) 
{
    socket_t* socket = from_handle(sd);
    uint8_t result = 0;
    if (socket) {
        result = !socket->closed;
    }
    return result ? SOCKET_STATUS_ACTIVE : SOCKET_STATUS_INACTIVE;
}

/**
 * Closes the socket handle.
 * @param sock
 * @return 
 */
sock_result_t socket_close(sock_handle_t sock) 
{
    sock_result_t result = WICED_SUCCESS;
    socket_t* socket = from_handle(sock);
    if (socket) {
        remove(socket);
        socket_dispose(sock);
        DEBUG("socket closed %x", int(sock));
    }
    return result;
}

/**
 * Create a new socket handle.
 * @param family    Must be {@code AF_INET}
 * @param type      Either SOCK_DGRAM or SOCK_STREAM
 * @param protocol  Either IPPROTO_UDP or IPPROTO_TCP
 * @return 
 */
sock_handle_t socket_create(uint8_t family, uint8_t type, uint8_t protocol, uint16_t port) 
{
    if (family!=AF_INET || !((type==SOCK_DGRAM && protocol==IPPROTO_UDP) || (type==SOCK_STREAM && protocol==IPPROTO_TCP)))
        return SOCKET_INVALID;
    
    sock_handle_t result = SOCKET_INVALID;
    socket_t* socket = new socket_t();
    if (socket) {
        wiced_result_t wiced_result;
        socket->type = (protocol==IPPROTO_UDP ? socket_t::UDP : socket_t::TCP);
        if (type==socket_t::TCP) {
            wiced_result = wiced_tcp_create_socket(tcp(socket), WICED_STA_INTERFACE);
        }
        else {
            wiced_result = wiced_udp_create_socket(udp(socket), port, WICED_STA_INTERFACE);            
        }
        if (wiced_result!=WICED_SUCCESS) {
            socket->type = socket_t::NONE;  // don't try to destruct the wiced resource since it was never created.
            socket_dispose(result);
            result = as_sock_result(wiced_result);
        }        
        else {
            add(socket);
            result = as_sock_result(socket);
        }
    }        
    return result;
}

/**
 * Send data to a socket.
 * @param sd    The socket handle to send data to.
 * @param buffer    The data to send
 * @param len       The number of bytes to send
 * @return 
 */
sock_result_t socket_send(sock_handle_t sd, const void* buffer, socklen_t len) 
{
    sock_result_t result = SOCKET_INVALID;
    socket_t* socket = from_handle(sd);
    wiced_tcp_socket_t* tcp_socket = as_wiced_tcp_socket(socket);
    if (is_open(socket) && tcp_socket) {
        wiced_result_t wiced_result = wiced_tcp_send_buffer(tcp_socket, buffer, uint16_t(len));
        result = wiced_result ? as_sock_result(wiced_result) : len;        
        DEBUG("Write %d bytes to socket %d", (int)len, (int)sd);
    }
    return result;
}

sock_result_t socket_sendto(sock_handle_t sd, const void* buffer, socklen_t len, 
        uint32_t flags, sockaddr_t* addr, socklen_t addr_size) 
{
    socket_t* socket = from_handle(sd);
    wiced_result_t result = WICED_INVALID_SOCKET;
    if (is_open(socket) && is_udp(socket)) {
        SOCKADDR_TO_PORT_AND_IPADDR(addr, addr_data, port, ip_addr);
        uint16_t available = 0;
        wiced_packet_t* packet = NULL;
        uint8_t* data;        
        if ((result=wiced_packet_create_udp(udp(socket), len, &packet, &data, &available))==WICED_SUCCESS) {
            size_t size = std::min(available, uint16_t(len));
            memcpy(data, buffer, size);
            /* Set the end of the data portion */
            wiced_packet_set_data_end(packet, (uint8_t*) data + size);
            result = wiced_udp_send(udp(socket), &ip_addr, port, packet);
        }                       
    }
    return as_sock_result(result);
}

sock_result_t socket_receivefrom(sock_handle_t sd, void* buffer, socklen_t bufLen, uint32_t flags, sockaddr_t* addr, socklen_t* addrsize) 
{
    socket_t* socket = from_handle(sd);
    wiced_result_t result = WICED_INVALID_SOCKET;
    uint16_t read_len = 0;
    if (is_open(socket) && is_udp(socket)) {
        wiced_packet_t* packet = NULL;
        // UDP receive timeout changed to 0 sec so as not to block
        if ((result=wiced_udp_receive(udp(socket), &packet, WICED_NO_WAIT))==WICED_SUCCESS) {
            if ((result=read_packet(packet, (uint8_t*)buffer, bufLen, &read_len))==WICED_SUCCESS) {
                wiced_ip_address_t wiced_ip_addr;
                uint16_t port;              
                if ((result=wiced_udp_packet_get_info(packet, &wiced_ip_addr, &port))==WICED_SUCCESS) {
                    uint32_t ipv4 = GET_IPV4_ADDRESS(wiced_ip_addr);
                    addr->sa_data[0] = (port>>8) & 0xFF;
                    addr->sa_data[1] = port & 0xFF;
                    addr->sa_data[2] = (ipv4 >> 24) & 0xFF;
                    addr->sa_data[3] = (ipv4 >> 16) & 0xFF;
                    addr->sa_data[4] = (ipv4 >> 8) & 0xFF;
                    addr->sa_data[5] = ipv4 & 0xFF;
                }
            }
            wiced_packet_delete(packet);
        }
    }       
    return result ? as_sock_result(result) : sock_result_t(read_len);
}

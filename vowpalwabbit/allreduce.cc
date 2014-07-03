/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
/*
This implements the allreduce function of MPI.  Code primarily by
Alekh Agarwal and John Langford, with help Olivier Chapelle.
 */
#include <iostream>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <WinSock2.h>
#include <Windows.h>
#include <WS2tcpip.h>
#include <io.h>
#else
#include <unistd.h>
#include <arpa/inet.h>
#endif
#include <sys/timeb.h>
#include "allreduce.h"

using namespace std;

// port is already in network order
socket_t sock_connect(const uint32_t ip, const int port) {

  socket_t sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock == -1)
    {
      cerr << "socket: " << strerror(errno) << endl;
      throw exception();
    }
  sockaddr_in far_end;
  far_end.sin_family = AF_INET;
  far_end.sin_port = port;

  far_end.sin_addr = *(in_addr*)&ip;
  memset(&far_end.sin_zero, '\0',8);

  {
    char dotted_quad[INET_ADDRSTRLEN];
    if (NULL == inet_ntop(AF_INET, &(far_end.sin_addr), dotted_quad, INET_ADDRSTRLEN)) {
      cerr << "inet_ntop: " << strerror(errno) << endl;
      throw exception();
    }

    char hostname[NI_MAXHOST];
    char servInfo[NI_MAXSERV];
    if (getnameinfo((sockaddr *) &far_end, sizeof(sockaddr), hostname, NI_MAXHOST, servInfo, NI_MAXSERV, NI_NUMERICSERV)) {
      cerr << "getnameinfo(" << dotted_quad << "): " << strerror(errno) << endl;
      throw exception();
    }
    cerr << "connecting to " << dotted_quad << " = " << hostname << ':' << ntohs(port) << endl;
  }

  size_t count = 0;
  int ret;
  while ( (ret =connect(sock,(sockaddr*)&far_end, sizeof(far_end))) == -1 && count < 100)
    {
      count++;
      cerr << "connect attempt " << count << " failed: " << strerror(errno) << endl;
#ifdef _WIN32
      Sleep(1);
#else
      sleep(1);
#endif
    }
  if (ret == -1)
    throw exception();
  return sock;
}

socket_t getsock()
{
  socket_t sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    cerr << "socket: " << strerror(errno) << endl;
    throw exception();
  }

  // SO_REUSEADDR will allow port rebinding on Windows, causing multiple instances
  // of VW on the same machine to potentially contact the wrong tree node.
#ifndef _WIN32
    int on = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on)) < 0)
      cerr << "setsockopt SO_REUSEADDR: " << strerror(errno) << endl;
#endif
  return sock;
}

void all_reduce_init(const string master_location, const size_t unique_id, const size_t total, const size_t node, node_socks& socks)
{
#ifdef _WIN32
  WSAData wsaData;
  WSAStartup(MAKEWORD(2,2), &wsaData);
  int lastError = WSAGetLastError();
#endif



  struct hostent* master = gethostbyname(master_location.c_str());

  if (master == NULL) {
    cerr << "gethostbyname(" << master_location << "): " << strerror(errno) << endl;
    throw exception();
  }
  socks.current_master = master_location;

  uint32_t master_ip = * ((uint32_t*)master->h_addr);
  int port = 26543;

  socket_t master_sock = sock_connect(master_ip, htons(port));
  if(send(master_sock, (const char*)&unique_id, sizeof(unique_id), 0) < (int)sizeof(unique_id))
    cerr << "write unique_id=" << unique_id << " failed!" << endl;
  else cerr << "wrote unique_id=" << unique_id << endl;
  if(send(master_sock, (const char*)&total, sizeof(total), 0) < (int)sizeof(total))
    cerr << "write total=" << total << " failed!" << endl;
  else cerr << "wrote total=" << total << endl;
  if(send(master_sock, (char*)&node, sizeof(node), 0) < (int)sizeof(node))
    cerr << "write node=" << node << " failed!" << endl;
  else cerr << "wrote node=" << node << endl;
  int ok;
  if (recv(master_sock, (char*)&ok, sizeof(ok), 0) < (int)sizeof(ok))
    cerr << "read ok failed!" << endl;
  else cerr << "read ok=" << ok << endl;
  if (!ok) {
    cerr << "mapper already connected" << endl;
    throw exception();
  }

  uint16_t kid_count;
  uint16_t parent_port;
  uint32_t parent_ip;

  if(recv(master_sock, (char*)&kid_count, sizeof(kid_count), 0) < (int)sizeof(kid_count))
    cerr << "read kid_count failed!" << endl;
  else cerr << "read kid_count=" << kid_count << endl;
  
  socket_t sock = -1;
  short unsigned int netport = htons(26544);
  if(kid_count > 0) {
    sock = getsock();
    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = netport;

    bool listening = false;
    while(!listening)
    {
      if (::bind(sock,(sockaddr*)&address, sizeof(address)) < 0)
      {
#ifdef _WIN32
        if (WSAGetLastError() == WSAEADDRINUSE)
#else
        if (errno == EADDRINUSE)
#endif
        {
          netport = htons(ntohs(netport)+1);
          address.sin_port = netport;
        }
        else
        {
          cerr << "bind: " << strerror(errno) << endl;
          throw exception();
        }
      }
      else
      {
        if (listen(sock, kid_count) < 0)
        {
          cerr << "listen: " << strerror(errno) << endl;
          CLOSESOCK(sock);
          sock = getsock();
        }
        else
        {
          listening = true;
        }
      }
    }
  }

  if(send(master_sock, (const char*)&netport, sizeof(netport), 0) < (int)sizeof(netport))
    cerr << "write netport failed!" << endl;

  if(recv(master_sock, (char*)&parent_ip, sizeof(parent_ip), 0) < (int)sizeof(parent_ip))
    cerr << "read parent_ip failed!" << endl;
  else {
    char dotted_quad[INET_ADDRSTRLEN];
    if (NULL == inet_ntop(AF_INET, (char*)&parent_ip, dotted_quad, INET_ADDRSTRLEN)) {
      cerr << "read parent_ip=" << parent_ip << "(inet_ntop: " << strerror(errno) << ")" << endl;
    } else
      cerr << "read parent_ip=" << dotted_quad << endl;
  }
  if(recv(master_sock, (char*)&parent_port, sizeof(parent_port), 0) < (int)sizeof(parent_port))
    cerr << "read parent_port failed!" << endl;
  else cerr << "read parent_port=" << parent_port << endl;
  
  CLOSESOCK(master_sock);

  if(parent_ip != (uint32_t)-1) {
    socks.parent = sock_connect(parent_ip, parent_port);
  }
  else
    socks.parent = -1;

  socks.children[0] = -1; socks.children[1] = -1;
  for (int i = 0; i < kid_count; i++)
  {
    sockaddr_in child_address;
    socklen_t size = sizeof(child_address);
    socket_t f = accept(sock,(sockaddr*)&child_address,&size);
    if (f < 0)
    {
      cerr << "accept: " << strerror(errno) << endl;
      throw exception();
    }
    // char hostname[NI_MAXHOST];
    // char servInfo[NI_MAXSERV];
    // getnameinfo((sockaddr *) &child_address, sizeof(sockaddr), hostname, NI_MAXHOST, servInfo, NI_MAXSERV, NI_NUMERICSERV);
    // cerr << "connected to " << hostname << ':' << ntohs(port) << endl;
    socks.children[i] = f;
  }

  if (kid_count > 0)
    CLOSESOCK(sock);
}


void pass_down(char* buffer, const size_t parent_read_pos, size_t& children_sent_pos, const socket_t * child_sockets, const size_t n) {

  size_t my_bufsize = min(ar_buf_size, (parent_read_pos - children_sent_pos));

  if(my_bufsize > 0) {
    //going to pass up this chunk of data to the children
    if(child_sockets[0] != -1 && send(child_sockets[0], buffer+children_sent_pos, (int)my_bufsize, 0) < (int)my_bufsize)
      cerr<<"Write to left child failed\n";
    if(child_sockets[1] != -1 && send(child_sockets[1], buffer+children_sent_pos, (int)my_bufsize, 0) < (int)my_bufsize)
      cerr<<"Write to right child failed\n";

    children_sent_pos += my_bufsize;
  }
}



void broadcast(char* buffer, const size_t n, const socket_t parent_sock, const socket_t * child_sockets) {

   size_t parent_read_pos = 0; //First unread float from parent
   size_t children_sent_pos = 0; //First unsent float to children
  //parent_sent_pos <= left_read_pos
  //parent_sent_pos <= right_read_pos

   if(parent_sock == -1) {
     parent_read_pos = n;
   }
   if(child_sockets[0] == -1 && child_sockets[1] == -1)
     children_sent_pos = n;

   while (parent_read_pos < n || children_sent_pos < n)
    {
      pass_down(buffer, parent_read_pos, children_sent_pos, child_sockets, n);
      if(parent_read_pos >= n && children_sent_pos >= n) break;

      if (parent_sock != -1) {
	//there is data to be read from the parent
	if(parent_read_pos == n) {
	  cerr<<"I think parent has no data to send but he thinks he has\n";
	  throw exception();
	}
	size_t count = min(ar_buf_size,n-parent_read_pos);
	int read_size = recv(parent_sock, buffer + parent_read_pos, (int)count, 0);
	if(read_size == -1) {
	  cerr <<" recv from parent: " << strerror(errno) << endl;
	}
	parent_read_pos += read_size;
      }
    }
}

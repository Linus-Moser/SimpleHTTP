/**
 * SimpleHTTP
 *
 * Copyright (C) 2024  Linus Ilian Moser <linus.moser@megakuul.ch>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SIMPLEHTTP_H
#define SIMPLEHTTP_H

#include <asm-generic/socket.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <filesystem>
#include <format>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>


using namespace std;

namespace fs = filesystem;

namespace SimpleHTTP {

	/**
	 * RAII compatible filedescriptor wrapper
	 *
   * Essentially just closes the filedescriptor on destruction
	 */
	class FileDescriptor {
	public:
    // Default constructor puts descriptor into invalid state (-1)
    FileDescriptor() : fd(-1) {};
    
    FileDescriptor(int fd) : fd(fd) {};
    // Move constructor sets descriptor to -1 so that close() will not lead to undefined behavior
		FileDescriptor(FileDescriptor&& other) noexcept : fd(other.fd) {
			if (this != &other) {
				other.fd = -1;
			}
		};
		// Copy constructor is deleted, socket cannot be copied
		FileDescriptor(const FileDescriptor&) noexcept = delete;
    
    // Move assignment sets descriptor to -1 so that close() will not lead to undefined behavior		
		FileDescriptor& operator=(FileDescriptor&& other) noexcept {
			if (this != &other) {
        // If other fd is not the same, close the original fd on this object
        if (other.fd!=fd) {
          close(fd);
        }
				fd = other.fd;
				other.fd = -1;
			}
      return *this;
		};
		// Copy assignment is deleted, socket cannot be copied
		FileDescriptor& operator=(const FileDescriptor&) noexcept = delete;
		
		~FileDescriptor() {
			close(fd);
		};

    /**
     * Returns filedescriptor
     */
		int getfd() const noexcept {
			return fd;
		}
	private:
		int fd;
	};

	/**
	 * HTTP Server object bound to one bsd socket
	 *
	 * Server can run on top of *ipv4* or *unix sockets*
	 *
	 * Exceptions: runtime_error, logical_error, filesystem::filesystem_error
	 */
	class Server {
	public:
		Server() = delete;

    /**
     * Launch Server using unix socket
     */
		Server(string unixSockPath) {
			fs::create_directories(fs::path(unixSockPath).parent_path());
      // Clean up socket, errors are ignored, if the socket cannot be cleaned up, it will fail at bind() which is fine
      unlink(unixSockPath.c_str());

      // Initialize core socket
      coreSocket = FileDescriptor(socket(AF_UNIX, SOCK_STREAM, 0));
      if (coreSocket.getfd() < 0) {
				throw runtime_error(
          format(
					  "Failed to initialize HTTP server ({}):\n{}",
						"create socket", strerror(errno)
          )
			  );
			}
      
      // Create sockaddr_un for convenient option setting
      struct sockaddr_un* unSockAddr = (struct sockaddr_un *)&coreSockAddr;
      // Clean unSockAddr, 'cause maybe some weird libs
			// still expect it to zero out sin_zero (which C++ does not do by def)
      memset(unSockAddr, 0, sizeof(*unSockAddr));
      // Set unSockAddr options
      unSockAddr->sun_family = AF_UNIX;
      strcpy(unSockAddr->sun_path, unixSockPath.c_str());

      // Bind unix socket
			int res = bind(coreSocket.getfd(), &coreSockAddr, sizeof(coreSockAddr));
			if (res < 0) {
				throw runtime_error(
          format(
					  "Failed to initialize HTTP server ({}):\n{}",
						"bind socket", strerror(errno)
          )
			  );
			}

      // Retrieve current flags
      sockFlags = fcntl(coreSocket.getfd(), F_GETFL, 0);
      if (res < 0) {
        throw runtime_error(
          format(
            "Failed to initialize HTTP server ({}):\n{}",
						"read socket flags", strerror(errno)
          )
        );
      }

      // Add nonblocking flag
      sockFlags = sockFlags | O_NONBLOCK;

      // Set flags for core socket
      res = fcntl(coreSocket.getfd(), F_SETFL, sockFlags);
      if (res < 0) {
        throw runtime_error(
          format(
            "Failed to initialize HTTP server ({}):\n{}",
						"update socket flags", strerror(errno)
          )
        );
      }
      
      // Socket is closed automatically in destructor, because Socket is RAII compatible.
		};

		/**
		 * Launch Server using kernel network stack
		 *
		 * Multiple instances of this server can be launched in parallel to increase performance.
		 * BSD sockets with same *ip* and *port* combination, will automatically loadbalance *tcp* sessions.
		 */
		Server(string ipAddr, u_int16_t port) {
      // Create sockaddr_in for convenient option setting
      struct sockaddr_in *inSockAddr = (struct sockaddr_in *)&coreSockAddr;
			// Clean inSockAddr, 'cause maybe some weird libs
			// still expect it to zero out sin_zero (which C++ does not do by def)
			memset(inSockAddr, 0, sizeof(*inSockAddr));
			// Set inSockAddr options
			inSockAddr->sin_family = AF_INET;
			inSockAddr->sin_port = htons(port);
      
			// Parse IPv4 addr and insert it to inSockAddr
			int res = inet_pton(AF_INET, ipAddr.c_str(), &inSockAddr->sin_addr);
			if (res==0) {
				throw logic_error(
				  format(
						"Failed to initialize HTTP server ({}):\n{}",
						"addr parsing", "Invalid IP-Address format"
				  )
			  );
			} else if (res==-1) {
				throw runtime_error(
				  format(
						"Failed to initialize HTTP server ({}):\n{}",
						"addr parsing", strerror(errno)
				  )
			  );
			}

      // Initialize core socket
      coreSocket = FileDescriptor(socket(AF_INET, SOCK_STREAM, 0));
      if (coreSocket.getfd() < 0) {
				throw runtime_error(
          format(
					  "Failed to initialize HTTP server ({}):\n{}",
						"create socket", strerror(errno)
          )
			  );
			}

			// SO_REUSEADDR = Enable binding TIME_WAIT network ports forcefully
			// SO_REUSEPORT = Enable to cluster (lb) multiple bsd sockets with same ip + port combination
			int opt = 1; // opt 1 indicates that the options should be enabled
			res = setsockopt(coreSocket.getfd(), SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
			if (res < 0) {
				throw runtime_error(
				  format(
					  "Failed to initialize HTTP server ({}):\n{}",
						"set socket options", strerror(errno)
				  )
			  );
			}
			
			// Set socket recv buffer (should match a regular HTTP package for optimal performance)
			res = setsockopt(coreSocket.getfd(), SOL_SOCKET, SO_RCVBUF, &sockBufferSize, sizeof(sockBufferSize));
			if (res < 0) {
				throw runtime_error(
				  format(
					  "Failed to initialize HTTP server ({}):\n{}",
						"set socket options", strerror(errno)
				  )
			  );
			}
			// Set socket send buffer (should match a regular HTTP package for optimal performance)
			res = setsockopt(coreSocket.getfd(), SOL_SOCKET, SO_SNDBUF, &sockBufferSize, sizeof(sockBufferSize));
			if (res < 0) {
				throw runtime_error(
				  format(
					  "Failed to initialize HTTP server ({}):\n{}",
						"set socket options", strerror(errno)
				  )
			  );
			}

			// Bind socket to specified addr
			res = bind(coreSocket.getfd(), &coreSockAddr, sizeof(coreSockAddr));
			if (res < 0) {
				throw runtime_error(
          format(
					  "Failed to initialize HTTP server ({}):\n{}",
						"bind socket", strerror(errno)
          )
			  );
			}

      // Retrieve current flags
      sockFlags = fcntl(coreSocket.getfd(), F_GETFL, 0);
      if (res < 0) {
        throw runtime_error(
          format(
            "Failed to initialize HTTP server ({}):\n{}",
						"read socket flags", strerror(errno)
          )
        );
      }

      // Add nonblocking flag
      sockFlags = sockFlags; // | O_NONBLOCK;

      // Set flags for core socket
      res = fcntl(coreSocket.getfd(), F_SETFL, sockFlags);
      if (res < 0) {
        throw runtime_error(
          format(
            "Failed to initialize HTTP server ({}):\n{}",
						"update socket flags", strerror(errno)
          )
        );
      }

			// Socket is closed automatically in destructor, because Socket is RAII compatible.
		};
		
		void Serve() {
      // Start listener on core socket
      int res = listen(coreSocket.getfd(), sockQueueSize);
      if (res < 0) {
        throw runtime_error(
          format(
            "Failed to initialize HTTP server ({}):\n{}",
            "start listener", strerror(errno)
          )
        );
      }

      // Create epoll instance
      FileDescriptor epollSocket(epoll_create1(0));
      if (epollSocket.getfd() < 0) {
        throw runtime_error(
          format(
            "Failed to initialize HTTP server ({}):\n{}",
            "create epoll instance", strerror(errno)
          )
        );
      }

      // Add core socket to epoll instance
      // This is just used to inform the epoll_ctl which events we are interested in
      // sock_event is not manipulated by the epoll_ctl syscall
      struct epoll_event sockEvent;
      // On core socket we are only interested in readable state, there is no need for any writes to it
      sockEvent.events = EPOLLIN;
      sockEvent.data.fd = coreSocket.getfd();
      
      res = epoll_ctl(epollSocket.getfd(), EPOLL_CTL_ADD, coreSocket.getfd(), &sockEvent);
      if (res < 0) {
        throw runtime_error(
          format(
            "Failed to initialize HTTP server ({}):\n{}",
            "add core socket to epoll instance", strerror(errno)
          )
        );
      }

      // Preinitialize list of connection events
      struct epoll_event conEvents[maxEventsPerLoop];
      
      // Start main loop
      while (1) {
        // Wait for any epoll event (includes core socket and connections)
        // The -1 timeout means that it waits indefinitely until a event is reported
        int n = epoll_wait(epollSocket.getfd(), conEvents, maxEventsPerLoop, -1);
        if (n < 0) {
          throw runtime_error(
            format(
              "Critical failure while running HTTP server ({}):\n{}",
              "wait for incoming events", strerror(errno)
            )
          );
        }
        // Handle events
        for (int i = 0; i < n; i++) {
          // If the event is from the core socket
          if (conEvents[n].data.fd == coreSocket.getfd()) {
            // Check if error occured, if yes fetch it and return
            // For simplicity reasons there is currently no http 500 response here
            // instead sockets are closed leading to hangup signal on the client
            if (conEvents[n].events & EPOLLERR) {
              int err = 0;
              socklen_t errlen = sizeof(err);
              // Read error from sockopt
              res = getsockopt(conEvents[n].data.fd, SOL_SOCKET, SO_ERROR, (void *)&err, &errlen);
              // If getsockopt failed, return unknown error
              if (res < 0) {
                throw runtime_error(
                  format(
                    "Critical failure while running HTTP server ({}):\n{}",
                    "error on core socket", "Unknown error"
                  )
                );
              } else {
                // If getsockopt succeeded, return error
                throw runtime_error(
                  format(
                    "Critical failure while running HTTP server ({}):\n{}",
                    "error on core socket", strerror(err)
                  )
                );
              }
              // Check if socket hang up (happens if e.g. fd is closed)
              // Socket hang up is expected and therefore the loop is closed without errors
              if (conEvents[n].events & EPOLLHUP) {
                return;
              }

              // Accept new connection if any
              struct sockaddr conSockAddr = coreSockAddr;
              socklen_t conSockLen = sizeof(conSockAddr);
              int conSocket = accept(coreSocket.getfd(), &conSockAddr, &conSockLen);
              // Think about how to handle this
            }
            // Handle case for other connections
            
            // First check if error, if yes fail
            // Second check pollin and accept connections
            continue;
          }
          // If pollin read / parse, if header is not fully read, write to associated memory block
          // if fully read, fully parse and initialize function

          // If pollout and no function in progress, ignore, if in progress run body writer
        }
      }
    };

	private:
    // Core bsd socket
		FileDescriptor coreSocket;
    // Socket addr
    struct sockaddr coreSockAddr;
    // Socket flags
    int sockFlags;
		// Size of the Send / Recv buffer in bytes
		const int sockBufferSize = 8192;
		// Size of waiting incomming connections before connections are refused
		const int sockQueueSize = 128;
    // Defines the maximum epoll events handled in one loop iteration
    const int maxEventsPerLoop = 12;
	};

}

#endif

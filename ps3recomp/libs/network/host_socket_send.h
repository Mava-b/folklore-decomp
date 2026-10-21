/* Host-only send flag: a broken peer is an I/O error, not a process signal.
 * Darwin and Linux expose MSG_NOSIGNAL. Winsock has no SIGPIPE. This flag
 * does not alter the process signal handler/mask or a socket's options. */
#ifndef PS3RECOMP_HOST_SOCKET_SEND_H
#define PS3RECOMP_HOST_SOCKET_SEND_H

#if !defined(_WIN32) && defined(MSG_NOSIGNAL)
#define PS3_NET_HOST_SEND_FLAGS MSG_NOSIGNAL
#else
#define PS3_NET_HOST_SEND_FLAGS 0
#endif

#endif

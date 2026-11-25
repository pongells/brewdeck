// TCP connection and listener on lwIP sockets (tcp.h).
#include "tcp.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <lwip/sockets.h>

bool TcpConn::connect(uint32_t addr, uint16_t port, uint32_t ms) {
  close();
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return false;
  const int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);              // connect in the background, wait for it below
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = addr;
  int r = ::connect(fd, (struct sockaddr *)&a, sizeof a);
  if (r != 0 && errno == EINPROGRESS) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
    int err = 0;
    socklen_t l = sizeof err;
    r = select(fd + 1, NULL, &w, NULL, &tv) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) == 0 && !err ? 0 : -1;
  }
  if (r != 0) {
    ::close(fd);
    return false;
  }
  fcntl(fd, F_SETFL, fl);                           // blocking again (with the time limits of set_timeout)
  fd_ = fd;
  return true;
}

void TcpConn::set_timeout(uint32_t ms) {
  if (fd_ < 0) return;
  struct timeval tv = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
void TcpConn::set_nodelay(bool on) {
  const int v = on ? 1 : 0;
  if (fd_ >= 0) setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
}

size_t TcpConn::write(const void *b, size_t n) {
  size_t done = 0;
  while (fd_ >= 0 && done < n) {
    const int k = send(fd_, (const char *)b + done, n - done, 0);
    if (k > 0) done += (size_t)k;
    else if (k < 0 && errno == EINTR) continue;
    else break;                                     // the time limit (EAGAIN) or the connection failed
  }
  return done;
}
size_t TcpConn::print(const char *s) { return write(s, strlen(s)); }
size_t TcpConn::printf(const char *fmt, ...) {
  char loc[128];                                    // most replies fit; longer ones in a heap buffer
  va_list a;
  va_start(a, fmt);
  const int n = vsnprintf(loc, sizeof loc, fmt, a);
  va_end(a);
  if (n < 0) return 0;
  if ((size_t)n < sizeof loc) return write(loc, (size_t)n);
  char *big = (char *)malloc((size_t)n + 1);
  if (!big) return 0;
  va_start(a, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, a);
  va_end(a);
  const size_t w = write(big, (size_t)n);
  free(big);
  return w;
}

size_t TcpConn::read(void *b, size_t n) {
  size_t done = 0;
  while (fd_ >= 0 && done < n) {
    const int k = recv(fd_, (char *)b + done, n - done, 0);
    if (k > 0) done += (size_t)k;
    else if (k < 0 && errno == EINTR) continue;
    else break;                                     // the limit, the other side closed (0), or an error
  }
  return done;
}
size_t TcpConn::read_some(void *b, size_t n) {
  for (;;) {
    if (fd_ < 0 || !n) return 0;
    const int k = recv(fd_, (char *)b, n, 0);
    if (k > 0) return (size_t)k;
    if (k < 0 && errno == EINTR) continue;
    return 0;
  }
}
size_t TcpConn::read_line(char *b, size_t cap) {
  size_t k = 0;
  if (!cap) return 0;
  while (fd_ >= 0 && k < cap - 1) {
    char c;
    const int r = recv(fd_, &c, 1, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r != 1 || c == '\n') break;
    b[k++] = c;
  }
  b[k] = 0;
  return k;
}
bool TcpConn::connected() {
  if (fd_ < 0) return false;
  char c;
  const int r = recv(fd_, &c, 1, MSG_PEEK | MSG_DONTWAIT);
  return r > 0 || (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
}
void TcpConn::peer_ip(char *out, size_t cap) {
  struct sockaddr_in a;
  socklen_t l = sizeof a;
  if (cap) out[0] = 0;
  if (fd_ >= 0 && getpeername(fd_, (struct sockaddr *)&a, &l) == 0) inet_ntop(AF_INET, &a.sin_addr, out, cap);
}
void TcpConn::close() {
  if (fd_ >= 0) {
    shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
  }
  fd_ = -1;
}

bool TcpListener::begin(uint16_t port) {
  fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd_ < 0) return false;
  const int one = 1;
  setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd_, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd_, 2) != 0) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  return true;
}
int TcpListener::accept(uint32_t ms) {
  if (fd_ < 0) return -1;
  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(fd_, &rd);
  struct timeval tv = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
  if (select(fd_ + 1, &rd, NULL, NULL, &tv) <= 0) return -1;
  return ::accept(fd_, NULL, NULL);
}

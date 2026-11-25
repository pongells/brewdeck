// A TCP connection on an lwIP socket (blocking, with a time limit on every read and write), and a listening socket.
// Small on purpose (one int and a time limit): the tasks that use it have small stacks. Reads are not buffered, so
// read_line takes exactly one line and leaves the rest (e.g. a request's body) in the socket.
#pragma once
#include <stddef.h>
#include <stdint.h>

class TcpConn {
 public:
  explicit TcpConn(int fd = -1) : fd_(fd) {}
  ~TcpConn() { close(); }
  TcpConn(const TcpConn &) = delete;
  TcpConn &operator=(const TcpConn &) = delete;

  // opens a connection to an IPv4 address (network byte order), waiting up to ms; closes any earlier one
  bool connect(uint32_t addr, uint16_t port, uint32_t ms);
  bool valid() const { return fd_ >= 0; }
  void set_timeout(uint32_t ms);   // for each read / write call from now on (default: lwIP's, i.e. none)
  void set_nodelay(bool on);       // no Nagle: small writes go out at once
  // writes all of b unless the time limit runs out or the connection fails; returns the bytes written
  size_t write(const void *b, size_t n);
  size_t print(const char *s);
  size_t printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  // up to n bytes, waiting up to the time limit for each part; returns fewer at the limit, end or an error
  size_t read(void *b, size_t n);
  // what has arrived, up to n bytes, waiting up to the time limit for the first; 0 at the limit, end or an error
  size_t read_some(void *b, size_t n);
  // one line without its '\n' (a '\r' before it is kept), at most cap - 1 chars, 0-terminated; returns its length
  // (0 at the limit or end)
  size_t read_line(char *b, size_t cap);
  // how many of the next n bytes have arrived (without waiting or taking them)
  size_t ready(size_t n);
  bool connected();                // still open (the other side has not closed it)
  void peer_ip(char *out, size_t cap);   // "192.168.1.10"
  void close();

 private:
  int fd_;
};

// accepts connections on a port (any address)
class TcpListener {
 public:
  bool begin(uint16_t port);
  // waits up to ms for a connection; returns its socket (wrap it in a TcpConn) or -1
  int accept(uint32_t ms);
 private:
  int fd_ = -1;
};

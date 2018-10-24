#ifndef LLARP_EV_HPP
#define LLARP_EV_HPP
#include <llarp/ev.h>
// writev
#ifndef _WIN32
#include <sys/uio.h>
#endif
#include <unistd.h>
#include <llarp/buffer.h>
#include <llarp/codel.hpp>
#include <list>
#include <deque>

#ifdef _WIN32
#include <variant>
#endif

#ifndef MAX_WRITE_QUEUE_SIZE
#define MAX_WRITE_QUEUE_SIZE 1024
#endif

#ifndef EV_READ_BUF_SZ
#define EV_READ_BUF_SZ (4 * 1024)
#endif

namespace llarp
{
  struct ev_io
  {
    struct WriteBuffer
    {
      llarp_time_t timestamp = 0;
      size_t bufsz;
      byte_t buf[1500];

      WriteBuffer() = default;

      WriteBuffer(const void* ptr, size_t sz)
      {
        if(sz <= sizeof(buf))
        {
          bufsz = sz;
          memcpy(buf, ptr, bufsz);
        }
        else
          bufsz = 0;
      }

      struct GetTime
      {
        llarp_time_t
        operator()(const WriteBuffer& w) const
        {
          return w.timestamp;
        }
      };

      struct PutTime
      {
        void
        operator()(WriteBuffer& w) const
        {
          w.timestamp = llarp_time_now_ms();
        }
      };

      struct Compare
      {
        bool
        operator()(const WriteBuffer& left, const WriteBuffer& right) const
        {
          return left.timestamp < right.timestamp;
        }
      };
    };

    typedef llarp::util::CoDelQueue< WriteBuffer, WriteBuffer::GetTime,
                                     WriteBuffer::PutTime, WriteBuffer::Compare,
                                     llarp::util::NullMutex,
                                     llarp::util::NullLock, 5, 100, 128 >
        LossyWriteQueue_t;

    typedef std::deque< WriteBuffer > LosslessWriteQueue_t;

#ifndef _WIN32
    int fd;

    ev_io(int f) : fd(f)
    {
    }

    /// for tun
    ev_io(int f, LossyWriteQueue_t* lossyqueue)
        : fd(f), m_LossyWriteQueue(lossyqueue)
    {
    }

    /// for tcp
    ev_io(int f, LosslessWriteQueue_t* q) : fd(f), m_BlockingWriteQueue(q)
    {
    }
#else
    // on windows, udp event loops are socket fds
    // and TUN device is a plain old fd
    std::variant< SOCKET, HANDLE > fd;
    // the unique completion key that helps us to
    // identify the object instance for which we receive data
    // Here, we'll use the address of the udp_listener instance, converted
    // to its literal int/int64 representation.
    ULONG_PTR listener_id = 0;
    ev_io(SOCKET f) : fd(f), m_writeq("writequeue"){};
    ev_io(HANDLE t)
        : fd(t), m_writeq("writequeue"){};  // overload for TUN device, which
                                            // _is_ a regular file descriptor
#endif
    virtual int
    read(void* buf, size_t sz) = 0;

    virtual int
    sendto(const sockaddr* dst, const void* data, size_t sz)
    {
      return -1;
    };

    virtual void
    tick(){};

    /// used for tun interface and tcp conn
    ssize_t
    do_write(void* data, size_t sz)
    {
#ifndef _WIN32
      return write(fd, data, sz);
#else
      DWORD w;
      WriteFile(std::get< HANDLE >(fd), data, sz, &w, nullptr);
      return w;
#endif
    }

    bool
    queue_write(const byte_t* buf, size_t sz)
    {
      if(m_LossyWriteQueue)
      {
        m_LossyWriteQueue->Emplace(buf, sz);
        return true;
      }
      else if(m_BlockingWriteQueue)
      {
        m_BlockingWriteQueue->emplace_back(buf, sz);
        return true;
      }
      else
        return false;
    }

    /// called in event loop when fd is ready for writing
    /// requeues anything not written
    /// this assumes fd is set to non blocking
    virtual void
    flush_write()
    {
      if(m_LossyWriteQueue)
        m_LossyWriteQueue->Process([&](WriteBuffer& buffer) {
          do_write(buffer.buf, buffer.bufsz);
          // if we would block we save the entries for later
          // discard entry
        });
      else if(m_BlockingWriteQueue)
      {
        // write buffers
        while(m_BlockingWriteQueue->size())
        {
          auto& itr      = m_BlockingWriteQueue->front();
          ssize_t result = do_write(itr.buf, itr.bufsz);
          if(result == -1)
            return;
          ssize_t dlt = itr.bufsz - result;
          if(dlt > 0)
          {
            // queue remaining to front of queue
            WriteBuffer buff(itr.buf + dlt, itr.bufsz - dlt);
            m_BlockingWriteQueue->pop_front();
            m_BlockingWriteQueue->push_front(buff);
            // TODO: errno?
            return;
          }
          m_BlockingWriteQueue->pop_front();
          if(errno == EAGAIN || errno == EWOULDBLOCK)
          {
            errno = 0;
            return;
          }
        }
      }
      /// reset errno
      errno = 0;
#if _WIN32
      SetLastError(0);
#endif
    }

    std::unique_ptr< LossyWriteQueue_t > m_LossyWriteQueue;
    std::unique_ptr< LosslessWriteQueue_t > m_BlockingWriteQueue;
    virtual ~ev_io()
    {
#ifndef _WIN32
      ::close(fd);
#else
      closesocket(std::get< SOCKET >(fd));
#endif
    };
  };
};  // namespace llarp

struct llarp_ev_loop
{
  byte_t readbuf[EV_READ_BUF_SZ];

  virtual bool
  init() = 0;
  virtual int
  run() = 0;

  virtual int
  tick(int ms) = 0;

  virtual void
  stop() = 0;

  bool
  udp_listen(llarp_udp_io* l, const sockaddr* src)
  {
    auto ev = create_udp(l, src);
    if(ev)
    {
#ifdef _WIN32
      l->fd = std::get< SOCKET >(ev->fd);
#else
      l->fd = ev->fd;
#endif
    }
    return ev && add_ev(ev, false);
  }

  virtual llarp::ev_io*
  create_udp(llarp_udp_io* l, const sockaddr* src) = 0;

  virtual bool
  udp_close(llarp_udp_io* l) = 0;
  virtual bool
  close_ev(llarp::ev_io* ev) = 0;

  virtual llarp::ev_io*
  create_tun(llarp_tun_io* tun) = 0;

  virtual llarp::ev_io*
  bind_tcp(llarp_tcp_acceptor* tcp, const sockaddr* addr) = 0;

  virtual bool
  add_ev(llarp::ev_io* ev, bool write = false) = 0;

  virtual bool
  running() const = 0;

  virtual ~llarp_ev_loop(){};

  std::list< std::unique_ptr< llarp::ev_io > > handlers;

  void
  tick_listeners()
  {
    for(const auto& h : handlers)
      h->tick();
  }
};

#endif

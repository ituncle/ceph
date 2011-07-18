// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#define __STDC_FORMAT_MACROS // for PRId64, etc.

#include "common/ProfLogger.h"
#include "common/Thread.h"
#include "common/config.h"
#include "common/config_obs.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/safe_io.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <map>
#include <poll.h>
#include <sstream>
#include <stdint.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define PFL_SUCCESS ((void*)(intptr_t)0)
#define PFL_FAIL ((void*)(intptr_t)1)
#define COUNT_DISABLED ((uint64_t)(int64_t)-1)

using std::ostringstream;

enum prof_log_data_any_t {
  PROF_LOG_DATA_ANY_NONE,
  PROF_LOG_DATA_ANY_U64,
  PROF_LOG_DATA_ANY_DOUBLE
};

class ProfLogThread : public Thread
{
public:

  static std::string create_shutdown_pipe(int *pipe_rd, int *pipe_wr)
  {
    int pipefd[2];
    int ret = pipe2(pipefd, O_CLOEXEC);
    if (ret < 0) {
      int err = errno;
      ostringstream oss;
      oss << "ProfLogThread::create_shutdown_pipe error: "
	  << cpp_strerror(err);
      return oss.str();
    }

    *pipe_rd = pipefd[0];
    *pipe_wr = pipefd[1];
    return "";
  }

  static std::string bind_and_listen(const std::string &sock_path, int *fd)
  {
    int sock_fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
      int err = errno;
      ostringstream oss;
      oss << "ProfLogThread::bind_and_listen: "
	  << "failed to create socket: " << cpp_strerror(err);
      return oss.str();
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), sock_path.c_str());
    if (bind(sock_fd, (struct sockaddr*)&address,
	     sizeof(struct sockaddr_un)) != 0) {
      int err = errno;
      ostringstream oss;
      oss << "ProfLogThread::bind_and_listen: "
	  << "failed to bind socket: " << cpp_strerror(err);
      close(sock_fd);
      return oss.str();
    }
    if (listen(sock_fd, 5) != 0) {
      int err = errno;
      ostringstream oss;
      oss << "ProfLogThread::bind_and_listen: "
	  << "failed to listen to socket: " << cpp_strerror(err);
      close(sock_fd);
      return oss.str();
    }
    *fd = sock_fd;
    return "";
  }

  ProfLogThread(int sock_fd, int shutdown_fd, ProfLoggerCollection *parent)
    : m_sock_fd(sock_fd),
      m_shutdown_fd(shutdown_fd),
      m_parent(parent)
  {
  }

  virtual ~ProfLogThread()
  {
    if (m_sock_fd != -1)
      close(m_sock_fd);
    if (m_shutdown_fd != -1)
      close(m_shutdown_fd);
  }

  virtual void* entry()
  {
    while (true) {
      struct pollfd fds[2];
      memset(fds, 0, sizeof(fds));
      fds[0].fd = m_sock_fd;
      fds[0].events = POLLOUT | POLLWRBAND;
      fds[1].fd = m_shutdown_fd;
      fds[1].events = POLLIN | POLLRDBAND;

      int ret = poll(fds, 2, NULL);
      if (ret < 0) {
	if (ret == -EINTR) {
	  continue;
	}
	int err = errno;
	lderr(m_parent->m_cct) << "ProfLogThread: poll(2) error: '"
	    << cpp_strerror(err) << dendl;
	return PFL_FAIL;
      }

      if (fds[0].revents & POLLOUT) {
	// Send out some data
	if (!do_accept())
	  return PFL_FAIL;
      }
      if (fds[1].revents & POLLIN) {
	// Parent wants us to shut down
	return PFL_SUCCESS;
      }
    }
  }

private:
  const static int MAX_PFL_RETRIES = 10;

  bool do_accept()
  {
    struct sockaddr_un address;
    socklen_t address_length;
    int connection_fd = accept(m_sock_fd, (struct sockaddr*) &address,
				   &address_length);
    if (connection_fd < 0) {
      int err = errno;
      lderr(m_parent->m_cct) << "ProfLogThread: do_accept error: '"
	  << cpp_strerror(err) << dendl;
      return false;
    }
    FILE *fp = fdopen(m_sock_fd, "w");
    if (!fp) {
      int err = errno;
      lderr(m_parent->m_cct) << "ProfLogThread: failed to fdopen '"
	  << m_sock_fd << "'. error " << cpp_strerror(err) << dendl;
      close(connection_fd);
      return false;
    }
    fprintf(fp, "{");

    {
      Mutex::Locker lck(m_parent->m_lock); // Take lock to access m_loggers
      for (std::set <ProfLogger*>::iterator log = m_parent->m_loggers.begin();
	   log != m_parent->m_loggers.end(); ++log)
      {
	// This will take the logger's lock for short period of time,
	// then release it.
	(*log)->write_json_to_fp(fp);
      }
    }

    fprintf(fp, "}");
    fflush(fp);
    fclose(fp); // calls close(connection_fd)
    return true;
  }

  ProfLogThread(ProfLogThread &rhs);
  const ProfLogThread &operator=(const ProfLogThread &rhs);

  int m_sock_fd;
  int m_shutdown_fd;
  ProfLoggerCollection *m_parent;
};

ProfLoggerCollection::
ProfLoggerCollection(CephContext *cct)
  : m_cct(cct),
    m_thread(NULL),
    m_lock("ProfLoggerCollection"),
    m_shutdown_fd(-1)
{
}

ProfLoggerCollection::
~ProfLoggerCollection()
{
  Mutex::Locker lck(m_lock);
  shutdown();
  for (std::set <ProfLogger*>::iterator l = m_loggers.begin();
	l != m_loggers.end(); ++l) {
    delete *l;
  }
  m_loggers.clear();
}

const char** ProfLoggerCollection::
get_tracked_conf_keys() const
{
  static const char *KEYS[] =
	{ "profiling_logger_uri", NULL };
  return KEYS;
}

void ProfLoggerCollection::
handle_conf_change(const md_config_t *conf,
		   const std::set <std::string> &changed)
{
  Mutex::Locker lck(m_lock);
  if (conf->profiling_logger_uri.empty()) {
    shutdown();
  }
  else {
    if (!init(conf->profiling_logger_uri)) {
      lderr(m_cct) << "Initializing profiling logger failed!" << dendl;
    }
  }
}

void ProfLoggerCollection::
logger_add(class ProfLogger *l)
{
  Mutex::Locker lck(m_lock);
  std::set<ProfLogger*>::iterator i = m_loggers.find(l);
  assert(i == m_loggers.end());
  m_loggers.insert(l);
}

void ProfLoggerCollection::
logger_remove(class ProfLogger *l)
{
  Mutex::Locker lck(m_lock);
  std::set<ProfLogger*>::iterator i = m_loggers.find(l);
  assert(i != m_loggers.end());
  m_loggers.erase(i);
}

bool ProfLoggerCollection::
init(const std::string &uri)
{
  /* Shut down old thread, if it exists.  */
  shutdown();

  /* Set up things for the new thread */
  std::string err;
  int pipe_rd, pipe_wr;
  err = ProfLogThread::create_shutdown_pipe(&pipe_rd, &pipe_wr);
  if (!err.empty()) {
    lderr(m_cct) << "ProfLoggerCollection::init: error: " << err << dendl;
    return false;
  }
  int sock_fd;
  err = ProfLogThread::bind_and_listen(uri, &sock_fd);
  if (!err.empty()) {
    lderr(m_cct) << "ProfLoggerCollection::init: failed: " << err << dendl;
    close(pipe_rd);
    close(pipe_wr);
    return false;
  }

  /* Create new thread */
  m_thread = new (std::nothrow) ProfLogThread(sock_fd, pipe_rd, this);
  if (!m_thread) {
    close(sock_fd);
    close(pipe_rd);
    close(pipe_wr);
    return false;
  }
  m_thread->create();
  m_shutdown_fd = pipe_wr;
  return 0;
}

void ProfLoggerCollection::
shutdown()
{
  if (m_thread) {
    // Send a byte to the shutdown pipe that the thread is listening to
    char buf[1] = { 0x0 };
    int ret = safe_write(m_shutdown_fd, buf, sizeof(buf));
    m_shutdown_fd = -1;

    if (ret == 0) {
      // Join and delete the thread
      m_thread->join();
      delete m_thread;
    }
    else {
      lderr(m_cct) << "ProfLoggerCollection::shutdown: failed to write "
	      "to thread shutdown pipe: error " << ret << dendl;
    }
    m_thread = NULL;
  }
}

ProfLogger::
~ProfLogger()
{
}

void ProfLogger::
inc(int idx, uint64_t amt)
{
  Mutex::Locker lck(m_lock);
  assert(idx > m_lower_bound);
  assert(idx < m_upper_bound);
  prof_log_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (data.type != PROF_LOG_DATA_ANY_U64)
    return;
  data.u.u64 += amt;
  if (data.count != COUNT_DISABLED)
    data.count++;
}

void ProfLogger::
set(int idx, uint64_t amt)
{
  Mutex::Locker lck(m_lock);
  assert(idx > m_lower_bound);
  assert(idx < m_upper_bound);
  prof_log_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (data.type != PROF_LOG_DATA_ANY_U64)
    return;
  data.u.u64 = amt;
  if (data.count != COUNT_DISABLED)
    data.count++;
}

uint64_t ProfLogger::
get(int idx)
{
  Mutex::Locker lck(m_lock);
  assert(idx > m_lower_bound);
  assert(idx < m_upper_bound);
  prof_log_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (data.type != PROF_LOG_DATA_ANY_DOUBLE)
    return 0;
  return data.u.u64;
}

void ProfLogger::
finc(int idx, double amt)
{
  Mutex::Locker lck(m_lock);
  assert(idx > m_lower_bound);
  assert(idx < m_upper_bound);
  prof_log_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (data.type != PROF_LOG_DATA_ANY_DOUBLE)
    return;
  data.u.dbl += amt;
  if (data.count != COUNT_DISABLED)
    data.count++;
}

void ProfLogger::
fset(int idx, double amt)
{
  Mutex::Locker lck(m_lock);
  assert(idx > m_lower_bound);
  assert(idx < m_upper_bound);
  prof_log_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (data.type != PROF_LOG_DATA_ANY_DOUBLE)
    return;
  data.u.dbl = amt;
  if (data.count != COUNT_DISABLED)
    data.count++;
}

double ProfLogger::
fget(int idx)
{
  Mutex::Locker lck(m_lock);
  assert(idx > m_lower_bound);
  assert(idx < m_upper_bound);
  prof_log_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (data.type != PROF_LOG_DATA_ANY_DOUBLE)
    return 0.0;
  return data.u.dbl;
}

void ProfLogger::
write_json_to_fp(FILE *fp)
{
  Mutex::Locker lck(m_lock);

  prof_log_data_vec_t::const_iterator d = m_data.begin();
  prof_log_data_vec_t::const_iterator d_end = m_data.end();
  for (; d != d_end; ++d) {
    const prof_log_data_any_d &data(*d);
    if (d->count != COUNT_DISABLED) {
      switch (d->type) {
	case PROF_LOG_DATA_ANY_U64:
	  fprintf(fp, "\"%s\" : { \"count\" : %" PRId64 ", "
		  "\"sum\" : %" PRId64 " },\n", 
		  data.name, data.count, data.u.u64);
	  break;
	case PROF_LOG_DATA_ANY_DOUBLE:
	  fprintf(fp, "\"%s\" : { \"count\" : %" PRId64 ", "
		  "\"sum\" : %g },\n",
		  data.name, data.count, data.u.dbl);
	  break;
	default:
	  assert(0);
	  break;
      }
    }
    else {
      switch (d->type) {
	case PROF_LOG_DATA_ANY_U64:
	  fprintf(fp, "\"%s\" : %" PRId64 ",\n", data.name, data.u.u64);
	  break;
	case PROF_LOG_DATA_ANY_DOUBLE:
	  fprintf(fp, "\"%s\" : %g,\n", data.name, data.u.dbl);
	  break;
	default:
	  assert(0);
	  break;
      }
    }
  }
}

ProfLogger::
ProfLogger(CephContext *cct, const std::string &name,
	   int lower_bound, int upper_bound)
  : m_cct(cct),
    m_lower_bound(lower_bound),
    m_upper_bound(upper_bound),
    m_name(std::string("ProfLogger::") + name.c_str()),
    m_lock(m_name.c_str())
{
  m_data.resize(upper_bound - lower_bound - 1);
}

ProfLogger::prof_log_data_any_d::
prof_log_data_any_d()
  : name(NULL),
    type(PROF_LOG_DATA_ANY_NONE),
    count(COUNT_DISABLED)
{
  memset(&u, 0, sizeof(u));
}

ProfLoggerBuilder::
ProfLoggerBuilder(CephContext *cct, const std::string &name,
                  int first, int last)
  : m_prof_logger(new ProfLogger(cct, name, first, last))
{
}

ProfLoggerBuilder::
~ProfLoggerBuilder()
{
  if (m_prof_logger)
    delete m_prof_logger;
  m_prof_logger = NULL;
}

void ProfLoggerBuilder::
add_u64(int idx, const char *name)
{
  add_impl(idx, name, PROF_LOG_DATA_ANY_U64, COUNT_DISABLED);
}

void ProfLoggerBuilder::
add_fl(int idx, const char *name)
{
  add_impl(idx, name, PROF_LOG_DATA_ANY_DOUBLE, COUNT_DISABLED);
}

void ProfLoggerBuilder::
add_fl_avg(int idx, const char *name)
{
  add_impl(idx, name, PROF_LOG_DATA_ANY_DOUBLE, 0);
}

void ProfLoggerBuilder::
add_impl(int idx, const char *name, int ty, uint64_t count)
{
  assert(idx > m_prof_logger->m_lower_bound);
  assert(idx < m_prof_logger->m_upper_bound);
  ProfLogger::prof_log_data_vec_t &vec(m_prof_logger->m_data);
  ProfLogger::prof_log_data_any_d
    &data(vec[idx - m_prof_logger->m_lower_bound - 1]);
  data.name = name;
  data.type = ty;
  data.count = count;
}

ProfLogger *ProfLoggerBuilder::
create_proflogger()
{
  ProfLogger::prof_log_data_vec_t::const_iterator d = m_prof_logger->m_data.begin();
  ProfLogger::prof_log_data_vec_t::const_iterator d_end = m_prof_logger->m_data.end();
  for (; d != d_end; ++d) {
    assert(d->type != PROF_LOG_DATA_ANY_NONE);
  }
  ProfLogger *ret = m_prof_logger;
  m_prof_logger = NULL;
  return ret;
}

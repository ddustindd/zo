/**
 * This program and the accompanying materials are made available under the terms of the
 * Eclipse Public License v2.0 which accompanies this distribution, and is available at
 * https://www.eclipse.org/legal/epl-v20.html
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Copyright Contributors to the Zowe Project.
 *
 */

#ifndef ZLOGGER_HPP
#define ZLOGGER_HPP

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "singleton.hpp"
#include "zlogger_metal.h"
#include "zlog_util.hpp"

typedef zlog_level_t LogLevel;

class ZLogger : public Singleton<ZLogger>
{
  friend class Singleton<ZLogger>;

private:
  static constexpr size_t LOG_BUFFER_SIZE = 4096;
  static constexpr size_t MAX_LOG_FILE_SIZE = 100 * 1024;
  static constexpr int MAX_LOG_FILES = 10;

  std::mutex m_log_mutex;
  std::ofstream m_log_file;
  std::string m_log_file_path;
  std::atomic<int> m_level;
  std::atomic<bool> m_server_mode{false};
  bool m_initialized = false;
  bool m_pending_initialization = true;
  bool m_rotation_stall_warned = false;
#if defined(ZLOG_ENABLE) && defined(__MVS__)
  bool m_metal_initialized = false;
#endif

  static int get_level_from_environment(int default_level)
  {
    const char *env_level = std::getenv("ZO_LOG_LEVEL");
    if (!env_level || !*env_level)
    {
      return default_level;
    }

    std::string level(env_level);
    std::transform(level.begin(), level.end(), level.begin(),
                   [](unsigned char ch)
    { return std::toupper(ch); });
    if (level == "WARNING")
    {
      return ZLOGLEVEL_WARN;
    }
    for (int i = ZLOGLEVEL_TRACE; i <= ZLOGLEVEL_OFF; ++i)
    {
      if (level == get_level_name(static_cast<LogLevel>(i)))
      {
        return i;
      }
    }
    return default_level;
  }

  static const char *get_level_name(LogLevel level)
  {
    static const char *const names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"};
    return names[level];
  }

  void write_timestamp()
  {
    const auto now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);
    m_log_file << timestamp;
  }

  bool open_log_file(bool truncate)
  {
    int fd = open(m_log_file_path.c_str(), O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND), 0600);
    if (fd == -1)
    {
      std::cerr << "Failed to create log file: " << m_log_file_path << std::endl;
      return false;
    }
    close(fd);

    m_log_file.clear();
    m_log_file.open(m_log_file_path.c_str(), std::ios::out | std::ios::app);
    if (!m_log_file.is_open())
    {
      std::cerr << "Failed to open log file: " << m_log_file_path << std::endl;
      return false;
    }
    return true;
  }

  void initialize_log_file(bool truncate)
  {
    m_pending_initialization = false;
    const std::string logs_dir = zlog_util::resolve_logs_dir();
    if (!zlog_util::make_dirs(logs_dir))
    {
      std::cerr << "Failed to create logs directory: " << logs_dir << std::endl;
      return;
    }

    m_log_file_path = logs_dir + (m_server_mode ? "/zowex_server.log" : "/zowex.log");
    m_initialized = open_log_file(truncate);
#if defined(ZLOG_ENABLE) && defined(__MVS__)
    // BSAM must not share a file with the C++ append stream.
    const std::string metal_path = logs_dir + "/zowex_metal.log";
    int level = m_level.load(std::memory_order_relaxed);
    m_metal_initialized = ZLGINIT(metal_path.c_str(), &level) == 0;
    if (!m_metal_initialized)
    {
      std::cerr << "Failed to initialize Metal C logger" << std::endl;
    }
#endif
  }

  void close_log_file()
  {
    if (m_log_file.is_open())
    {
      m_log_file.close();
    }
    m_initialized = false;
#if defined(ZLOG_ENABLE) && defined(__MVS__)
    if (m_metal_initialized)
    {
      ZLGCLEAN();
      m_metal_initialized = false;
    }
#endif
  }

  void configure(bool server_mode, bool verbose, bool truncate)
  {
    close_log_file();
    m_server_mode.store(server_mode, std::memory_order_relaxed);
    m_level.store(get_level_from_environment(verbose ? ZLOGLEVEL_DEBUG : ZLOGLEVEL_INFO), std::memory_order_relaxed);
    m_rotation_stall_warned = false;
    initialize_log_file(truncate);
  }

  // Never replace the active log unless it has been archived successfully.
  bool rotate_log_files()
  {
    const std::string oldest = m_log_file_path + "." + std::to_string(MAX_LOG_FILES - 1);
    unlink(oldest.c_str());
    for (int i = MAX_LOG_FILES - 2; i >= 1; --i)
    {
      const std::string from = m_log_file_path + "." + std::to_string(i);
      const std::string to = m_log_file_path + "." + std::to_string(i + 1);
      rename(from.c_str(), to.c_str());
    }

    const std::string first_backup = m_log_file_path + ".1";
    if (rename(m_log_file_path.c_str(), first_backup.c_str()) != 0)
    {
      std::cerr << "Failed to archive log file to " << first_backup << ": " << strerror(errno) << std::endl;
      return false;
    }
    return true;
  }

  void check_and_rotate_log_file()
  {
    struct stat st;
    if (stat(m_log_file_path.c_str(), &st) != 0 || static_cast<size_t>(st.st_size) <= MAX_LOG_FILE_SIZE)
    {
      return;
    }

    m_log_file.close();
    if (!rotate_log_files())
    {
      m_log_file.clear();
      m_log_file.open(m_log_file_path.c_str(), std::ios::out | std::ios::app);
      m_initialized = m_log_file.is_open();
      if (!m_initialized)
      {
        std::cerr << "Failed to reopen log file after failed rotation: " << m_log_file_path << std::endl;
      }
      else if (!m_rotation_stall_warned && static_cast<size_t>(st.st_size) > 2 * MAX_LOG_FILE_SIZE)
      {
        write_timestamp();
        m_log_file << " [WARN] Log rotation has been failing repeatedly; file size (" << st.st_size
                   << " bytes) exceeds twice the configured limit\n";
        m_log_file.flush();
        m_rotation_stall_warned = true;
      }
      return;
    }

    m_rotation_stall_warned = false;
    m_initialized = open_log_file(true);
    if (m_initialized)
    {
      write_timestamp();
      m_log_file << " [INFO] Log file rolled over due to size limit\n";
      m_log_file.flush();
    }
  }

  bool should_log(LogLevel level) const
  {
    return level >= ZLOGLEVEL_TRACE && level < ZLOGLEVEL_OFF && level >= m_level.load(std::memory_order_relaxed);
  }

  void write_message(LogLevel level, const char *message)
  {
    const std::lock_guard<std::mutex> lock(m_log_mutex);
    if (!should_log(level))
    {
      return;
    }
    if (m_pending_initialization)
    {
      initialize_log_file(false);
    }
    if (!m_initialized)
    {
      return;
    }

    write_timestamp();
    m_log_file << " [" << get_level_name(level) << "] " << message << '\n';
    m_log_file.flush();
    check_and_rotate_log_file();
  }

protected:
  ZLogger() : m_level(get_level_from_environment(ZLOGLEVEL_INFO))
  {
#if defined(ZLOG_ENABLE) && defined(__MVS__)
    initialize();
#endif
  }

public:
  ~ZLogger()
  {
    shutdown();
  }

  void initialize()
  {
    const std::lock_guard<std::mutex> lock(m_log_mutex);
    configure(false, false, false);
  }

  void initialize_server(bool verbose = false, bool truncate = false)
  {
    const std::lock_guard<std::mutex> lock(m_log_mutex);
    configure(true, verbose, truncate);
  }

  bool is_server_mode() const
  {
    return m_server_mode.load(std::memory_order_relaxed);
  }

  bool is_verbose_logging() const
  {
    return get_log_level() <= ZLOGLEVEL_DEBUG;
  }

  void set_log_level(LogLevel level)
  {
    const std::lock_guard<std::mutex> lock(m_log_mutex);
    m_level.store(level, std::memory_order_relaxed);
#if defined(ZLOG_ENABLE) && defined(__MVS__)
    if (m_metal_initialized)
    {
      int level_value = level;
      ZLGSTLVL(&level_value);
    }
#endif
  }

  int get_log_level() const
  {
    return m_level.load(std::memory_order_relaxed);
  }

  void shutdown()
  {
    const std::lock_guard<std::mutex> lock(m_log_mutex);
    close_log_file();
    m_pending_initialization = false;
    m_level.store(ZLOGLEVEL_OFF, std::memory_order_relaxed);
  }

  void vlog(LogLevel level, const char *format, va_list args)
  {
    if (!format || !should_log(level))
    {
      return;
    }
    char buffer[LOG_BUFFER_SIZE];
    vsnprintf(buffer, sizeof(buffer), format, args);
    write_message(level, buffer);
  }

  void log(LogLevel level, const char *format, ...)
  {
    va_list args;
    va_start(args, format);
    vlog(level, format, args);
    va_end(args);
  }

  void trace(const char *format, ...)
  {
    va_list args;
    va_start(args, format);
    vlog(ZLOGLEVEL_TRACE, format, args);
    va_end(args);
  }

  void debug(const char *format, ...)
  {
    va_list args;
    va_start(args, format);
    vlog(ZLOGLEVEL_DEBUG, format, args);
    va_end(args);
  }

  void info(const char *format, ...)
  {
    va_list args;
    va_start(args, format);
    vlog(ZLOGLEVEL_INFO, format, args);
    va_end(args);
  }

  void warn(const char *format, ...)
  {
    va_list args;
    va_start(args, format);
    vlog(ZLOGLEVEL_WARN, format, args);
    va_end(args);
  }

  void error(const char *format, ...)
  {
    va_list args;
    va_start(args, format);
    vlog(ZLOGLEVEL_ERROR, format, args);
    va_end(args);
  }

  void fatal(const char *format, ...)
  {
    va_list args;
    va_start(args, format);
    vlog(ZLOGLEVEL_FATAL, format, args);
    va_end(args);
  }

  void fatal(const char *format, int exit_code)
  {
    log(ZLOGLEVEL_FATAL, format);
    std::exit(exit_code);
  }

  [[noreturn]] void fatal_exit(const char *format, ...)
  {
    char buffer[LOG_BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    write_message(ZLOGLEVEL_FATAL, buffer);
    std::cerr << "FATAL: " << buffer << std::endl;
    std::exit(1);
  }
};

#define ZLOG_TRACE(...) ZLogger::get_instance().trace(__VA_ARGS__)
#define ZLOG_DEBUG(...) ZLogger::get_instance().debug(__VA_ARGS__)
#define ZLOG_INFO(...) ZLogger::get_instance().info(__VA_ARGS__)
#define ZLOG_WARN(...) ZLogger::get_instance().warn(__VA_ARGS__)
#define ZLOG_ERROR(...) ZLogger::get_instance().error(__VA_ARGS__)
#define ZLOG_FATAL(...) ZLogger::get_instance().log(ZLOGLEVEL_FATAL, __VA_ARGS__)

#define LOG_TRACE(...) ZLogger::get_instance().trace(__VA_ARGS__)
#define LOG_DEBUG(...) ZLogger::get_instance().debug(__VA_ARGS__)
#define LOG_INFO(...) ZLogger::get_instance().info(__VA_ARGS__)
#define LOG_WARN(...) ZLogger::get_instance().warn(__VA_ARGS__)
#define LOG_ERROR(...) ZLogger::get_instance().error(__VA_ARGS__)
#define LOG_FATAL(...) ZLogger::get_instance().fatal_exit(__VA_ARGS__)

#endif // ZLOGGER_HPP

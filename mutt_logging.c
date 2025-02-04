/**
 * @file
 * Mutt Logging
 *
 * @authors
 * Copyright (C) 2018 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page mutt_logging Mutt Logging
 *
 * Mutt Logging
 */

#include "config.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "mutt/mutt.h"
#include "curs_lib.h"
#include "globals.h"
#include "mutt_curses.h"
#include "mutt_window.h"
#include "muttlib.h"
#include "options.h"

struct timeval LastError = { 0 };

short C_DebugLevel = 0;   ///< Config: Logging level for debug logs
char *C_DebugFile = NULL; ///< Config: File to save debug logs
char *CurrentFile = NULL; /**< The previous log file name */
const int NumOfLogs = 5;  /**< How many log files to rotate */

#define S_TO_NS 1000000000UL
#define S_TO_US 1000000UL
#define US_TO_NS 1000UL

/**
 * micro_elapsed - Number of microseconds between two timevals
 * @param begin Begin time
 * @param end   End time
 * @retval num      Microseconds elapsed
 * @retval LONG_MAX Begin time was zero
 */
static long micro_elapsed(const struct timeval *begin, const struct timeval *end)
{
  if ((begin->tv_sec == 0) && (end->tv_sec != 0))
    return LONG_MAX;

  return (end->tv_sec - begin->tv_sec) * S_TO_US + (end->tv_usec - begin->tv_usec);
}

/**
 * error_pause - Wait for an error message to be read
 *
 * If '$sleep_time' seconds hasn't elapsed since LastError, then wait
 */
static void error_pause(void)
{
  struct timeval now = { 0 };

  if (gettimeofday(&now, NULL) < 0)
  {
    mutt_debug(LL_DEBUG1, "gettimeofday failed: %d\n", errno);
    return;
  }

  unsigned long sleep = C_SleepTime * S_TO_NS;
  long micro = micro_elapsed(&LastError, &now);
  if ((micro * US_TO_NS) >= sleep)
    return;

  sleep -= (micro * US_TO_NS);

  struct timespec wait = {
    .tv_sec = (sleep / S_TO_NS),
    .tv_nsec = (sleep % S_TO_NS),
  };

  mutt_refresh();
  nanosleep(&wait, NULL);
}

/**
 * rotate_logs - Rotate a set of numbered files
 * @param file  Template filename
 * @param count Maximum number of files
 * @retval ptr Name of the 0'th file
 *
 * Given a template 'temp', rename files numbered 0 to (count-1).
 *
 * Rename:
 * - ...
 * - temp1 -> temp2
 * - temp0 -> temp1
 */
static const char *rotate_logs(const char *file, int count)
{
  if (!file)
    return NULL;

  char old[PATH_MAX];
  char new[PATH_MAX];

  /* rotate the old debug logs */
  for (count -= 2; count >= 0; count--)
  {
    snprintf(old, sizeof(old), "%s%d", file, count);
    snprintf(new, sizeof(new), "%s%d", file, count + 1);

    mutt_expand_path(old, sizeof(old));
    mutt_expand_path(new, sizeof(new));
    rename(old, new);
  }

  return mutt_str_strdup(old);
}

/**
 * mutt_clear_error - Clear the message line (bottom line of screen)
 */
void mutt_clear_error(void)
{
  /* Make sure the error message has had time to be read */
  if (OptMsgErr)
    error_pause();

  ErrorBufMessage = false;
  if (!OptNoCurses)
    mutt_window_clearline(MuttMessageWindow, 0);
}

/**
 * log_disp_curses - Display a log line in the message line - Implements ::log_dispatcher_t
 */
int log_disp_curses(time_t stamp, const char *file, int line,
                    const char *function, int level, ...)
{
  if (level > C_DebugLevel)
    return 0;

  char buf[1024];

  va_list ap;
  va_start(ap, level);
  const char *fmt = va_arg(ap, const char *);
  int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (level == LL_PERROR)
  {
    char *buf2 = buf + ret;
    int len = sizeof(buf) - ret;
    char *p = strerror(errno);
    if (!p)
      p = _("unknown error");

    ret += snprintf(buf2, len, ": %s (errno = %d)", p, errno);
  }

  const bool dupe = (strcmp(buf, ErrorBuf) == 0);
  if (!dupe)
  {
    /* Only log unique messages */
    log_disp_file(stamp, file, line, function, level, "%s", buf);
    if (stamp == 0)
      log_disp_queue(stamp, file, line, function, level, "%s", buf);
  }

  /* Don't display debugging message on screen */
  if (level > LL_MESSAGE)
    return 0;

  /* Only pause if this is a message following an error */
  if ((level > LL_ERROR) && OptMsgErr && !dupe)
    error_pause();

  mutt_simple_format(ErrorBuf, sizeof(ErrorBuf), 0, MuttMessageWindow->cols,
                     FMT_LEFT, 0, buf, sizeof(buf), 0);
  ErrorBufMessage = true;

  if (!OptKeepQuiet)
  {
    if (level == LL_ERROR)
      BEEP();
    SETCOLOR((level == LL_ERROR) ? MT_COLOR_ERROR : MT_COLOR_MESSAGE);
    mutt_window_mvaddstr(MuttMessageWindow, 0, 0, ErrorBuf);
    NORMAL_COLOR;
    mutt_window_clrtoeol(MuttMessageWindow);
    mutt_refresh();
  }

  if ((level <= LL_ERROR) && !dupe)
  {
    OptMsgErr = true;
    if (gettimeofday(&LastError, NULL) < 0)
      mutt_debug(LL_DEBUG1, "gettimeofday failed: %d\n", errno);
  }
  else
  {
    OptMsgErr = false;
    LastError.tv_sec = 0;
  }

  return ret;
}

/**
 * mutt_log_prep - Prepare to log
 */
void mutt_log_prep(void)
{
  char ver[64];
  snprintf(ver, sizeof(ver), "-%s%s", PACKAGE_VERSION, GitVer);
  log_file_set_version(ver);
}

/**
 * mutt_log_stop - Close the log file
 */
void mutt_log_stop(void)
{
  log_file_close(false);
  FREE(&CurrentFile);
}

/**
 * mutt_log_set_file - Change the logging file
 * @param file Name to use
 * @param verbose If true, then log the event
 * @retval  0 Success, file opened
 * @retval -1 Error, see errno
 *
 * Close the old log, rotate the new logs and open the new log.
 */
int mutt_log_set_file(const char *file, bool verbose)
{
  if (mutt_str_strcmp(CurrentFile, C_DebugFile) != 0)
  {
    const char *name = rotate_logs(C_DebugFile, NumOfLogs);
    if (!name)
      return -1;

    log_file_set_filename(name, false);
    FREE(&name);
    mutt_str_replace(&CurrentFile, C_DebugFile);
  }

  cs_str_string_set(Config, "debug_file", file, NULL);

  return 0;
}

/**
 * mutt_log_set_level - Change the logging level
 * @param level Logging level
 * @param verbose If true, then log the event
 * @retval  0 Success
 * @retval -1 Error, level is out of range
 */
int mutt_log_set_level(int level, bool verbose)
{
  if (!CurrentFile)
    mutt_log_set_file(C_DebugFile, false);

  if (log_file_set_level(level, verbose) != 0)
    return -1;

  cs_str_native_set(Config, "debug_level", level, NULL);
  return 0;
}

/**
 * mutt_log_start - Enable file logging
 * @retval  0 Success, or already running
 * @retval -1 Failed to start
 *
 * This also handles file rotation.
 */
int mutt_log_start(void)
{
  if (C_DebugLevel < 1)
    return 0;

  if (log_file_running())
    return 0;

  mutt_log_set_file(C_DebugFile, false);

  /* This will trigger the file creation */
  if (log_file_set_level(C_DebugLevel, true) < 0)
    return -1;

  return 0;
}

/**
 * level_validator - Validate the "debug_level" config variable
 * @param cs    Config items
 * @param cdef  Config definition
 * @param value Native value
 * @param err   Message for the user
 * @retval CSR_SUCCESS     Success
 * @retval CSR_ERR_INVALID Failure
 */
int level_validator(const struct ConfigSet *cs, const struct ConfigDef *cdef,
                    intptr_t value, struct Buffer *err)
{
  if ((value < 0) || (value > LL_DEBUG5))
  {
    mutt_buffer_printf(err, _("Invalid value for option %s: %ld"), cdef->name, value);
    return CSR_ERR_INVALID;
  }

  return CSR_SUCCESS;
}

/**
 * mutt_log_listener - Listen for config changes affecting the log file - Implements ::cs_listener()
 */
bool mutt_log_listener(const struct ConfigSet *cs, struct HashElem *he,
                       const char *name, enum ConfigEvent ev)
{
  if (mutt_str_strcmp(name, "debug_file") == 0)
    mutt_log_set_file(C_DebugFile, true);
  else if (mutt_str_strcmp(name, "debug_level") == 0)
    mutt_log_set_level(C_DebugLevel, true);

  return true;
}

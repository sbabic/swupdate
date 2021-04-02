/*
 * (C) Copyright 2016
 * Denis Osterland, Diehl Connectivity Solutions GmbH, Denis.Osterland@diehl.com.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
#include <syslog.h>
#include <stdio.h>
#include "util.h"

static void syslog_notifier(RECOVERY_STATUS status, int error, int level, const char *msg);

int syslog_init(void)
{
   setlogmask(LOG_UPTO(LOG_DEBUG));
   return register_notifier(syslog_notifier);
}

void syslog_notifier(RECOVERY_STATUS status, int error, int level, const char *msg)
{
   const char* statusMsg;

   switch(status) {
      case IDLE: statusMsg = "IDLE"; break;
      case DOWNLOAD: statusMsg = "DOWNLOAD"; break;
      case START: statusMsg = "START"; break;
      case RUN: statusMsg = "RUN"; break;
      case SUCCESS: statusMsg = "SUCCESS"; break;
      case FAILURE: statusMsg = "FAILURE"; break;
      case DONE: statusMsg = "DONE"; break;
      /*
       * Unknown messages are maybe for other subsystems
       * and not to the logger, so silently ignore them
       */
      default: return;
   }

   openlog("swupdate", 0, LOG_USER);

   int logprio = LOG_INFO;
   switch (level) {
      case ERRORLEVEL: logprio = LOG_ERR; break;
      case WARNLEVEL:  logprio = LOG_WARNING; break;
      case INFOLEVEL:  logprio = LOG_INFO; break;
      case DEBUGLEVEL:
      case TRACELEVEL: logprio = LOG_DEBUG; break;
   }

   syslog(logprio, "%s%s %s\n", ((error != (int)RECOVERY_NO_ERROR) ? "FATAL_" : ""), statusMsg, msg);

   closelog();
}


/*
 * (C) Copyright 2016
 * Denis Osterland, Diehl Connectivity Solutions GmbH, Denis.Osterland@diehl.com.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.   See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
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

   openlog("swupdate", 0, LOG_USER);

   switch(status) {
      case IDLE: statusMsg = "IDLE"; break;
      case DOWNLOAD: statusMsg = "DOWNLOAD"; break;
      case START: statusMsg = "START"; break;
      case RUN: statusMsg = "RUN"; break;
      case SUCCESS: statusMsg = "SUCCESS"; break;
      case FAILURE: statusMsg = "FAILURE"; break;
      case DONE: statusMsg = "DONE"; break;
      default: statusMsg = "UNKNOWN"; break;
   }

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


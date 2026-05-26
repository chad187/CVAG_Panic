#ifndef TASKS_H
#define TASKS_H

#include <TaskSchedulerDeclarations.h>

extern Scheduler ts;
extern Task taskScreenTimeout;
extern Task taskMidnightCheck;
extern Task taskClockRefresh;
extern Task taskDebounce;
extern Task taskWiFiConnect; // Added background connection worker task

#endif
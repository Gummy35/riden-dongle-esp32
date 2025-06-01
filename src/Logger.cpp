#include <Logger.h>

LoggerClass::LoggerClass()
{
    _logfunc = nullptr;
}

void LoggerClass::SetLogger(void (*log)(const char *logString))
{
    _logfunc = log;
}

void LoggerClass::Log(const char *logString)
{
    if (_logfunc != nullptr)
        _logfunc(logString);
}


LoggerClass Logger;
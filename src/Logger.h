#include <Arduino.h>

class LoggerClass
{
    public:
        LoggerClass();
        void SetLogger(void (*log)(const char *logString));
        void Log(const char *logString);
    private:        
        void (*_logfunc)(const char *logString) = nullptr;
};

extern LoggerClass Logger;
#ifndef MIM_ERR_H
#define MIM_ERR_H

#ifdef __cplusplus
    #define NORETURN [[noreturn]]
#else
    #define NORETURN _Noreturn
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Print information about a system error and quits.
NORETURN void syserr(const char* fmt, ...);

// Print information about an error and quits.
NORETURN void fatal(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif

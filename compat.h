#ifndef UTCP_COMPAT_H
#define UTCP_COMPAT_H

#ifndef _MSC_VER
  #include <unistd.h>
#endif

#ifdef _WIN32
  #include <winsock2.h>
#else
  #include <sys/time.h>
#endif

#ifndef PRINT_SIZE_T
  #ifdef _WIN32
    #define PRINT_SIZE_T "%Iu"
    #define PRINT_SSIZE_T "%Id"
  #else
    #define PRINT_SIZE_T "%zu"
    #define PRINT_SSIZE_T "%zd"
  #endif
#endif

#ifdef _MSC_VER
  // VS2012 and up has no ssize_t defined, before it was defined as unsigned int
  #ifndef _SSIZE_T_DEFINED
    #define _SSIZE_T_DEFINED
    #undef ssize_t
    #ifdef _WIN64
      typedef signed __int64  ssize_t;
    #else
      typedef signed int      ssize_t;
    #endif
  #endif
#endif

#endif // UTCP_COMPAT_H

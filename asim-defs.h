#ifndef ASIM_DEFS_H
#define ASIM_DEFS_H

#include <stdint.h>
#include <string>
#include <iostream>
#include <assert.h>

using namespace std;

// Define types used by ASIM code
typedef int8_t INT8;
typedef uint8_t UINT8;

typedef int16_t INT16;
typedef uint16_t UINT16;

typedef int32_t INT32;
typedef uint32_t UINT32;

typedef int64_t INT64;
typedef uint64_t UINT64;

// Define error reporting macro
#define ASIMERROR(_text) std::cerr << "ERROR: " << _text
#define ASIMWARNING(_text) std::cerr << "WARNING: " << _text

#ifndef ASSERT
#define ASSERT assert
#endif

#ifndef ASSERTX
#define ASSERTX assert
#endif

// Stub out tracing macros

#if 1
#define ACCEL_T2(out)
#else
#define ACCEL_T2(out) std::cerr << out << std::endl;
#endif

#define ACCEL_T_INIT_BEGIN()
#define ACCEL_T_INIT_END()

#endif // ASIM_DEFS_H

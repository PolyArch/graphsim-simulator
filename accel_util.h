/*
 * Copyright (C) 2015-2019 Intel Corporation. All rights reserved.
 *
 * The information and source code contained herein is the exclusive
 * property of Intel Corporation and may not be disclosed, examined
 * or reproduced in whole or in part without explicit written authorization
 * from the company.
 */

#ifndef ACCEL_UTIL_H
#define ACCEL_UTIL_H

#include <fstream>
#include <string.h>
#include "accel_knobs.h"

namespace MEMTRACE
{

void debugBreak();
void assert_fail();
inline bool strToBool(const char *value);
void checkForQuotes(const char *value);
bool accel_boolean_env(const char *env, bool defaultValue);
void toUpper(char *knob);
} // namespace MEMTRACE

#endif // ACCEL_UTIL_H

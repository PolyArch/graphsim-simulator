/*
 * Copyright (C) 2015-2019 Intel Corporation. All rights reserved.
 *
 * The information and source code contained herein is the exclusive
 * property of Intel Corporation and may not be disclosed, examined
 * or reproduced in whole or in part without explicit written authorization
 * from the company.
 */

#ifndef GRAPH_CONSTANTS_H
#define GRAPH_CONSTANTS_H

namespace MEMTRACE
{

// Called during initialization to override defaults
void overrideDefaultConstants();

enum KnobBatchValue
{
    KNOB_ACCELSIM_BATCH_OFF = 0,
    KNOB_ACCELSIM_BATCH_ON = 1,
    KNOB_ACCELSIM_BATCH_AUTO = 2
};

}; // namespace MEMTRACE

#endif // GRAPH_CONSTANTS_H

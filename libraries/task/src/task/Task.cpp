//
//  Task.cpp
//  task/src/task
//
//  Created by Sam Gateau on 2/14/2018.
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "Task.h"

using namespace task;

JobContext::JobContext(const QLoggingCategory& category) :
    profileCategory(category) {
    assert(&category);
}

JobContext::~JobContext() {
}

void JobContext::resetTaskFlow() {
    _doAbortTask = false;
}

void JobContext::abortTask() {
    _doAbortTask = true;
}

bool JobContext::doAbortTask() const {
    return _doAbortTask;
}



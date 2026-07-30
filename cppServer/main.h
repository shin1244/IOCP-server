#pragma once

#include <thread>
#include <vector>
#include <iostream>
#include <unordered_map>
#include <stack>
#include <atomic>
#include <chrono>
#include <algorithm>
#include"RingBuffer.h"
#include"DoubleBuffer.h"
#include"EventQueue.h"
#include"Protocol.h"
#include"NetworkCore.h"
#include"World.h"

#define PSAPI_VERSION 1
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

//#define USE_EVENT_QUEUE
#define USE_POLLING

HANDLE g_iocp;   // IOCP �ڵ�

struct TickBenchmark {
    std::vector<long long> samples;
    long long totalTime = 0;
    int tickCount = 0;
    void reset() {
        samples.clear();
        samples.reserve(4096);
        totalTime = 0;
        tickCount = 0;
    }
};
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

HANDLE g_iocp;   // IOCP 핸들러

struct TickBenchmark {
    std::vector<long long> samples;      // 틱 전체(sim + flush)
    std::vector<long long> simSamples;   // 시뮬레이션만 (recv 드레인 + Update)
    std::vector<long long> flushSamples; // 네트워크 flush만 (FlushPending)
    long long totalTime = 0;
    int tickCount = 0;
    void reset() {
        samples.clear();
        simSamples.clear();
        flushSamples.clear();
        samples.reserve(4096);
        simSamples.reserve(4096);
        flushSamples.reserve(4096);
        totalTime = 0;
        tickCount = 0;
    }
};
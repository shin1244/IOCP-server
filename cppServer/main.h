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
#include"User.h"

#define PSAPI_VERSION 1
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

//#define USE_EVENT_QUEUE

HANDLE g_iocp;   // IOCP �ڵ�

struct TickBenchmark {
	long long totalTime = 0;
	long long maxTime = 0;
	long long minTime = LLONG_MAX;
	int tickCount = 0;
	long long totalConsumeTime = 0;
	int missCount = 0;                    // ��������(33ms)�� ���� ƽ ��
	std::vector<long long> samples;       // ƽ���� �ҿ� �ð�(ns) �� p99�� ���� ����

	void reset() {
		totalTime = 0;
		maxTime = 0;
		minTime = LLONG_MAX;
		tickCount = 0;
		totalConsumeTime = 0;
		missCount = 0;
		samples.clear();
	}

	// p�� [0,100] �������� ��鰪(ns) ��ȯ (nearest-rank)
	long long percentile(double p) {
		if (samples.empty()) return 0;
		std::vector<long long> s = samples;
		std::sort(s.begin(), s.end());
		size_t idx = (size_t)(p / 100.0 * (s.size() - 1));
		return s[idx];
	}
};
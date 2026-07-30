#pragma once
#include <winsock2.h>
#include <mutex>
#include <atomic>
#include <stack>
#include <iostream>
#include "RingBuffer.h"
#include "VecterBuffer.h"
#include "DoubleBuffer.h"
#include "EventQueue.h"
#include "Protocol.h"
#include "ObjectPool.h"
#pragma comment(lib, "ws2_32.lib")

// #define USE_EVENT_QUEUE
// #define USE_VECTER_BUFFER
#define USE_POLLING 

struct Session {
    SOCKET socket;
    int index;
    int roomId = -1;

#ifdef USE_VECTER_BUFFER
    VectorBuffer recvBuffer;
    VectorBuffer sendBuffer;
#else
    RingBuffer recvBuffer;
    RingBuffer sendBuffer;
#endif

    bool sendPending;
    bool sendDirty = false;   // 이번 틱에 보낼 데이터가 쌓였는지 (배치 송신용)
    std::mutex sendLock;

    OVERLAPPED recvOverlapped;
    OVERLAPPED sendOverlapped;
    WSABUF recvWsaBuf;
    WSABUF sendWsaBuf;

    std::atomic<bool> connected;
};

extern HANDLE g_iocp;
extern ObjectPool<Session, 5000> g_sessions;
extern std::stack<int> g_freeIndices;

#ifdef USE_EVENT_QUEUE
    extern EventQueue<RecvPacket> g_recvQueue;
#else
    extern DoubleBuffer<RecvPacket> g_recvQueue;
#endif

void workerThread();
void Accepter(SOCKET);
void postRecv(Session*);
void postSend(Session*, const char*, int);
void flushSend(Session*);
void CloseSession(Session*);
void OnRecvBytes(Session* s, int bytes);
void PollServer(SOCKET);

// 배치 송신: 틱 동안엔 queueSend 로 버퍼에만 쌓고, 틱 끝에 FlushPending 으로 세션당 1회 전송.
void queueSend(Session* s, const char* data, int len);
void FlushPending();
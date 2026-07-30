#include"NetworkCore.h"
#include"Protocol.h"
#include"ObjectPool.h"

ObjectPool<Session, 5000> g_sessions;

#ifdef USE_EVENT_QUEUE
EventQueue<RecvPacket> g_recvQueue;
#else
DoubleBuffer<RecvPacket> g_recvQueue;
#endif

#ifndef USE_POLLING

// 모든 종료를 한 곳으로 모은다. 여러 스레드가 동시에 들어와도
// exchange 로 첫 호출만 실제 정리하고 나머지는 즉시 반환(멱등).
void CloseSession(Session* session) {
    if (!session->connected.exchange(false)) return;   // 이미 닫힘

    closesocket(session->socket);
    session->socket = INVALID_SOCKET;

    RecvPacket rp;                     // 월드가 슬롯을 비우도록 Leave 통지
    rp.sessionIndex = session->index;
    rp.id = PacketId::Leave;
    g_recvQueue.Push(std::move(rp));
}

void workerThread() {
    while (true) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED* overlapped = nullptr;

        BOOL ok = GetQueuedCompletionStatus(
            g_iocp,
            &bytesTransferred,
            &completionKey,
            &overlapped,
            INFINITE
        );

        Session* session = (Session*)completionKey;

        if (!ok) {
            CloseSession(session);
            continue;
        }

        if (overlapped == &session->recvOverlapped) {
            if (bytesTransferred == 0) {
                CloseSession(session);
                continue;
            }

            OnRecvBytes(session, bytesTransferred);
            postRecv(session);
        }
        else if (overlapped == &session->sendOverlapped) {
            session->sendLock.lock();
            session->sendBuffer.OnRead(bytesTransferred);
            session->sendPending = false;
            flushSend(session);
            session->sendLock.unlock();
        }
    }
}

void Accepter(SOCKET s) {
    while (true)
    {
        sockaddr_in clientAddr = {};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(s, (sockaddr*)&clientAddr, &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            std::cout << "accept failed: " << WSAGetLastError() << "\n";
            continue;
        }


        int index = g_sessions.Alloc();
        if (index == -1) {
            closesocket(clientSocket);
            continue;
        }
        Session* session = &g_sessions[index];

        session->socket = clientSocket;
        session->index = index;
        session->roomId = -1;
        session->sendPending = false;
        session->connected = true;
        session->recvBuffer.Clear();
        session->sendBuffer.Clear();

        CreateIoCompletionPort((HANDLE)clientSocket, g_iocp, (ULONG_PTR)session, 0);

        RecvPacket rp;
        rp.sessionIndex = index;
        rp.id = PacketId::Join;
        g_recvQueue.Push(std::move(rp));

        postRecv(session);
    }
}

void postRecv(Session* session)
{
    int freeSize = session->recvBuffer.GetLinearFreeSize();
    if (freeSize <= 0) return;

    ZeroMemory(&session->recvOverlapped, sizeof(session->recvOverlapped));
    session->recvWsaBuf.buf = session->recvBuffer.GetWriteBuffer();
    session->recvWsaBuf.len = session->recvBuffer.GetLinearFreeSize();

    DWORD flags = 0;
    DWORD byteRecv = 0;

    int ret = WSARecv(
        session->socket,
        &session->recvWsaBuf,
        1,
        &byteRecv,
        &flags,
        &session->recvOverlapped,
        NULL
    );

    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            CloseSession(session);
        }
    }
}

void flushSend(Session* session) {
    if (!session->connected) return;
    if (session->sendPending) return;
    int used = session->sendBuffer.GetLinearUsedSize();
    if (used == 0) return;

    session->sendPending = true;
    session->sendWsaBuf.buf = session->sendBuffer.GetReadBuffer();
    session->sendWsaBuf.len = used;
    int ret = WSASend(
        session->socket,
        &session->sendWsaBuf,
        1,
        0,
        0,
        &session->sendOverlapped,
        NULL
    );

    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            session->sendPending = false;
            CloseSession(session);
        }
    }
}

void postSend(Session* session, const char* data, int len)
{
    session->sendBuffer.Write(data, len);
    flushSend(session);
}

#endif


void OnRecvBytes(Session* s, int bytes) {
    s->recvBuffer.OnWrite(bytes);
    while (true) {
        if (s->recvBuffer.GetUsedSize() < HEADER_SIZE) break;
        PacketHeader header;
        s->recvBuffer.Peek((char*)&header, HEADER_SIZE);
        if (s->recvBuffer.GetUsedSize() < header.size) break;
        char packet[4096];
        s->recvBuffer.Peek(packet, header.size);
        s->recvBuffer.OnRead(header.size);
        RecvPacket rp;
        rp.sessionIndex = s->index;
        rp.id = static_cast<PacketId>(header.id);
        rp.body.assign(packet + HEADER_SIZE, packet + header.size);
        g_recvQueue.Push(std::move(rp));
    }
}
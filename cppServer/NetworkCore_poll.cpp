#include "NetworkCore.h"
#include <vector>

#ifdef USE_POLLING

// fds[i] 와 conns[i] 는 같은 인덱스로 짝을 이룬다.
static std::vector<WSAPOLLFD> g_fds;
static std::vector<Session*>  g_conns;

// 20ms동안 요청을 모아서 관리하니 논블로킹 형식으로도 가능함
static void SetNonBlocking(SOCKET s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}

void flushSend(Session* s) {
    s->sendPending = true;
}

void postSend(Session* s, const char* data, int len) {
    s->sendLock.lock();
    s->sendBuffer.Write(data, len);
    s->sendPending = true;
    s->sendLock.unlock();
}

// 접속 요청이 더 이상 없을 때까지 accept 를 반복한다.
static void PollAccept(SOCKET listenSocket) {
    while (true) {
        sockaddr_in addr{};
        int len = sizeof(addr);
        SOCKET c = accept(listenSocket, (sockaddr*)&addr, &len);
        if (c == INVALID_SOCKET) break;   // 요청 없음

        // IOCP와 동일

        int index = g_sessions.Alloc();
        if (index == -1) { closesocket(c); continue; }

        Session* s = &g_sessions[index];
        s->socket = c;
        s->index = index;
        s->roomId = -1;
        s->sendPending = false;
        s->connected = true;
        s->recvBuffer.Clear();
        s->sendBuffer.Clear();
        SetNonBlocking(c);

        RecvPacket rp;
        rp.sessionIndex = index;
        rp.id = PacketId::Join;
        g_recvQueue.Push(std::move(rp));

        g_fds.push_back({ c, POLLRDNORM, 0 });
        g_conns.push_back(s);
    }
}

// 읽기 가능 → recv 로 링버퍼를 채우고 공용 파싱(OnRecvBytes)에 넘긴다.
static bool PollRecv(Session* s) {
    if (s->recvBuffer.GetFreeSize() <= 0) return false;
    int free = s->recvBuffer.GetLinearFreeSize();
    if (free <= 0) return true;

    int n = recv(s->socket, s->recvBuffer.GetWriteBuffer(), free, 0);
    if (n > 0) { OnRecvBytes(s, n); return true; }        
    if (n == 0) return false;                             
    return WSAGetLastError() == WSAEWOULDBLOCK;           
}

// 쓰기 가능 → sendBuffer 에 쌓인 걸 논블로킹 send 로 흘려보낸다.
static void PollSend(Session* s) {
    s->sendLock.lock();
    while (true) {
        int used = s->sendBuffer.GetLinearUsedSize();
        if (used <= 0) { s->sendPending = false; break; } 
        int n = send(s->socket, s->sendBuffer.GetReadBuffer(), used, 0);
        if (n > 0) { s->sendBuffer.OnRead(n); continue; }
        break;   
    }
    s->sendLock.unlock();
}

// 네트워크는 바로 종료, 퇴장 처리는 다음 틱
static void Drop(size_t i) {
    Session* s = g_conns[i];

    RecvPacket rp;
    rp.sessionIndex = s->index;
    rp.id = PacketId::Leave;
    g_recvQueue.Push(std::move(rp));

    closesocket(s->socket);
    g_fds[i].fd = INVALID_SOCKET;
}

static void Compact() {
    size_t w = 0;
    for (size_t r = 0; r < g_fds.size(); ++r) {
        if (g_fds[r].fd == INVALID_SOCKET) continue;
        g_fds[w] = g_fds[r];
        g_conns[w] = g_conns[r];
        ++w;
    }
    g_fds.resize(w);
    g_conns.resize(w);
}

void PollServer(SOCKET listenSocket) {
    SetNonBlocking(listenSocket);
    g_fds.push_back({ listenSocket, POLLRDNORM, 0 });
    g_conns.push_back(nullptr);

    while (true) {
        // 매 루프: 보낼 게 있는 세션에만 쓰기 ON
        for (size_t i = 1; i < g_fds.size(); ++i)
            g_fds[i].events = POLLRDNORM | (g_conns[i]->sendPending ? POLLWRNORM : 0);

        int ready = WSAPoll(g_fds.data(), (ULONG)g_fds.size(), 20);
        if (ready <= 0) continue; 

        bool anyDrop = false;
        for (size_t i = 0; i < g_fds.size(); ++i) {
            short re = g_fds[i].revents;
            if (re == 0) continue;

            if (i == 0) { PollAccept(listenSocket); continue; }   // listen 소켓

            Session* s = g_conns[i];
            if (re & (POLLHUP | POLLERR | POLLNVAL)) { Drop(i); anyDrop = true; continue; } // 접속 종료, 에러 발생 

            if (re & POLLRDNORM) { if (!PollRecv(s)) { Drop(i); anyDrop = true; continue; } }
            if (re & POLLWRNORM) { PollSend(s); }
        }
        if (anyDrop) Compact();
    }
}

#endif // USE_POLLING

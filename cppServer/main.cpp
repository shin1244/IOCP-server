#include"main.h"

int PickRoom(std::vector<World>& worlds) {
    for (int i = 0; i < (int)worlds.size(); ++i)
        if (worlds[i].CanJoin())
            return i;
    return -1;
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    std::cout << "Winsock ready\n";

    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (g_iocp == NULL) {
        std::cout << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
        return 1;
    }
    std::cout << "IOCP created\n";

    unsigned int n = std::thread::hardware_concurrency() - 1;
    std::cout << "spawning " << n << " worker threads\n";

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::cout << "socket failed: " << WSAGetLastError() << "\n";
        return 1;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(5050);
    if (bind(listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cout << "bind failed: " << WSAGetLastError() << "\n";
        return 1;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cout << "listen failed: " << WSAGetLastError() << "\n";
        return 1;
    }
    std::cout << "listening on port 5050...\n";

    // IOCP 워커 쓰레드(코어 수)와 접속 쓰레드 생성
    for (unsigned int i = 0; i < n; i++)
        std::thread(workerThread).detach();
    std::thread (Accepter, listenSocket).detach();

    constexpr int   TICK_MS = 33;    
    constexpr float TICK_DT = TICK_MS / 1000.0f;

    std::vector<World> worlds(4);
    for (auto& w : worlds) w.Init();

    std::vector<RecvPacket> buffer;
    TickBenchmark bench; // 벤치마크
    auto lastReportTime = std::chrono::steady_clock::now(); // 마지막 결과 출력 시간

    while (true) {
        auto tickStart = std::chrono::steady_clock::now();

#ifdef USE_EVENT_QUEUE
        RecvPacket p;
        while (g_recvQueue.Pop(p)) {
            Session& s = g_sessions[p.sessionIndex];
            if (p.id == PacketId::Join) s.roomId = PickRoom(worlds);
            if (s.roomId >= 0) worlds[s.roomId].HandlePacket(p);
        }
#else
        buffer.clear();
        g_recvQueue.Swap(buffer);
        for (auto& p : buffer) {
            Session& s = g_sessions[p.sessionIndex];
            if (p.id == PacketId::Join) s.roomId = PickRoom(worlds);
            if (s.roomId >= 0) worlds[s.roomId].HandlePacket(p);
        }
#endif

        for (auto& w : worlds) {
            w.Update(TICK_DT);
        }

        auto tickEnd = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(tickEnd - tickStart).count();
        bench.samples.push_back(duration);
        bench.tickCount++;
        bench.totalTime += duration;

        // --- 60초마다 벤치마크 결과 출력 ---
        if (tickEnd - lastReportTime >= std::chrono::seconds(60)) {
            auto& s = bench.samples;
            std::sort(s.begin(), s.end());

            auto pct = [&](double p) {
                // size*p 인덱스, 경계 방어
                size_t idx = (size_t)(s.size() * p);
                if (idx >= s.size()) idx = s.size() - 1;
                return s[idx] / 1'000'000.0;
                };

            double avgMs = (bench.totalTime / (double)bench.tickCount) / 1'000'000.0;
            double p50 = pct(0.50);
            double p99 = pct(0.99);
            double p999 = pct(0.999);
            double maxMs = s.back() / 1'000'000.0;

            std::cout << std::format(
                "\n========================================\n"
                " [ Tick Benchmark Report (Last 60s) ]   \n"
                "----------------------------------------\n"
                " Total Ticks : {:>10}\n"
                " P50         : {:>10.3f} ms\n"
                " P99         : {:>10.3f} ms\n"
                " P99.9       : {:>10.3f} ms\n"
                " Max Time    : {:>10.3f} ms\n"
                "========================================\n\n",
                bench.tickCount, avgMs, p50, p99, p999, maxMs
            );
            bench.reset();
            lastReportTime = tickEnd;
        }

        std::this_thread::sleep_until(tickStart + std::chrono::milliseconds(TICK_MS));
    }
    
    WSACleanup();
    return 0;
}
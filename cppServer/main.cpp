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

#ifndef USE_POLLING
    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (g_iocp == NULL) {
        std::cout << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
        return 1;
    }
    std::cout << "IOCP created\n";

    unsigned int n = std::thread::hardware_concurrency();
    std::cout << "spawning " << n << " worker threads\n";

    // IOCP 워커 쓰레드(코어 수)와 접속 쓰레드 생성
    for (unsigned int i = 0; i < n; i++)
        std::thread(workerThread).detach();
    std::thread (Accepter, listenSocket).detach();
#else
    std::cout << "polling server\n";
    std::thread(PollServer, listenSocket).detach();
#endif


    constexpr int   TICK_MS = 33;    
    constexpr float TICK_DT = TICK_MS / 1000.0f;

    std::vector<World> worlds(1);
    for (auto& w : worlds) w.Init();

    std::vector<RecvPacket> buffer;
    TickBenchmark bench; // 벤치마크
    auto lastReportTime = std::chrono::steady_clock::now(); // 마지막 결과 출력 시간
    auto lastTickLog = std::chrono::steady_clock::now();    // 3초마다 간략 벤치마크
    long long totalTicks = 0;                               // 시작 이후 누적 틱 수
    std::vector<long long> win3;                            // 3초 창: 틱 전체 시간(ns)

    while (true) {
        auto tickStart = std::chrono::steady_clock::now();

#ifdef USE_EVENT_QUEUE
        RecvPacket p;
        while (g_recvQueue.Pop(p)) {
            Session& s = g_sessions[p.sessionIndex];
            if (p.id == PacketId::Join) s.roomId = PickRoom(worlds);
            if (s.roomId >= 0) worlds[s.roomId].HandlePacket(p);
            if (p.id == PacketId::Leave) g_sessions.Free(p.sessionIndex);
        }
#else
        buffer.clear();
        g_recvQueue.Swap(buffer);
        for (auto& p : buffer) {
            Session& s = g_sessions[p.sessionIndex];
            if (p.id == PacketId::Join) s.roomId = PickRoom(worlds);
            if (s.roomId >= 0) worlds[s.roomId].HandlePacket(p);
            if (p.id == PacketId::Leave) g_sessions.Free(p.sessionIndex);
        }
#endif

        for (auto& w : worlds) {
            w.Update(TICK_DT);
        }

        auto simEnd = std::chrono::steady_clock::now();

        FlushPending();   // 틱 동안 queueSend 로 쌓인 걸 세션당 1회 실제 전송

        auto tickEnd = std::chrono::steady_clock::now();

        auto simDur   = std::chrono::duration_cast<std::chrono::nanoseconds>(simEnd - tickStart).count();
        auto flushDur = std::chrono::duration_cast<std::chrono::nanoseconds>(tickEnd - simEnd).count();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(tickEnd - tickStart).count();
        bench.samples.push_back(duration);
        bench.simSamples.push_back(simDur);
        bench.flushSamples.push_back(flushDur);
        bench.tickCount++;
        bench.totalTime += duration;
        totalTicks++;
        win3.push_back(duration);

        // --- 3초마다 간략 벤치마크 (틱 전체 시간) ---
        if (tickEnd - lastTickLog >= std::chrono::seconds(3)) {
            std::sort(win3.begin(), win3.end());
            long long sum = 0;
            for (long long x : win3) sum += x;
            double avgMs = sum / (double)win3.size() / 1'000'000.0;
            size_t p99i = (size_t)(win3.size() * 0.99);
            if (p99i >= win3.size()) p99i = win3.size() - 1;
            double p99Ms = win3[p99i] / 1'000'000.0;
            double maxMs = win3.back() / 1'000'000.0;
            std::cout << std::format(
                "[3s] tick {:>8} | ticks {:>4} | avg {:>7.3f} | p99 {:>7.3f} | max {:>7.3f}  (ms)\n",
                totalTicks, win3.size(), avgMs, p99Ms, maxMs);
            win3.clear();
            lastTickLog = tickEnd;
        }

        // --- 60초마다 벤치마크 결과 출력 ---
        if (tickEnd - lastReportTime >= std::chrono::seconds(60)) {
            auto report = [](const char* label, std::vector<long long>& v) {
                if (v.empty()) return;
                std::sort(v.begin(), v.end());
                auto pct = [&](double p) {
                    size_t idx = (size_t)(v.size() * p);
                    if (idx >= v.size()) idx = v.size() - 1;
                    return v[idx] / 1'000'000.0;
                    };
                long long sum = 0;
                for (long long x : v) sum += x;
                double avgMs = (sum / (double)v.size()) / 1'000'000.0;

                std::cout << std::format(
                    " {:<6} | avg {:>8.3f} | p50 {:>8.3f} | p99 {:>8.3f} | p99.9 {:>8.3f} | max {:>8.3f}\n",
                    label, avgMs, pct(0.50), pct(0.99), pct(0.999), v.back() / 1'000'000.0
                );
                };

            std::cout << std::format(
                "\n=================================================================================\n"
                " [ Tick Benchmark Report (Last 60s) ]   Total Ticks : {}   (unit: ms)\n"
                "---------------------------------------------------------------------------------\n",
                bench.tickCount
            );
            report("total", bench.samples);        // 틱 전체
            report("sim",   bench.simSamples);      // 시뮬레이션만 (recv 드레인 + Update)
            report("flush", bench.flushSamples);    // 네트워크 flush만 (FlushPending)
            std::cout << "=================================================================================\n\n";

            bench.reset();
            lastReportTime = tickEnd;
        }

        std::this_thread::sleep_until(tickStart + std::chrono::milliseconds(TICK_MS));
    }
    
    WSACleanup();
    return 0;
}
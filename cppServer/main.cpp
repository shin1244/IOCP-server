#include"main.h"

int PickRoom(std::vector<World>& worlds) {
    for (int i = 0; i < (int)worlds.size(); ++i)
        if (worlds[i].CanJoin())
            return i;
    return -1;
}

void RoutePacket(RecvPacket& p, std::vector<World>& worlds, UserManager& users) {
    Session& s = g_sessions[p.sessionIndex];

    if (p.id == PacketId::Connect) {
        users.OnSessionOpened(p.sessionIndex);
        return;
    }

    if (p.id == PacketId::Leave) {
        users.OnSessionClosed(p.sessionIndex);
        if (s.roomId >= 0) worlds[s.roomId].HandlePacket(p);
        s.roomId = -1;
        return;
    }

    if (p.id == PacketId::LoginReq || p.id == PacketId::SignupReq) {
        users.HandlePacket(p);
        return;
    }

    // Everything past this point requires a logged in session.
    if (!users.IsAuthenticated(p.sessionIndex)) return;

    if (p.id == PacketId::Join) s.roomId = PickRoom(worlds);
    if (s.roomId >= 0) worlds[s.roomId].HandlePacket(p);
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

    //unsigned int n = std::thread::hardware_concurrency() - 1;
    unsigned int n = 32;
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

    // IOCP ��Ŀ ������(�ھ� - 1)�� ���� ������ ����
    for (unsigned int i = 0; i < n; i++)
        std::thread(workerThread).detach();
    std::thread (Accepter, listenSocket).detach();

    constexpr int   TICK_MS = 33;    
    constexpr float TICK_DT = TICK_MS / 1000.0f;

    std::vector<World> worlds(1);
    for (auto& w : worlds) w.Init();

    UserManager users;

    // -- ���� ���� ���� --

    std::vector<RecvPacket> buffer;
    TickBenchmark bench; // 벤치마크
    auto lastReportTime = std::chrono::steady_clock::now(); // 마지막 결과 출력 시간
    bool measuring = false;
    auto benchStart = std::chrono::steady_clock::now();

    while (true) {
        auto tickStart = std::chrono::steady_clock::now();

        // 이벤트 큐 방식과 더블 버퍼 방식 비교
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
        auto ConsumeEnd = std::chrono::steady_clock::now();
        auto ConsumeDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(ConsumeEnd - tickStart).count();
        bench.totalConsumeTime += ConsumeDuration;

        for (auto& w : worlds) {
            w.Update(TICK_DT);
        }

        if (!measuring && worlds[0].IsRunning()) {
            measuring = true;
            bench.reset();
            benchStart = std::chrono::steady_clock::now(); // ★ 120초 타이머 시작 시점 잡기
            lastReportTime = benchStart;
            std::cout << "[Benchmark] 120초간 측정을 시작합니다...\n";
        }

        auto tickEnd = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(tickEnd - tickStart).count();
        
        if (measuring) {
            bench.tickCount++;
            bench.totalTime += duration;
            if (duration > bench.maxTime) bench.maxTime = duration;
            if (bench.minTime == 0 || duration < bench.minTime) bench.minTime = duration; // minTime이 0일 때 방어문 추가
        }

        if (tickEnd - lastReportTime >= std::chrono::seconds(60)) {
            auto& s = bench.samples;
            std::sort(s.begin(), s.end());

            auto pct = [&](const double p) {
                size_t idx = static_cast<size_t>(s.size() * p);
                if (idx >= s.size()) idx = s.size() - 1;
                return s[idx] / 1'000'000.0;
                };

            std::cout << "[Live Monitor] 진행중... 현재 평균 틱: " << avgMs << " ms | 최고 틱: " << maxMs << " ms | 메모리: " << currentMemMb << " MB\n";
            lastReportTime = tickStart; // 다음 3초 체크용
        }

        if (measuring && tickStart - benchStart >= std::chrono::seconds(120)) {
            double avgMs = (bench.totalTime / (double)bench.tickCount) / 1'000'000.0;
            double maxMs = bench.maxTime / 1'000'000.0;
            double minMs = bench.minTime / 1'000'000.0;
            double avgCMs = (bench.totalConsumeTime / (double)bench.tickCount) / 1'000'000.0;

            std::cout << std::format(
                "\n========================================\n"
                " [ Tick Benchmark Report (Last 60s) ]   \n"
                "----------------------------------------\n"
                " Total Ticks : {:>10}\n"
                " Avg Time    : {:>10.3f} ms\n"
                " P50         : {:>10.3f} ms\n"
                " P99         : {:>10.3f} ms\n"
                " P99.9       : {:>10.3f} ms\n"
                " Max Time    : {:>10.3f} ms\n"
                "========================================\n\n",
                bench.tickCount, avgMs, p50, p99, p999, maxMs
            );

            std::cout << "\n======================================================\n"
                << " [FINAL BENCHMARK RESULT (120 Seconds)] \n"
                << " - Total Ticks      : " << bench.tickCount << " ticks\n"
                << " - Avg Tick Time    : " << avgMs << " ms\n"
                << " - Max Tick Time    : " << maxMs << " ms\n"
                << " - Min Tick Time    : " << minMs << " ms\n"
                << " - Avg Consume Time   : " << avgCMs << " ms\n"
                << " - Memory Usage     : " << currentMemMb << " MB (Peak: " << peakMemMb << " MB)\n"
                << "======================================================\n" << std::endl;
            measuring = false;
        }

        std::this_thread::sleep_until(tickStart + std::chrono::milliseconds(TICK_MS));
    }
    
    WSACleanup();
    return 0;
}
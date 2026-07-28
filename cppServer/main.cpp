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
    TickBenchmark bench; // ��ġ��ũ
    auto lastReportTime = std::chrono::steady_clock::now(); // ������ ��� ��� �ð�
    bool measuring = false;
    auto benchStart = std::chrono::steady_clock::now();

    while (true) {
        auto tickStart = std::chrono::steady_clock::now();

        // �̺�Ʈ ť ��İ� ���� ���� ��� ��
        #ifdef USE_EVENT_QUEUE
                RecvPacket p;
                while (g_recvQueue.Pop(p)) {
                    RoutePacket(p, worlds, users);
                }
        #else
                buffer.clear();
                g_recvQueue.Swap(buffer); 
                for (auto& p : buffer) {
                    RoutePacket(p, worlds, users);
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
            benchStart = std::chrono::steady_clock::now(); // �� 120�� Ÿ�̸� ���� ���� ���
            lastReportTime = benchStart;
            std::cout << "[Benchmark] 120�ʰ� ������ �����մϴ�...\n";
        }

        auto tickEnd = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(tickEnd - tickStart).count();
        
        if (measuring) {
            bench.tickCount++;
            bench.totalTime += duration;
            bench.samples.push_back(duration);
            if (duration > (long long)TICK_MS * 1'000'000) bench.missCount++;
            if (duration > bench.maxTime) bench.maxTime = duration;
            if (bench.minTime == 0 || duration < bench.minTime) bench.minTime = duration; // minTime�� 0�� �� �� �߰�
        }

        if (measuring && tickStart - lastReportTime >= std::chrono::seconds(3)) {
            double avgMs = (bench.totalTime / (double)bench.tickCount) / 1'000'000.0;
            double maxMs = bench.maxTime / 1'000'000.0;

            PROCESS_MEMORY_COUNTERS pmc;
            double currentMemMb = 0.0;
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                currentMemMb = pmc.WorkingSetSize / (1024 * 1024);
            }

            std::cout << "[Live Monitor] ������... ���� ��� ƽ: " << avgMs << " ms | �ְ� ƽ: " << maxMs << " ms | �޸�: " << currentMemMb << " MB\n";
            lastReportTime = tickStart; // ���� 3�� üũ��
        }

        if (measuring && tickStart - benchStart >= std::chrono::seconds(120)) {
            double avgMs = (bench.totalTime / (double)bench.tickCount) / 1'000'000.0;
            double maxMs = bench.maxTime / 1'000'000.0;
            double minMs = bench.minTime / 1'000'000.0;
            double avgCMs = (bench.totalConsumeTime / (double)bench.tickCount) / 1'000'000.0;
            double p50Ms = bench.percentile(50) / 1'000'000.0;
            double p95Ms = bench.percentile(95) / 1'000'000.0;
            double p99Ms = bench.percentile(99) / 1'000'000.0;
            double missRate = bench.tickCount ? (bench.missCount * 100.0 / bench.tickCount) : 0.0;

            PROCESS_MEMORY_COUNTERS pmc;
            double currentMemMb = 0.0;
            double peakMemMb = 0.0;
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                currentMemMb = pmc.WorkingSetSize / (1024 * 1024);
                peakMemMb = pmc.PeakWorkingSetSize / (1024 * 1024);
            }

            std::cout << "\n======================================================\n"
                << " [FINAL BENCHMARK RESULT (120 Seconds)] \n"
                << " - Total Ticks      : " << bench.tickCount << " ticks\n"
                << " - Avg Tick Time    : " << avgMs << " ms\n"
                << " - p50 Tick Time    : " << p50Ms << " ms\n"
                << " - p95 Tick Time    : " << p95Ms << " ms\n"
                << " - p99 Tick Time    : " << p99Ms << " ms\n"
                << " - Max Tick Time    : " << maxMs << " ms\n"
                << " - Min Tick Time    : " << minMs << " ms\n"
                << " - Deadline Miss    : " << bench.missCount << " ticks (" << missRate << " % over " << TICK_MS << "ms)\n"
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
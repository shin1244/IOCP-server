<h1 align="center">IOCP 기반 실시간 탑다운 슈팅 배틀로얄 게임 서버</h1>

<p align="center">
  Windows <strong>IOCP</strong>로 구현한 C++ 멀티스레드 게임 서버 —<br/>
  최대 500명이 한 맵에서 이동·사격하며 <strong>마지막 1인까지 생존</strong>하는 실시간 배틀로얄을 돌립니다.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white" alt="C++17"/>
  <img src="https://img.shields.io/badge/Winsock2-IOCP-0078D6?logo=windows&logoColor=white" alt="IOCP"/>
  <img src="https://img.shields.io/badge/Client%20%26%20Bot-Go-00ADD8?logo=go&logoColor=white" alt="Go"/>
  <img src="https://img.shields.io/badge/Tick-30Hz%20(33ms)-success" alt="30Hz"/>
</p>

<!-- ▼ 데모 GIF/스크린샷을 여기에 넣기 (스탯창·플레이 화면). 예: <p align="center"><img src="portfolio/demo.gif" width="720"/></p> -->
<p align="center"><em>🎬 데모 GIF 자리 — 플레이 화면 / 스탯창 캡처</em></p>

---

> **이 프로젝트는 "동작하는 IOCP 서버"가 목적이 아닙니다.**
> 실시간 게임 서버에서 마주치는 **핵심 설계 선택 3가지를 각각 두 방식으로 직접 구현하고, 부하를 걸어 실측 비교**하며 *"왜 이 선택인가"* 를 검증한 기록입니다.

| | 무엇을 | 한 줄 결론 |
|---|---|---|
| **I/O 모델** | IOCP vs WSAPoll | 500명 +7% → 5000명 +12%, **부하가 커질수록 격차 확대** |
| **수신 버퍼** | 링 버퍼 vs 가변 버퍼 | 속도가 아니라 **최악의 상황에서 무너지는 방식**이 다름 (DoS 내성) |
| **스레드 전달 큐** | 이벤트 큐 vs 더블 스왑 | 상위 지표에 가려진 락 경합을 **측정 지점을 바꿔** 2~3배 차이로 분리 |

---

## 아키텍처

```
       네트워크 계층 (워커 = CPU 코어 수)              로직 계층 (단일 스레드 · 33ms 틱)
  ┌───────────────────────────────────────┐   ┌────────────────────────────────────────┐
  WSARecv ─▶ 세션 수신버퍼 ─▶ 프레임 파싱 ─▶ 전달 큐 ─▶ 큐 소비 ─▶ World.Update ─▶ 배치 송신
             [실험2 링/가변]              [실험1 더블스왑/이벤트큐]              (틱당 세션 1회)
  └───────────────────────────────────────┘   └────────────────────────────────────────┘
        ▲ [실험0] 이 계층 자체를 IOCP vs WSAPoll 두 방식으로 구현
```

- 워커 스레드(**= CPU 코어 수**)가 패킷을 받아 프레임 단위로 파싱한 뒤 전달 큐에 넣고, **단일 로직 스레드**가 33ms(≈30Hz)마다 큐를 비워 처리합니다. 게임 상태를 로직 스레드 혼자 소유하므로 플레이어·총알·맵에 **락이 필요 없습니다** — 락은 큐 push/swap과 세션 송신 버퍼 같은 경계 지점에만 둡니다.
- 방(World) **5개** · 방당 최대 **100명** · 총알 512/방 → 동시 **최대 500명** 부하 (봇 부하는 5000까지 확장 테스트).
- **서버 권위(server-authoritative)**: 클라이언트는 좌표가 아니라 **입력 키만** 보냅니다. 이동 가능 여부·사격 쿨타임·충돌·시야(LOS)를 전부 서버가 판정해 최종 상태를 결정하고, 시야+반경 안에 든 대상만 골라 전송합니다.
- **배틀로얄 규칙**: 100×100 격자 맵에서 이동·사격, 총알로 벽 파괴, 피격 시 관전으로 전환, **마지막 1인 생존 시 승자 확정**.

---

## 핵심: 3가지 설계 선택을 직접 구현하고 실측 비교

> 각 실험은 **결론 한 줄 → 왜 → (접힌) 코드** 순서입니다. 빠르게 훑을 사람은 결론만, 깊게 볼 사람은 펼쳐서 코드까지.

### 실험 1. I/O 모델 — IOCP vs WSAPoll

> **한 줄 결론:** "IOCP가 빠르다"는 뻔한 결론 대신 **몇 명부터·얼마나 갈라지는지**를 실측 — 500명에선 +7%로 미미하지만, 부하가 커질수록 격차가 벌어져 5000명에선 +12%(18.6ms).

<p align="center"><img src="portfolio/bench_iocp_vs_poll.svg" width="720" alt="동시접속 수에 따른 게임 틱 시간: IOCP vs WSAPoll"/></p>

- **왜 둘 다 만들었나:** "게임 서버는 IOCP 써라"는 말은 많이 듣지만, *실제로 얼마나* 차이 나는지 직접 확인하고 싶어 단일 스레드 `WSAPoll` 서버도 구현해 같은 부하로 붙였습니다.
- **공정한 비교를 위해:** 송신을 양쪽 모두 **배치 방식(틱당 세션 1회)** 으로 동일화한 뒤, 순수 I/O 모델 차이만 남기고 60초 평균 틱을 측정했습니다.
- **배운 점:** 저부하에선 커널 완료 통지(IOCP)의 이점이 오버헤드에 가려 크지 않다. IOCP의 진짜 가치는 절대 속도가 아니라 **동시성이 커질 때의 확장성**. (두 방식 모두 33ms 예산은 1000명 부근에서 이미 초과 — 이 서버의 실제 한계도 함께 드러남.)

<details><summary>토글 & 코드 (<code>USE_POLLING</code>)</summary>

```cpp
// NetworkCore.h — 매크로 하나로 네트워크 계층 전체를 교체
// #define USE_POLLING   // 정의 시 WSAPoll 단일 스레드, 미정의 시 IOCP 워커 풀

// main.cpp
#ifndef USE_POLLING
    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    unsigned int n = std::thread::hardware_concurrency();   // 워커 = 코어 수
    for (unsigned int i = 0; i < n; i++) std::thread(workerThread).detach();
    std::thread(Accepter, listenSocket).detach();
#else
    std::thread(PollServer, listenSocket).detach();          // 단일 스레드 폴링
#endif
```
</details>

### 실험 2. 세션 수신 버퍼 — 링 버퍼 vs 가변 버퍼

> **한 줄 결론:** 정상 부하에선 거의 동일. 진짜 차이는 속도가 아니라 **최악의 상황에서 무너지는 방식** — 링은 상한 안에서 세션을 격리하고, 가변은 세션마다 메모리가 무한히 늘어 **DoS/OOM**에 노출됩니다.

- **정상 부하:** 소비가 생산을 따라잡아 링/가변 차이가 거의 없음.
- **악성 상황 재현:** 부하 봇에 **악성 모드**(완성되지 않는 거대 패킷을 흘리는)를 추가해 backpressure를 강제 → 링 버퍼는 고정 상한 안에서 해당 세션만 끊어 격리, 가변 버퍼는 세션마다 메모리가 계속 증가.
- **결론:** "어느 게 빠른가"가 아니라 **"최악에 어떻게 무너지는가"** 가 설계 기준. 링은 오버플로 정책(backpressure)이, 가변은 상한(cap)이 반드시 필요.
- 이 과정에서 링 버퍼의 full/empty 오인 버그와 backpressure 결함을 찾아 수정했습니다.

<details><summary>토글 (<code>USE_VECTER_BUFFER</code>)</summary>

```cpp
// NetworkCore.h
// #define USE_VECTER_BUFFER   // 정의 시 가변(vector) 버퍼, 미정의 시 링 버퍼
struct Session {
#ifdef USE_VECTER_BUFFER
    VectorBuffer recvBuffer, sendBuffer;   // 필요 시 grow — 상한 없음
#else
    RingBuffer   recvBuffer, sendBuffer;   // 고정 크기 — 메모리 상한 보장
#endif
};
```
</details>

### 실험 3. 세션 → 로직 스레드 전달 큐 — 이벤트 큐 vs 더블 스왑

> **한 줄 결론:** 이점이 전체 틱 시간에 묻혀 안 보였다가, **측정 지점을 바꾸자** 드러남 — 소비 측 락 획득을 *메시지당 → 프레임당* 으로 줄여 최악 틱을 크게 낮춤.

- **이벤트 큐:** 단일 큐 + 뮤텍스, push·pop **매 건마다** 락.
- **더블 스왑:** 버퍼 2개, 워커는 push 시에만 락 / 로직은 **틱당 1회만 swap** → 소비 중 워커와 경합 없음.
- **핵심 교훈:** 락 경합의 이점은 게임 로직 연산에 가려 상위 지표(전체 틱)에서는 안 보인다. **어디를 재느냐가 결과를 만든다.**

<details><summary>토글 & 스왑 아이디어 (<code>USE_EVENT_QUEUE</code>)</summary>

```cpp
// NetworkCore.h
// #define USE_EVENT_QUEUE   // 정의 시 이벤트 큐(매 건 락), 미정의 시 더블 스왑
#ifdef USE_EVENT_QUEUE
    extern EventQueue<RecvPacket>  g_recvQueue;   // push/pop 매번 mutex
#else
    extern DoubleBuffer<RecvPacket> g_recvQueue;  // 로직은 틱당 1회 swap 후 락 없이 소비
#endif
```
</details>

---

## 측정 방법론 — 평균이 아니라 마감선으로

실시간 서버에서 **33ms는 평균 목표가 아니라 매 틱의 마감선(30 FPS)** 입니다. 처음엔 "평균 틱 33ms 이하면 OK"로 판단했지만, 평균은 튀는 최악 틱(스파이크)을 숨깁니다. 그래서 지표를 바꿨습니다:

- 매 틱을 **틱 전체 / 시뮬레이션만 / 네트워크 flush만** 세 구간으로 나눠 기록
- 60초마다 정렬해 **avg · p50 · p99 · p99.9 · max** 를 뽑고 **33ms 예산선**과 비교

<details><summary>벤치 리포트 코드 (<code>main.cpp</code>)</summary>

```cpp
report("total", bench.samples);      // 틱 전체 (sim + flush)
report("sim",   bench.simSamples);   // 시뮬레이션만 (recv 드레인 + Update)
report("flush", bench.flushSamples); // 네트워크 flush만 (FlushPending)
// → " total | avg .. | p50 .. | p99 .. | p99.9 .. | max .. "
```
</details>

---

## 빌드 & 실행

```bash
# 서버: cppServer.sln (Visual Studio / MSVC, x64) 빌드 후 실행 → 5050 포트 리슨
```

```bash
# 부하 봇 (Go) — client 폴더
go run ./bot -n 200                # 정상 봇 200개
go run ./bot -n 200 -mal 0.2       # 20% 악성 봇 (실험 2)
```

```bash
# 렌더링 클라이언트 (시야/전장의 안개 시각화)
go run .
```

**설계 토글** — 매크로 하나로 각 실험의 두 방식을 전환 (모두 `NetworkCore.h`)

| 실험 | 매크로 | 정의 시 → 미정의 시 |
|---|---|---|
| 1. I/O 모델 | `USE_POLLING` | WSAPoll 단일 스레드 → **IOCP 워커 풀** |
| 2. 수신 버퍼 | `USE_VECTER_BUFFER` | 가변 버퍼 → **링 버퍼** |
| 3. 전달 큐 | `USE_EVENT_QUEUE` | 이벤트 큐 → **더블 스왑** |

---

## 디렉터리 구조

```
cppServer/
├─ cppServer/                        # 서버 (C++17)
│  ├─ main.cpp / main.h              # 진입점, 33ms 로직 루프, 틱 벤치마크
│  ├─ NetworkCore.*                  # IOCP 워커/세션/수신·배치송신 (+ 설계 토글)
│  ├─ RingBuffer.* / VecterBuffer.*  # [실험2] 수신 버퍼
│  ├─ DoubleBuffer.h / EventQueue.h  # [실험3] 전달 큐
│  ├─ World.* / World_Net.cpp        # 게임 로직 / 패킷 핸들러
│  ├─ Player.* / Bullet.* / Map.*    # 게임 오브젝트
│  ├─ SpatialGrid.*                  # 공간 분할(AOI broad-phase)
│  └─ Protocol.h / ObjectPool.h      # 바이너리 프로토콜 / 오브젝트 풀
├─ client/                           # 클라이언트 (Go)
│  ├─ main.go                        # 렌더링 클라 (ebiten)
│  └─ bot/main.go                    # 헤드리스 부하 봇 (+ 악성 모드)
└─ portfolio/                        # 벤치마크 그래프 등 포트폴리오 자료
```

<h1 align="center">IOCP 기반 실시간 탑다운 슈팅 배틀로얄 게임 서버</h1>

<p align="center">
  Windows <strong>IOCP</strong>로 구현한 C++ 멀티스레드 게임 서버<br/>
  최대 방 갯수를 가변적으로 늘리고 줄일 수 있으며<br/>
  하나의 방에 최대 100명이 동시 접속해 이동·사격하며 <strong>마지막 1인까지 생존</strong>하는 실시간 배틀로얄 게임입니다.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Winsock2-IOCP-0078D6?logo=windows&logoColor=white" alt="IOCP"/>
  <img src="https://img.shields.io/badge/Client%20%26%20Bot-Go-00ADD8?logo=go&logoColor=white" alt="Go"/>
</p>

<!-- 최상단 스크린샷 2개 (Flexbox 형태) -->
<div align="center" style="display: flex; justify-content: center; align-items: center; gap: 16px; flex-wrap: nowrap; width: 100%; max-width: 880px; margin: 0 auto;">
  <img src="./images/client-play.png" style="width: 50%; max-width: 420px; height: auto; object-fit: contain;" alt="플레이 화면" />
  <img src="./images/server-monitor.png" style="width: 50%; max-width: 420px; height: auto; object-fit: contain;" alt="서버 통계" />
</div>
<p align="center">
  <sub><strong>▲ (좌) Go 언어 기반 그래픽 클라이언트 플레이 화면</strong> &nbsp;|&nbsp; <strong>(우) 서버 실시간 Tick/I/O 벤치마크 모니터링 콘솔 ▲</strong></sub>
</p>

---

> 게임 콘텐츠 구현보다 고성능 네트워크 라이브러리와 서버 코어 구조 설계에 집중한 프로젝트입니다.<br/>
> 실제 게임 서버에서 중요한 3가지 핵심 구조를 각각 두 가지 방식으로 구현하고, 동일한 환경에서 성능과 특성을 비교·분석했습니다.

| **실험 주제** | **비교 대상** | **목적** |
| -------------- | ---------------- | -------------------- |
| **I/O 모델** | IOCP ↔ WSAPoll | 대규모 동시 접속 소켓 처리 방식 비교 |
| **스레드 간 전달 큐** | 이벤트 큐 ↔ 더블 스왑 버퍼 | 멀티스레드 메시지 전달 구조 비교 |
| **수신 버퍼 구조** | 링 버퍼 ↔ 가변 버퍼 | 패킷 수신 및 메모리 관리 방식 비교 |

---

## 아키텍처

<p align="center">
  <img width="100%" style="max-width: 1000px; height: auto;" alt="Architecture" src="./images/architecture.png" />
</p>

패킷이 **수신 → 배치 송신**까지 흐르는 단일 파이프라인이고, 위 3가지 실험은 이 흐름의 서로 다른 지점에 위치합니다.

- **계층 분리** — 워커 스레드(= CPU 코어 수)는 수신·파싱만 담당하고, 게임 상태는 33ms(≈30Hz) 틱을 도는 **단일 로직 스레드가 단독 소유**합니다. 덕분에 플레이어·총알 상태에 락이 필요 없습니다.
- **서버 권위** — 클라이언트는 입력 키만 보내고 이동·충돌·시야 판정은 서버가 합니다. AOI는 공간 그리드로 **주변 이웃 셀만 계산**해 전송합니다.

---

## 실험 1 · I/O 모델 — IOCP vs WSAPoll

> "게임 서버는 IOCP로 짜야한다." 라는 통념에 따라 IOCP로 구현했지만, **그래서 폴링과 실제로 얼마나 차이가 나는거지?** 직접 재보기로 했습니다.
> WSAPoll 단일 스레드 서버를 따로 만들어 동일한 부하로 붙였습니다.

**① 1차 측정 — 예상과 정반대의 결과**

전 구간(500~5000)에서 오히려 **폴링이 IOCP보다 압도적으로 빨랐습니다.** "IOCP가 정답"이라는 통념과 반대라, 바로 결론을 내리는 대신 원인을 파고들었습니다.

**② 원인은 I/O 모델이 아니라 '송신 방식'이었다**

코드를 뜯어보니 폴링 서버는 틱 끝에 모아 보내는 **배치 송신**인데, IOCP 쪽은 **패킷마다 게임 로직 스레드에서 즉시 `WSASend`** 를 호출하고 있었습니다. 송신 비용이 로직 틱을 잡아먹어, 정작 비교하려던 변수(I/O 모델)가 아니라 **엉뚱한 변수(송신 방식)가 결과를 지배**하고 있었습니다.

**③ 변수를 통제하고 재측정**

양쪽 모두 **틱당 세션 1회 배치 송신**(`queueSend` → `FlushPending`)으로 통일한 뒤 다시 쟀습니다. 그러자 결과가 뒤집혀, **전 구간에서 IOCP가 앞섰고 부하가 커질수록 격차가 벌어졌습니다(6.8% → 12.3%)**.

<br/>

<table border="0" style="border-collapse: collapse; border: none; width: 100%;">
  <tr style="border: none;">
    <td width="50%" align="center" style="border: none; vertical-align: middle; padding-right: 12px;">
      <img width="100%" style="max-width: 500px; height: auto;" alt="I/O Model Benchmark Graph" src="./images/experiment1-graph.png" />
    </td>
    <td width="50%" align="center" style="border: none; vertical-align: middle; padding-left: 12px;">
      <table style="width: 100%; text-align: center; font-size: 0.9em;">
        <thead>
          <tr>
            <th>동접(봇)</th>
            <th>WSAPoll</th>
            <th>IOCP</th>
            <th>격차</th>
            <th>격차 %</th>
          </tr>
        </thead>
        <tbody>
          <tr><td>500</td><td>14.09</td><td>13.14</td><td><strong>+0.96</strong></td><td>6.8%</td></tr>
          <tr><td>1000</td><td>31.53</td><td>28.92</td><td><strong>+2.61</strong></td><td>8.3%</td></tr>
          <tr><td>2000</td><td>56.53</td><td>51.33</td><td><strong>+5.21</strong></td><td>9.2%</td></tr>
          <tr><td>3000</td><td>88.66</td><td>77.99</td><td><strong>+10.68</strong></td><td>12.0%</td></tr>
          <tr><td>4000</td><td>121.02</td><td>108.07</td><td><strong>+12.96</strong></td><td>10.7%</td></tr>
          <tr><td>5000</td><td>151.41</td><td>132.81</td><td><strong>+18.60</strong></td><td>12.3%</td></tr>
        </tbody>
      </table>
    </td>
  </tr>
</table>

<br/>

> ⚠️ 절대적인 틱 시간은 두 방식 모두 게임 로직 연산 부하로 인해 **33ms 예산을 초과**합니다. 이 실험이 말하는 건 "같은 로직을 얼마나 효율적으로 처리하느냐"이지 "몇 명을 수용하느냐"가 아닙니다.

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
평균 틱에서는 문제가 없었습니다. 문제는 테스트 봇 N마리를 **동시에 접속시킨 순간**이었습니다. 봇들이 한꺼번에 패킷을 쏟아내자 특정 틱에서 큰 스파이크가 생겼습니다.
</details>
**② 원인 — 메시지당 락 경합**
- **핵심 교훈:** 락 경합의 이점은 게임 로직 연산에 가려 상위 지표(전체 틱)에서는 안 보인다. **어디를 재느냐가 결과를 만든다.**
소비자인 게임 스레드는 `while(Pop())`으로 **메시지 하나당 락을 한 번씩** 잡는데, 그동안 워커 스레드들도 계속 `Push`하며 **같은 뮤텍스 하나를 두고 경합**하고 있었습니다. 부하가 커질수록 락 경합도 늘어나는 구조였습니다.
<details><summary>토글 & 스왑 아이디어 (<code>USE_EVENT_QUEUE</code>)</summary>
**③ 해결 — 더블 스왑 버퍼**
```cpp
"어쩔 수 없다"로 넘기려다, 락을 최소화할 방법을 찾던 중 더블 스왑 버퍼를 알게 됐습니다. 게임 스레드는 **프레임당 `Swap()` 한 번**으로 back 버퍼를 통째로 가져오고, 처리는 **락 없이** 합니다. 스왑은 포인터만 교체하는 **O(1)** 이라, 락 유지 시간도 배치 크기와 무관하게 일정합니다.
    extern EventQueue<RecvPacket>  g_recvQueue;   // push/pop 매번 mutex
<br/>

<table border="0" style="border-collapse: collapse; border: none; width: 100%;">
  <tr style="border: none;">
    <td width="50%" align="center" style="border: none; vertical-align: middle; padding-right: 12px;">
      <img width="100%" style="max-width: 500px; height: auto;" alt="Double Swap Buffer Benchmark Graph" src="./images/experiment2-graph.png" />
    </td>
    <td width="50%" align="center" style="border: none; vertical-align: middle; padding-left: 12px;">
      <table style="width: 100%; text-align: center; font-size: 0.9em;">
        <thead>
          <tr>
            <th>지표</th>
            <th>이벤트 큐</th>
            <th>더블 스왑</th>
            <th>개선</th>
          </tr>
        </thead>
        <tbody>
          <tr><td>avg</td><td>15.64</td><td>13.14</td><td>–</td></tr>
          <tr><td>p50</td><td>13.49</td><td>12.28</td><td>–</td></tr>
          <tr><td><strong>p99</strong></td><td>41.83</td><td>21.83</td><td>예산 초과 → <strong>이하</strong></td></tr>
          <tr><td><strong>p99.9</strong></td><td>77.84</td><td>28.91</td><td>예산 초과 → <strong>이하</strong></td></tr>
          <tr><td><strong>max</strong></td><td>83.93</td><td>49.62</td><td><strong>−41%</strong></td></tr>
        </tbody>
      </table>
      <div style="text-align: right; font-size: 0.8em; margin-top: 4px; color: #888;">
        단위 ms · 동일 부하 60초 측정
      </div>
    </td>
  </tr>
</table>
report("total", bench.samples);      // 틱 전체 (sim + flush)
report("sim",   bench.simSamples);   // 시뮬레이션만 (recv 드레인 + Update)
report("flush", bench.flushSamples); // 네트워크 flush만 (FlushPending)
// → " total | avg .. | p50 .. | p99 .. | p99.9 .. | max .. "
```
**배운 점** — **초기에는 평균 틱 타임만 측정했으나, 평균(15.6ms → 13.1ms)은 심각한 스파이크를 숨기고 있었습니다.** 33ms(30Hz) 예산을 초과하여 순간적인 렉을 유발하는 진짜 원인은 꼬리 지표(p99)에 숨어있었고, 이를 추적하면서 **실시간 게임 서버에서는 '평균'이 아닌 'p99' 관점으로 측정하고 최적화해야 한다는 사실**을 알게되었습니다.```bash
# 렌더링 클라이언트 (시야/전장의 안개 시각화)
go run .
```

**설계 토글** — 매크로 하나로 각 실험의 두 방식을 전환 (모두 `NetworkCore.h`)

| 실험 | 매크로 | 정의 시 → 미정의 시 |
|---|---|---|
| 1. I/O 모델 | `USE_POLLING` | WSAPoll 단일 스레드 → **IOCP 워커 풀** |
| 2. 수신 버퍼 | `USE_VECTER_BUFFER` | 가변 버퍼 → **링 버퍼** |
| 3. 전달 큐 | `USE_EVENT_QUEUE` | 이벤트 큐 → **더블 스왑** |

<br/>

소비자 쪽 락 획득이 **메시지당 → 프레임당**으로 줄면서, **피크 프레임 타임(max)이 83.9ms → 49.6ms**, **p99가 41.8ms → 21.8ms**로 절반 가까이 떨어졌습니다. 무엇보다 이벤트 큐에서 **33ms 예산을 넘던 p99·p99.9가 더블 스왑에서는 예산 안으로 들어왔습니다.**

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

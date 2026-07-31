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

<p align="center">
  <img src="https://github.com/user-attachments/assets/2fd61d01-0b96-4e7d-8aba-fbf2f89c748b"
       width="400"
       alt="플레이 화면" />
  &nbsp;&nbsp;&nbsp;
  <img src="https://github.com/user-attachments/assets/e925ef4a-b155-4562-a709-92470e13a23c"
       width="400"
       alt="서버 통계" />
</p>
<p align="center">
  <sub><strong>▲ (좌) Go 언어 기반 그래픽 클라이언트 플레이 화면  </strong>   &nbsp;|&nbsp;   <strong>  (우) 서버 실시간 Tick/I/O 벤치마크 모니터링 콘솔 ▲</strong></sub>
</p>

---

> 게임 콘텐츠 구현보다 고성능 네트워크 라이브러리와 서버 코어 구조 설계에 집중한 프로젝트입니다.<br/>
> 실제 게임 서버에서 중요한 3가지 핵심 구조를 각각 두 가지 방식으로 구현하고, 동일한 환경에서 성능과 특성을 비교·분석했습니다.
> 
| **실험 주제**      | **비교 대상**        | **목적**               |
| -------------- | ---------------- | -------------------- |
| **I/O 모델**     | IOCP ↔ WSAPoll   | 대규모 동시 접속 소켓 처리 방식 비교   |
| **스레드 간 전달 큐** | 이벤트 큐 ↔ 더블 스왑 버퍼 | 멀티스레드 메시지 전달 구조 비교   |
| **수신 버퍼 구조**   | 링 버퍼 ↔ 가변 버퍼     | 패킷 수신 및 메모리 관리 방식 비교 |

---

## 아키텍처

<p align="center">
<img width="1536" height="1024" alt="image" src="https://github.com/user-attachments/assets/b65967d7-d010-493f-81f2-037206a767a9" />



</p>

패킷이 **수신 → 배치 송신**까지 흐르는 단일 파이프라인이고, 위 3가지 실험은 이 흐름의 서로 다른 지점에 위치합니다.

- **계층 분리** — 워커 스레드(= CPU 코어 수)는 수신·파싱만 담당하고, 게임 상태는 33ms(≈30Hz) 틱을 도는 **단일 로직 스레드가 단독 소유**합니다. 덕분에 플레이어·총알 상태에 락이 필요 없습니다.
- **서버 권위** — 클라이언트는 입력 키만 보내고 이동·충돌·시야 판정은 서버가 합니다. AOI는 공간 그리드로 **주변 이웃 셀만 계산**해 전송합니다.

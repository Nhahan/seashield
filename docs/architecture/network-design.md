# 네트워크 레이어 설계서 (Network Layer Design)

> Project SeaShield — P1 네트워크 코어의 구현 기준 문서.
> 상위 문서: [01-기획서.md](../01-기획서.md) §4 (시스템 아키텍처), §4.8 (멀티클라이언트 서버 메커니즘)
>
> 문서 버전: v1.0 (2026-06-10) · 상태: P1 구현 기준 확정

---

## 1. 목표와 비목표

### 1.1 목표

| # | 목표 | 출처 |
|---|---|---|
| G1 | TCP 다중 클라이언트(≥8) 동시 접속 수용 — 프레이밍·수명주기·격리 포함 | 기획서 §1.3 하드 요구사항, §4.8 |
| G2 | UDP 송수신 엔드포인트 (P3 프로토콜 레이어의 운반 계층) | 기획서 §4.3 |
| G3 | kqueue(macOS)/epoll(Linux)를 단일 인터페이스로 추상화한 Reactor 이벤트 루프 | 기획서 §4.5 |
| G4 | 느린 클라이언트가 서버·타 클라이언트에 영향을 주지 않는 격리(backpressure) | 기획서 §4.8 |
| G5 | 소켓 무의존 순수 로직(프레이밍·송신 큐)의 단위 테스트 가능 구조 | 기획서 §10.1 |

### 1.2 비목표 (의도적 범위 제외)

- **암호화/인증 강화**: LAN 신뢰 환경 전제 (기획서 §6 보안 경계). 세션 토큰은 P3에서.
- **reliable UDP**: P3 프로토콜 레이어 소관. P1의 UDP는 원시 데이터그램 송수신만.
- **멀티 I/O 스레드**: P1은 단일 I/O 스레드. 확장 경로만 설계에 반영 (§7).
- **IOCP(Windows)**: 인터페이스 수준의 수용 가능성만 명시 (§4.4), 구현하지 않음.
- **혼잡 제어, 대역폭 셰이핑**: 요구 규모(LAN, ≤8 클라이언트)에서 불필요.

---

## 2. 전체 구조

```
                        ┌──────────────────────────────────────────┐
                        │              I/O Thread (1개)             │
                        │  ┌────────────────────────────────────┐  │
   TCP 연결 ──────────▶ │  │ EventLoop (kqueue | epoll)         │  │
   UDP 데이터그램 ────▶ │  │  run_once(timeout) → 콜백 디스패치   │  │
                        │  └──────┬─────────────┬───────────────┘  │
                        │         │             │                  │
                        │   ┌─────┴────┐  ┌─────┴────────┐         │
                        │   │ Acceptor │  │ UdpEndpoint  │         │
                        │   └─────┬────┘  └──────────────┘         │
                        │         │ 신규 연결                       │
                        │   ┌─────┴──────────────────────┐         │
                        │   │ TcpSession (클라이언트당 1개) │         │
                        │   │  ├ FrameParser (수신, 순수)  │         │
                        │   │  └ SendQueue   (송신, 순수)  │         │
                        │   └────────────────────────────┘         │
                        └──────────────────────────────────────────┘
```

- 모든 네트워크 객체는 **I/O 스레드에서만** 접근한다 (스레드 친화도 규약, §7).
- `FrameParser`/`SendQueue`는 fd를 모르는 **순수 클래스** — 소켓 없이 단위 테스트 (G5).
- P2 이후 시뮬레이션 스레드와는 SPSC 큐로만 통신 (기획서 §4.6) — P1 범위 외.

---

## 3. EventLoop 인터페이스

```cpp
struct IoEvents {                      // 비트마스크
  static constexpr unsigned kRead   = 1u << 0;
  static constexpr unsigned kWrite  = 1u << 1;
  static constexpr unsigned kError  = 1u << 2;
  static constexpr unsigned kHangup = 1u << 3;
};
using IoCallback = std::function<void(unsigned events)>;

class EventLoop {
 public:
  static std::unique_ptr<EventLoop> create();   // 플랫폼별 구현 선택
  virtual ~EventLoop() = default;
  virtual bool add(int fd, unsigned interest, IoCallback callback) = 0;
  virtual bool modify(int fd, unsigned interest) = 0;
  virtual bool remove(int fd) = 0;
  // timeout_ms 동안 블록 (-1 = 무한). 디스패치한 이벤트 수, 타임아웃 0, 치명적 오류 -1.
  virtual int run_once(int timeout_ms) = 0;
  virtual void wakeup() = 0;          // 유일한 스레드 안전 메서드
};
```

### 3.1 인터페이스 설계 결정

| 결정 | 근거 |
|---|---|
| **readiness 기반** (준비되면 알림 → 호출자가 read/write) | kqueue/epoll의 공통 모델. completion 기반(IOCP)과의 경계는 §4.4 |
| **레벨 트리거(LT)** | 정확성 우선: 한 번에 다 읽지 못해도 다음 루프에서 재통지 → "읽다 만 데이터" 버그 부재. ET는 EAGAIN까지 드레인 강제 + 기아 관리가 필요해 복잡도 대비 이득이 이 규모에선 없음. ET 전환은 성능 측정 후 결정할 확장 항목 |
| **WRITE 관심은 송신 잔량이 있을 때만 등록** | LT에서 WRITE를 상시 등록하면 소켓 버퍼가 빈 동안 매 루프 깨어나는 busy-loop 발생. `SendQueue`가 비면 즉시 해제 (§6.2) |
| **콜백 디스패치 시 fd 재조회 + 세대 토큰(generation) 검증** | 한 배치 안에서 앞선 콜백이 뒤의 fd를 close/remove할 수 있음 → 테이블 재조회로 제거된 fd는 건너뜀. 단, 재조회만으로는 부족: **fd 번호는 close 직후 즉시 재사용**되므로(같은 배치에서 close + accept면 같은 번호) 옛 등록의 이벤트가 새 세션에 배달될 수 있다(silent cross-session delivery). 등록 시 세대 번호를 kqueue `udata` / epoll `data.u64`에 실어 보내고, 디스패치 시 테이블의 현재 세대와 일치할 때만 콜백을 호출한다 |
| **EINTR → 0 반환** | 시그널로 깨어난 것은 오류가 아님 — 호출자 루프의 다음 반복이 자연스러운 재시도 |
| **`run_once` 단위 노출 (run 루프를 내장하지 않음)** | 호출자(서버 메인)가 틱 사이에 세션 정리·종료 플래그 검사 등을 끼워 넣을 수 있어야 함 (§6.3 지연 삭제) |

---

## 4. kqueue / epoll 차이와 흡수 전략

### 4.1 API 대응표

| 관점 | kqueue (macOS/BSD) | epoll (Linux) | 흡수 방법 |
|---|---|---|---|
| 등록 모델 | (fd, **필터**) 쌍 단위 — EVFILT_READ와 EVFILT_WRITE를 **개별 등록/삭제** | fd 단위 — `EPOLLIN\|EPOLLOUT` **비트마스크 한 번에** | 구현체가 이전 interest를 기억하고 **차분(diff)만 반영**: kqueue는 필터별 EV_ADD/EV_DELETE, epoll은 EPOLL_CTL_MOD |
| 변경 적용 | `kevent()` 한 호출에 changelist 배치 가능 | `epoll_ctl()` 호출당 1개 연산 | 인터페이스는 단건 `add/modify/remove`로 통일 (배치 최적화는 비목표) |
| EOF 통지 | `EV_EOF` 플래그 (READ 필터 이벤트에 동반) | `EPOLLRDHUP` (반쪽 종료), `EPOLLHUP` | 공통 `kHangup`으로 매핑. 단, **EOF 판정의 최종 권위는 `read() == 0`** — 플래그는 힌트로만 사용 (이식성 함정 회피) |
| 오류 통지 | changelist 항목별 `EV_ERROR` | `EPOLLERR` | 공통 `kError`. 콜백에서 `getsockopt(SO_ERROR)`로 원인 조회 |
| wakeup | `EVFILT_USER` + `NOTE_TRIGGER` (fd 불필요) | `eventfd` 를 EPOLLIN으로 등록 | 인터페이스는 `wakeup()` 하나. 구현 디테일 은닉 |
| 타임아웃 | `timespec` (ns 정밀도) | `int` ms | 인터페이스는 ms — 도메인 요구(틱 16.6ms)에 충분 |

### 4.2 가장 까다로운 불일치 — 등록 모델

kqueue에서 "READ만 → READ+WRITE"는 WRITE 필터의 EV_ADD 1건이지만, "READ+WRITE → READ"는 WRITE 필터의 **EV_DELETE**가 필요하다. 존재하지 않는 필터에 EV_DELETE를 던지면 changelist 오류(ENOENT)가 돌아온다. 따라서 **구현체가 fd별 이전 interest를 보관**하고 전이 차분만 적용하는 것이 추상화의 핵심이다. epoll은 동일 전이가 EPOLL_CTL_MOD 1건이므로 같은 "이전 상태 보관" 구조를 재사용한다.

### 4.3 wakeup의 용도와 보장

- 용도: 다른 스레드(P2의 시뮬레이션 스레드, 종료 신호 등)가 `run_once` 블록을 즉시 깨움.
- 보장: `wakeup()`만 스레드 안전. 그 외 메서드를 I/O 스레드 밖에서 부르는 것은 규약 위반 (디버그 빌드에서 스레드 ID 검증 — §7).
- 합치기(coalescing): 연속 wakeup은 1회 통지로 합쳐질 수 있음 — "깨어난다"만 보장하며 횟수는 보장하지 않는다.
- **누락 없는 wakeup 규약**: 생산자는 "데이터를 큐에 push한 뒤 wakeup" 순서를 지키고, 소비자는 깨어난 뒤 큐를 끝까지 드레인한다 — 합쳐짐이 있어도 push가 통지보다 먼저이므로 유실 불가. 드레인 메커니즘: epoll은 eventfd를 read해 카운터를 0으로 리셋, kqueue는 EV_CLEAR 등록으로 전달 즉시 자동 리셋.
- **kError/kHangup도 힌트다**: 오류·행업 비트만 도착해도 종료 판정은 수신 경로로 합류시켜 `read()`의 0/오류 반환으로 확정한다(§4.1 EOF 철학의 확장). kError 시 `getsockopt(SO_ERROR)`로 원인을 로깅.

### 4.4 IOCP 수용 가능성 (설계 여지만)

IOCP는 "준비됨"이 아니라 "**완료됨**(버퍼에 이미 수신됨)"을 통지하는 Proactor다. 본 인터페이스를 IOCP로 구현하려면 구현체 내부에서 zero-byte read 트릭 또는 내부 버퍼 운용으로 readiness를 모사해야 한다. 현재 콜백 단위(이벤트 통지)를 유지하되, 상위 계층(`TcpSession`)이 "수신된 버퍼"만 다루도록 분리해 두었으므로 — read 호출이 `TcpSession` 한 곳에 격리됨 — 추후 "버퍼 전달" 수준 인터페이스로 올리는 변경 비용이 작다. 구현은 범위 외.

---

## 5. 수신 경로 — 프레이밍

### 5.1 와이어 포맷

```
[ length: uint16 LE ][ payload: length bytes ]      length ∈ [1, 16384]
```

- **길이 프리픽스 선택 근거**: 구분자(delimiter) 방식은 바이너리 페이로드와 충돌하고 스캔 비용이 있다. 길이 프리픽스는 O(1) 경계 판정.
- `length == 0` 또는 `> 16KB`는 **프로토콜 위반 → 즉시 절단**. uint16 필드의 이론상 최대는 65535이지만 16KiB는 **정책 상한**이다 — 제어 채널 메시지에 충분하면서 수신 측 메모리 상한을 보장하는 값(P3 패킷 설계의 ~1200B UDP 상한과 별개인 TCP 제어 채널의 자체 상한).
- **ingress 백프레셔(내재적 유계)**: 파서는 완성 프레임을 즉시 배출하므로 수신 누적 버퍼는 구조적으로 `헤더(2B) + 16KiB` 미만을 유지 — 인바운드 방향의 메모리 상한이 설계로 보장된다. 단, **미완성 프레임을 들고 무한정 버티는 유휴 연결(slow-loris류)** 의 차단은 타임아웃이 필요하며, 이는 P3 keepalive와 함께 도입한다(P1 비목표로 명시).

### 5.2 FrameParser (순수 클래스)

```cpp
class FrameParser {
 public:
  using FrameHandler = std::function<void(std::span<const std::uint8_t>)>;
  // 수신 바이트를 누적하고 완성된 프레임마다 handler 호출.
  // 프로토콜 위반 시 false (호출측은 세션 절단).
  bool feed(std::span<const std::uint8_t> data, const FrameHandler& handler);
  std::size_t buffered_bytes() const;
  static void encode(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> payload);
};
```

- **부분 수신**: TCP는 스트림이므로 한 번의 read에 0.5개·1.5개·N개의 프레임이 올 수 있다. 파서는 내부 누적 버퍼에 바이트를 모으고 완성분만 배출하는 **상태 보존 파서**다.
- 한 번의 `feed`에서 여러 프레임이 완성되면 모두 순서대로 배출. 처리 오프셋을 전진시키고 마지막에 한 번만 compaction — 프레임당 O(1) 상각.
- fd, errno, EventLoop을 일절 모름 → 바이트 배열만으로 단위 테스트 (G5). "1바이트씩 흘려 넣기" 같은 최악 분할 케이스를 테이블 테스트로 고정.

### 5.3 read 루프 규약

```
on kRead:
  loop:
    n = read(fd, buf, 16KiB)        // 최대 프레임 크기와 정합 — 큰 프레임도 적은 시스템콜로
    n > 0  → parser.feed(...)  (false → close("protocol violation"))
    n == 0 → close("peer closed")                    // EOF의 최종 권위
    n < 0  → EINTR → continue / EAGAIN → break / 그 외 → close("read error")
```

LT이므로 한 번만 읽고 반환해도 정확하지만, 남은 데이터가 있으면 다음 `run_once`까지 지연되므로 EAGAIN까지 드레인한다(시스템콜 수 절약 + 지연 최소화).

---

## 6. 송신 경로 — SendQueue와 backpressure

### 6.1 SendQueue (순수 클래스)

```cpp
class SendQueue {
 public:
  explicit SendQueue(std::size_t max_bytes);          // 기본 256 KiB
  bool push(std::vector<std::uint8_t> frame);          // 초과 시 false + overflowed
  enum class FlushResult { kDrained, kWouldBlock, kError };
  // writer: >0 쓴 바이트 / 0 would-block / -1 치명적 오류
  FlushResult flush(const std::function<long(const std::uint8_t*, std::size_t)>& writer);
  bool empty() const;  std::size_t size_bytes() const;  bool overflowed() const;
};
```

- **partial write**: 커널 송신 버퍼가 차면 `write`는 일부만 쓴다. 큐가 청크별 소비 오프셋을 보관하고 다음 flush에서 이어 쓴다.
- writer를 함수로 주입받아 **소켓 없이 단위 테스트** — "3바이트만 써주는 writer"로 partial write 경로를 결정적으로 재현 (G5).

### 6.2 흐름과 WRITE 관심 관리

```
send(payload):
  frame = encode(payload)
  push 실패(상한 초과) → close("send queue overflow — slow client")   ← 격리 정책(G4)
  flush 시도 → kWouldBlock이고 잔량 있으면 WRITE 관심 등록
on kWrite:
  flush → kDrained면 WRITE 관심 해제 (busy-loop 방지, §3.1)
```

### 6.3 backpressure 정책 (기획서 §4.8의 구현 명세)

| 항목 | 값/규칙 | 근거 |
|---|---|---|
| 클라이언트별 송신 큐 상한 | **256 KiB** (서버 옵션 `--send-cap`) | 30Hz 스냅샷 수 초 분량. 이를 못 비우는 클라이언트는 회복 불가능하게 뒤처진 것 |
| 초과 시 | **해당 세션만 즉시 절단** | 오래된 시뮬레이션 상태를 붙들고 기다리는 것은 무가치 — 차라리 재접속 + 전체 스냅샷 재동기화(P3)가 빠름 |
| 서버 전역 영향 | 없음 — 큐는 세션별, write는 non-blocking | 한 클라이언트의 지연이 틱·타 클라이언트로 전파되지 않음 (G4). 부하 테스트에서 의도적 저속 클라이언트로 실증 |

### 6.4 세션 수명주기

```
CONNECTING (accept 직후) ──▶ ACTIVE ── close(reason) ──▶ DISCONNECTED ── (owner가 지연 삭제)
```

- 기획서 §4.8의 상태머신(`CONNECTING → HANDSHAKE → ACTIVE → DISCONNECTED`)에서 **P1은 HANDSHAKE를 생략한 부분집합** — 역할 협상이 P3 프로토콜 소관이므로 상태 이름만 예약해 둔다.
- **재접속 복귀의 구조적 요구(P3 예고)**: 기획서 §4.8의 "세션 토큰으로 재접속 시 역할 복귀"는 transport 세션(이 문서의 수명주기)보다 **오래 살아남는 세션 예약 테이블**이 필요하다 — 즉 P3의 논리 세션과 P1의 transport 세션은 수명이 다른 별개 객체임을 지금 명시해 둔다.
- `close()`는 멱등: 최초 1회만 fd 해제·루프 등록 해제·CloseHandler 통지.
- **지연 삭제(deferred deletion)**: 콜백 스택 안에서 세션 객체를 delete하면 콜백 복귀 시 use-after-free. CloseHandler는 소유자(SessionManager)에 id만 통지하고, 소유자는 `run_once` 반환 후 일괄 삭제한다.

---

## 7. 스레딩 규약

- **P1은 I/O 스레드 1개가 전부**다. 모든 네트워크 객체는 이 스레드 친화도(thread affinity)를 가진다.
- 외부 스레드에 허용된 것은 `EventLoop::wakeup()` 뿐.
- 디버그 빌드에서 생성 스레드 ID를 기록하고 메서드 진입 시 assert — 규약을 기계적으로 검증.
- 확장 경로 (기획서 §4.6): P2에서 시뮬레이션 스레드 추가 시 통신은 SPSC 큐 + wakeup. I/O 스레드 증설 시(SO_REUSEPORT) 수신 큐를 Vyukov MPSC로 전환.
- **G4(격리) 논증의 범위**: P1에서는 단일 스레드 + 세션별 송신 큐로 성립한다. P2에서 시뮬→I/O SPSC 경계가 생겨도, 스냅샷은 SPSC에서 pop한 뒤 **세션별 송신 큐로 복제·분배**되므로 정체된 세션이 공유 큐를 역류시킬 수 없는 구조를 설계 불변식으로 유지한다 — "느린 클라이언트 격리"는 스레드 토폴로지가 바뀌어도 깨지지 않는 속성으로 정의.

---

## 8. 소켓·에러 처리 규약

| 항목 | 규칙 |
|---|---|
| SIGPIPE | 프로세스 전역 `SIG_IGN` + macOS는 소켓별 `SO_NOSIGPIPE`, Linux는 `send(MSG_NOSIGNAL)` 이중 방어 |
| non-blocking + CLOEXEC | 모든 소켓. macOS는 accept 직후 **fcntl 2회** — `O_NONBLOCK`은 파일 *상태* 플래그(`F_GETFL`/`F_SETFL` read-modify-write), `FD_CLOEXEC`는 *디스크립터* 플래그(`F_SETFD`)로 **서로 다른 플래그 공간이라 한 호출로 합칠 수 없음**. Linux는 `accept4(SOCK_NONBLOCK\|SOCK_CLOEXEC)`로 원자적 — 비원자 macOS 경로와의 비대칭은 의도된 플랫폼 차이로 명시 |
| EINTR | 모든 시스템콜 재시도 래퍼 경유. `run_once`의 대기 시스템콜은 EINTR 시 0 반환(호출자 루프가 자연 재시도) |
| accept 루프 | EAGAIN까지 드레인. `EMFILE`/`ENFILE`(fd 고갈) 시 단순 "거절 후 지속"은 **LT에서 리스너가 계속 readable로 남아 100% CPU 스핀** — 리스너의 READ 관심을 일시 해제(pause)하고, 세션이 닫혀 fd가 확보되면 재무장(resume)하는 방식으로 회피 |
| 수용 한도 (admission control) | 동시 세션 상한(`--max-clients`, 기본 64) 초과 시 accept 직후 즉시 close — fd 고갈에 도달하기 전 단계의 1차 방어선 |
| 종료 의미론 | 정상 종료는 송신 큐 드레인 후 close(FIN). **강제 절단**(송신 큐 초과·프로토콜 위반)은 미송신 데이터를 버리고 즉시 close — 수신 미처리 데이터가 남아 있으면 커널이 RST를 보낼 수 있으나, "회복 불가능한 클라이언트에게 오래된 데이터를 끝까지 보내주지 않는다"는 §6.3 정책과 일치하는 의도된 동작 |
| 클라이언트 connect 경로 | P1의 도구/테스트 클라이언트는 **블로킹 소켓**(이벤트 루프 비참여) — non-blocking connect(`EINPROGRESS` → kWrite → `SO_ERROR` 확인) 경로는 서버가 아웃바운드 연결을 만들지 않으므로 P1 비목표로 명시 |
| listen | `SO_REUSEADDR`(TIME_WAIT 재바인드), backlog 128 |
| fd 소유권 | 전 구간 `UniqueFd`(RAII) — 누수·이중 close 원천 제거 |
| UDP | recvfrom EAGAIN까지 드레인. sendto가 EAGAIN이면 **드롭+카운트** (UDP 의미론상 재시도 무가치, 통계는 관측성용) |

---

## 9. 대안 비교 — "알고도 직접 만들었다"

| 대안 | 장점 | 본 프로젝트에서 기각한 이유 |
|---|---|---|
| **Boost.Asio** | 검증된 크로스플랫폼 Proactor 모사, 코루틴 지원 | OS 레벨 I/O 멀티플렉싱을 직접 다뤄본 경험이 포트폴리오의 핵심 목적. 의존성 0 원칙(기획서 §8). 실무 신규 프로젝트라면 1순위 검토 대상임을 인지 |
| **libuv / libevent** | 가볍고 성숙 | C API 래핑 비용 + 동일한 학습 목적 상실 |
| **select/poll 직접** | 가장 단순 | select는 `FD_SETSIZE`(통상 1024) 제한 + 매 호출 O(n), poll은 fd 수 제한은 없으나 역시 매 호출 O(n) 전달·스캔 — epoll/kqueue가 존재하는 이유 자체가 어필 포인트 |
| **스레드-퍼-커넥션 + blocking I/O** | 코드 단순 | 클라이언트 수만큼 스레드 — 이 규모(≤8)에선 사실 동작함. 그러나 시뮬레이션 틱과의 결합(P2)에서 이벤트 루프가 구조적으로 우월하고, 확장성 논증이 면접 단골 |

---

## 10. 테스트 전략

| 계층 | 대상 | 방법 |
|---|---|---|
| 단위 (순수) | FrameParser | 바이트 분할 테이블 테스트: 0.5개/1.5개/N개/1바이트씩/최대길이/위반(0, >16KB) |
| 단위 (순수) | SendQueue | 주입 writer로 partial write·would-block·상한 초과 결정적 재현 |
| 단위 (OS) | EventLoop | pipe 쌍으로 kRead 통지, 별도 스레드에서 wakeup → run_once 즉시 복귀, 타임아웃 0 반환, **같은 배치 내 fd close→재사용 시 옛 이벤트가 새 등록에 배달되지 않음(세대 토큰 검증)** |
| 통합 | Acceptor+TcpSession+UDP | 루프백 실소켓: 에코 round-trip(한 write에 2.5프레임), 느린 클라이언트(수신 정지) → 큐 상한 절단 확인, UDP 에코 |
| 비기능 | 전 계층 | CI에서 ASan/UBSan/TSan 매트릭스 (kqueue: macOS 러너, epoll: Linux 러너 + 로컬 Docker) |

통합 테스트의 스레딩: EventLoop은 전용 스레드에서 구동, 테스트 본문은 **blocking 클라이언트 소켓**으로만 상호작용 (루프 객체 직접 접근 금지 — §7 규약을 테스트도 준수).

---

## 11. 한계와 확장 (정직성 절)

- **ET 미사용**: 측정 없이 ET로 가는 것은 추측 최적화. 성능 보고서(P6)에서 LT 오버헤드가 보이면 전환 검토.
- **epoll/kqueue 외 플랫폼 없음**: IOCP는 §4.4의 설계 여지로만.
- **단일 루프의 한계**: 모든 세션이 한 스레드 — 콜백 하나가 길어지면 전체 지연. P1 콜백은 모두 O(수신 바이트) 작업뿐이며, 무거운 로직(시뮬레이션)은 P2에서 별도 스레드로 분리되는 구조가 이 한계의 답.
- **`std::function` 콜백 비용**: 세션당 고정 1회 할당 수준 — 이 규모에서 측정 가치 없음. 프로파일에 나타나면 정적 디스패치로 교체.

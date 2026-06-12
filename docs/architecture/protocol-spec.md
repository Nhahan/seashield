# 프로토콜 명세서 (Protocol Specification)

> Project SeaShield — P3 프로토콜 레이어의 구현 기준 문서.
> 상위 문서: [01-기획서.md](../01-기획서.md) §4.3 (TCP/UDP 역할 분리), §4.4 (reliable UDP), §6 (프로토콜 개요)
>
> 문서 버전: v1.3 (2026-06-12) · 상태: P6 구현 기준 (프로토콜 버전 4)
> 구현: `protocol/` (서버·더미 클라이언트·UE5 클라이언트가 공유하는 독립 라이브러리 — 표준 라이브러리 외 의존성 0)
> v1.1: 프로토콜 v2 — kTrack 엔티티(추정 트랙 스트림), 트랙 수명주기 이벤트, FireRequest 트랙 지정, FireSolution 정의, 서버 리플레이 모드.
> v1.2: 프로토콜 v3 — ServerWelcome 날씨 스칼라(클라 비주얼 구동), FireSolution 주기 송신 개시, coasting staleness 게이트.
> v1.3: 프로토콜 v4 — 스냅샷 델타 압축(acked-baseline CV 잔차, §5b), UDP 바인딩 incarnation nonce, 바인드 시 TCP 이벤트 백로그. 변경 이력은 §11.

---

## 1. 설계 원칙

| 원칙 | 내용 | 출처 |
|---|---|---|
| 자체 바이너리 직렬화 | 리틀엔디언 고정, 필드 단위 명시적 패킹. 구조체 memcpy 금지(패딩·엔디언 비이식) | 기획서 §6 |
| 선택적 신뢰성 | TCP 재구현이 아니라 **Unreliable / Reliable-Unordered 2채널**만 제공. Reliable-Ordered는 의도적 미지원 — 전순서가 필요한 데이터는 TCP로 | 기획서 §4.4 |
| 독립 디코딩 | 모든 UDP 데이터그램은 단독으로 해석 가능 — 스냅샷 분할 배치 중 하나가 유실돼도 나머지는 유효 | 기획서 §6 단편화 정책 |
| 엄격한 파싱 | 범위 밖 enum, 잘린 페이로드, 트레일링 바이트 전부 거부(시나리오 파서와 동일한 문화). 경계 검사 실패는 sticky-fail로 전파 | P2 컨벤션 |
| 시간 주입 | reliable 계층은 소켓·벽시계를 모름(`now_s` 주입) → 유실/재정렬 property 테스트가 가상 시계로 결정론적 실행 | 기획서 §10.3 |

## 2. 전송 채널 구성

```
TCP (제어, 저빈도)                  UDP (상태, 고빈도)
 ├ [길이 2B LE][type 1B][페이로드]   ├ 패킷 헤더 12B + 메시지 묶음
 ├ ClientHello / ServerWelcome      ├ Unreliable: Snapshot, UdpHello(+Ack), Keepalive
 ├ ServerReject                     └ Reliable-Unordered: EngagementEvent
 └ FireRequest
```

- TCP 프레이밍은 P1의 `net::FrameParser`(2B LE 길이 프리픽스, 16KiB 상한)를 그대로 사용한다.
- UDP 메시지의 채널 배정 근거: 스냅샷은 "최신 상태만 가치"(오래된 상태의 재전송은 무가치), 교전 이벤트는 "정확히 1회 도달 필수 + 자체 틱 번호로 순서 재구성 가능"(기획서 §5.7 → Reliable-**Unordered**로 충분).

## 3. UDP 패킷 포맷

### 3.1 헤더 (12바이트)

| 오프셋 | 크기 | 필드 | 설명 |
|---|---|---|---|
| 0 | 2 | magic | `0x5EA5` (LE: `A5 5E`) |
| 2 | 1 | version | 현재 `2` (P4). 불일치 시 패킷 폐기 |
| 3 | 1 | channel + flags | bit0-6: 채널(0=Unreliable, 1=Reliable). **bit7: ack 유효 플래그** |
| 4 | 2 | sequence | 패킷 시퀀스. 채널 구분 없이 **연결당 단일 시퀀스 공간** |
| 6 | 2 | ack | 상대로부터 수신한 최신 패킷 시퀀스 |
| 8 | 4 | ack_bits | bit i = (ack−1−i) 수신 여부 — 최근 33개 패킷의 수신 상태를 모든 송신 패킷에 piggyback (기획서 §4.4) |

- **ack 유효 플래그(bit7)**: 아직 아무것도 수신하지 못한 엔드포인트는 ack 필드에 실을 정보가 없다. 플래그 없이 `ack=0`을 보내면 상대의 진짜 패킷 0번이 **가짜 확인응답**되는 결함이 생긴다 — 카오스 스윕 테스트가 실제로 검출한 버그이며, 플래그가 그 수정이다.
- 시퀀스 비교는 wrap-around 안전 연산: `int16_t(a - b) > 0` (기획서 §4.4).

### 3.2 메시지 묶음 (헤더 뒤)

```
[type 1B][len 2B LE][body len바이트]  반복
```

- Unreliable 채널: body = 메시지 페이로드.
- Reliable 채널: body = `[msg_id 2B LE][페이로드]` — msg_id가 수신측 중복 제거 키. **재전송은 새 패킷(새 seq)에 실리므로** 패킷 시퀀스가 아닌 msg_id로 디덥한다.
- 데이터그램 상한 **1200B**(헤더 포함, 기획서 §6) — IP 단편화에 맡기지 않는다. 같은 채널의 작은 메시지는 상한까지 greedy 번들링.

### 3.3 수신 윈도우와 중복 제거

서로 다른 두 가지를 같은 자료구조(1024-시퀀스 슬라이딩 비트 윈도우, RTP replay-window 방식)로 각각 추적한다:

| 윈도우 | 키 | 보호 대상 |
|---|---|---|
| 패킷 윈도우 | 패킷 seq | **중복 데이터그램** 통째 폐기 (네트워크 dup·재전송 아님 — 동일 패킷의 재도착). acks는 최초 수신 때 이미 반영 |
| 메시지 윈도우 | reliable msg_id | **재전송 중복** — 같은 메시지가 다른 패킷으로 두 번 도착하는 정상 상황의 exactly-once 보장 |

윈도우(1024)보다 오래된 시퀀스는 stale로 폐기한다. in-flight 상한(256) ≪ 윈도우 크기이므로 정상 피어는 이 영역에 도달할 수 없다(`diff == ±1024` 정확 경계는 단위 테스트로 고정).

### 3.4 재전송과 RTT

- RTT 추정: RFC 6298 형태 — `srtt = 7/8·srtt + 1/8·sample`, `rttvar = 3/4·rttvar + 1/4·|err|`, `RTO = clamp(srtt + 4·rttvar, 50ms, 1s)` (초기 200ms).
- 재전송이 **항상 새 시퀀스**로 나가므로 Karn 모호성이 없다 — 모든 ack가 깨끗한 RTT 샘플.
- reliable 메시지는 "운반 패킷이 ack될 때까지" in-flight: 마지막 송신이 RTO보다 오래되면 다음 flush에서 재번들.
- **ack-only 패킷**: reliable 패킷을 수신했고 25ms 내에 자체 송신이 없을 때만 빈 Unreliable 패킷으로 ack 전달. Unreliable 수신은 ack-only를 유발하지 않는다(두 유휴 엔드포인트가 서로의 ack를 ack하는 무한 루프 방지).
- in-flight 상한 256개. 초과 시 송신 실패 = 해당 클라이언트 회복 불능으로 간주(서버는 UDP 바인딩 해제) — P1 송신 큐 상한과 같은 격리 철학(네트워크 설계서 §6.3).
  - 사이징 근거: 최악 일제사(서버 상한 salvo 64)는 발사+판정 **128 이벤트** = 상한의 절반. 즉 정상 클라이언트가 한 일제사 전체를 ack하지 못한 채 **연속 두 번째 최악 일제사**가 끝나야 상한에 닿는다 — 그 시점이면 peer timeout(10초)이 먼저 발화하는 것이 통상 경로다. 한계 인지: 극단 시나리오(고지연 + 연속 대형 일제사)에서 회복 가능 클라이언트가 절단될 수 있음 — P4에서 이벤트 발생률이 정해지면 재사이징.
- 가장 오래된 in-flight가 10초간 미확인이면 **peer timeout** — reliable 계층의 생존성 판정. 침묵 클라이언트의 바인딩 해제가 타 클라이언트에 영향 없음을 통합 테스트로 검증(P1 slow-client 격리의 UDP 판).

## 4. 메시지 카탈로그

### 4.1 TCP 제어 (type 1–4)

| type | 메시지 | 필드 | 비고 |
|---|---|---|---|
| 1 | ClientHello | version u16, role u8, token u64 | token 0 = 신규, ≠0 = 재접속. version은 현재 2 |
| 2 | ServerWelcome | token u64, role u8, udp_port u16, tick_rate u16, snapshot_rate u16, weather str16, surface_wind_e/n f32×2, rain f32, gust_sigma f32, udp_nonce u32 | weather는 표시용 요약(≤512B). v3 스칼라 4종은 클라 비주얼 구동용 — 전체 기상 생성기는 서버 전용. rain∉[0,1]·gust<0·NaN은 디코더가 거부. udp_nonce(v4)는 incarnation별 바인딩 키(§5c) |
| 5 | EventBacklog | events: u8 count + EngagementEvent×n | TCP, 바인드 시점 캐치업(§5c). 255개 초과는 다중 프레임 |
| 22 | SnapshotAck | tick u32 | 클라→서버 unreliable: 완성-조립한 최신 틱 — 델타 베이스라인(§5b) |
| 23 | SnapshotDelta | tick u32, base_tick u32, phase u8, total u16, first u16, count u8 + DeltaEntity×n | §5b 잔차 스트림. base_tick≥tick·예약 mask 비트는 strict 거부 |
| 3 | ServerReject | reason u8 | 0 버전 불일치 / 1 역할 점유 / 2 만석 / 3 무효 토큰 |
| 4 | FireRequest | az f64, el f64, salvo u16, dispersion f64, interval f64, track_id u16 | track_id 0 = 수동(az/el 절대각). ≠0 = **트랙 지정 사격**: sim 스레드가 추정 트랙으로 해를 풀고 az/el은 운용자 보정(±15° 한도)으로 가산. 저널에는 **확정된 절대 제원**이 기록되어 리플레이가 재계산하지 않는다 |

역할: 0 Observer(다수 허용) / 1 Commander / 2 Weapons(각 1석) / 3 Solo(전 좌석 겸임 — 다른 비관전 역할과 상호 배타). FireRequest는 Weapons/Solo만.

### 4.2 UDP 데이터 (type 16–20)

| type | 메시지 | 채널 | 필드 |
|---|---|---|---|
| 16 | UdpHello | Unreliable | token u64 — Ack 수신까지 200ms 간격 반복 |
| 17 | UdpHelloAck | Unreliable | (빈 페이로드) |
| 18 | Keepalive | Unreliable | (빈) — 실제 화물은 헤더의 ack. 클라이언트 10Hz |
| 19 | Snapshot | Unreliable | §5 |
| 20 | EngagementEvent | **Reliable** | tick u32, kind u8, subject_id u16, miss f32, detonated u8, killed u8 |
| 21 | FireSolution | Unreliable | tick u32, track_id u16, valid u8, pip 3×i24(1cm), tof f32, dispersion_radius f32 — **v3부터 서버가 주기 송신**: 확정 트랙마다 시나리오 키 `fire_solution_rate_hz`(기본 2Hz, 0=off) 주기로 송출. **2Hz 근거**: solve 1회가 전체 탄도 RK4 적분(밀리초 단위)이라 30Hz 스냅샷 동승은 60Hz 틱 예산(16.6ms, §10.3 p99 8ms)을 위협 — PIP 표류 표시에는 2Hz로 충분. PIP는 트랙 추정의 CV 외삽(솔버 조준점과 동일 정의), dispersion_radius는 기본 산포(5mrad)×|PIP| 소각 근사 1σ — 운용자 산포 변경 시 클라가 선형 스케일. valid=false는 미수렴/stale 통지. staleness: `track_max_coast_scans`(기본 2) 이상 코스팅한 확정 트랙은 솔버가 kStale로 거부(지정 사격도 동일 게이트) |

EventKind: 0 발사 / 1 탄 판정(요격/실패) / 2 표적 격파 / 3 교전 종료 / **4 트랙 확정 / 5 트랙 소실** (P4). subject_id는 로켓 이벤트엔 로켓 id, 트랙 이벤트엔 트랙 id (v1의 rocket_id를 개명 — 와이어 동일). 트랙 **개시**는 이벤트가 없다: tentative 트랙은 다발·단명이며 스냅샷의 state 바이트가 이미 전달한다 — 놓치면 안 되는 천이만 reliable 채널에 태운다(§4.3 철학). 이벤트가 자체 tick을 가지므로 수신측이 순서를 재구성한다(Reliable-Unordered로 충분한 근거, 기획서 §5.7).

## 5. 스냅샷 동기화

- 서버 60Hz 시뮬레이션 → **30Hz 전체 스냅샷**(1차 구현 — 델타 압축은 P6 비교 실험 대상).
- 배치 구조: `tick u32, phase u8(0 진행/1 종료), total u16, first_index u16, count u8, entities[count]` — 각 데이터그램이 (tick, 슬라이스 범위)를 자체 보유 → **부분 유실 허용** (기획서 §6).
- 엔티티 레코드 **20B** (기획서 §6 산정과 일치):

| 필드 | 크기 | 양자화 |
|---|---|---|
| id | u16 | 로켓/트랙 id 하위 16비트 (단일 교전 규모에서 충돌 없음; kind로 공간 구분) |
| kind | u8 | 0 표적 / 1 로켓탄 / **2 트랙(칼만 추정 — 콘솔이 실제로 그리는 것, P4)** |
| state | u8 | 표적: 생존/격파. 로켓: 부스트/활공. 트랙: 0 tentative / 1 confirmed / 2 coasting(확정 후 미관측 외삽 중) |
| flags | u8 | 트랙: **위치 σ 로그 양자화** q=50·log₁₀(σ/0.1m), 0.1m~12.6km를 스텝당 ~4.7%로 — PPI 불확실성 타원용. 그 외 kind: 예약(0) |
| 위치 x,y,z | 3×i24 | **1cm LSB** → ±83.9km (시나리오 박스 40×40×10km 커버) |
| 속도 x,y,z | 3×i16 | **0.1m/s LSB** → ±3276.7m/s (아음속 위협·부스트 로켓 커버) |

- 데이터그램당 최대 58 엔티티(12+3+10+58×20 ≤ 1200B). 실전 ~100 엔티티 = 2 데이터그램 ≈ 기획서 산정(풀 스냅샷 ~2KB, 30Hz ~480kbps)과 일치.
- 양자화 오차 상한 LSB/2은 단위 테스트로 고정. 범위 밖·비유한 값은 클램프.

## 5b. 스냅샷 델타 압축 (v4, 기획서 §6)

- **동기**: 이 시뮬은 거의 모든 엔티티가 매 틱 움직여 "변경 엔티티만 송신"이 무효 —
  대신 양 끝이 **클라가 ack한 베이스라인에서 동일한 정수 CV 예측**을 수행하고 잔차만
  보낸다. `pred_pos_q = base_pos_q + (base_vel_q×10×dticks)/tick_rate` (64-bit 곱 후
  마지막 1회 절단 나눗셈 — 인코더·디코더가 같은 함수 `predict_position_q`를 공유).
- **레코드**: id u16 + mask u8(state/flags 변경 비트, full-escape 비트, kind 2비트) +
  res_pos i8×3 + res_vel i8×3 ≈ **9B** (풀 20B 대비 55%↓). 잔차 i8 초과·신규 엔티티·
  트랙 갱신 점프는 풀 EntityRecord escape. 삭제는 암시적(전 엔티티 나열).
- **유실 안전**: 델타는 항상 클라가 *증명 가능하게 보유한* acked base 기준 — 델타
  유실은 다음 델타로 자연 복원, 체인 단절 없음. ack 미수신·base 노화(링 ~2s)·델타
  비활성(`snapshot_delta=0`) 시 풀 스냅샷 폴백. 클라는 틱 완성마다 SnapshotAck 송신.
- **실측** (performance-report.md): ~100 엔티티·클라 8 기준 407 → **188 kbps/클라**
  (53.8%↓, §10.3 목표 <256kbps 충족). 회귀 가드: `DeltaStreamCutsDownlink...` E2E.
- **FireSolution 스트리밍 비용 통제** (v3 후속): 미수렴 solve는 솔버 반복 예산을
  소진(실측 ~60-70ms/회)하므로 **실패 트랙별 5s 백오프** — valid=false는 실패당 1회
  송신 후 침묵. 측정 근거는 performance-report.md §3.

## 5c. 재접속·바인딩 hardening (v4, P3 백로그 해소)

- **Incarnation nonce**: ServerWelcome.udp_nonce(접속/재접속마다 갱신)를 UdpHello가
  에코 — 구 소켓의 늦은 hello가 새 incarnation의 바인딩을 탈취할 수 없다(레이스 차단,
  `stale_udp_hellos` 카운터 + `StaleUdpHelloCannotStealTheBinding` 테스트).
- **이벤트 백로그**: UDP 바인드 시점에 그 세션이 아직 못 본 EngagementEvent 전체를
  **TCP**(kEventBacklog, 255개/프레임)로 재생 — 늦은 합류자·재접속자가 AAR-완전한
  이력을 갖는다(§5.8). 언바인드 시 백로그 커서를 바인드 시점으로 되감아 reliable
  in-flight 유실 가능분을 다음 바인드에서 재전송 — 경계 중복은 클라이언트의
  (kind, subject, tick) 디덥이 흡수한다. `LateJoinerCatchesUpViaEventBacklog` 테스트.
- **in-flight 상한 산정**: reliable 채널 상한은 `EndpointConfig.max_in_flight_messages`
  (기본 256). 점유원은 EngagementEvent뿐이며 최악 신(일제사 64발 = launch 64 +
  resolve 64 + 트랙 이벤트 수 개)도 한 자릿수 % 점유 — 256 유지 근거. 백로그가 TCP로
  이동했으므로 재접속 직후의 일괄 재전송이 이 상한을 소모하지 않는다.

## 6. 세션 흐름

```
TCP connect ──► ClientHello ──► ServerWelcome(token, udp_port)
                                   │
UDP: UdpHello(token) 반복 ◄────────┘     ← 서버가 출발지 주소를 토큰으로 식별·바인딩
     ◄── UdpHelloAck
교전 루프: ◄── Snapshot 30Hz / ◄── EngagementEvent(reliable)
           ──► Keepalive 10Hz (ack 운반) / ──► FireRequest (TCP)
```

- **논리 세션 vs transport**: 토큰으로 식별되는 논리 세션(역할 보유)은 TCP transport보다 오래 산다(네트워크 설계서 §6.4의 P3 예고 이행). 끊김 중에는 해당 좌석 권한 잠금; 다른 신규 접속이 그 역할을 가로챌 수 없다.
- **재접속**: `ClientHello(token≠0)` → 역할 복귀 + 새 Welcome. 양측 reliable 엔드포인트는 **재접속 시 함께 초기화**(시퀀스 공간은 쌍으로 태어나야 함) 후 UdpHello 재바인딩. 전체 스냅샷이 30Hz로 흐르므로 상태 재동기화는 자동.
  - 한계(정직성) ①: 이전 incarnation에서 미확인된 이벤트는 소멸한다 — exactly-once 보장은 **연결 incarnation 단위**. 교전 이력 전체가 필요한 화면(AAR)은 P4의 TCP 재동기화 영역.
  - 한계(정직성) ②: **동일 토큰 UDP 재바인딩 레이스** — 죽지 않은 구 프로세스가 같은 토큰으로 UdpHello를 계속 보내면 바인딩이 구/신 주소 사이를 오갈 수 있다(최후 hello 승리, 자가 치유적이나 그동안 스냅샷이 분산). 토큰 소지 = 좌석 소유라는 LAN 신뢰 모델의 의도된 귀결이며, 바인딩을 transport incarnation에 결속하는 강화는 P4 백로그로 기록.
- **미접속 핸드셰이크 타임아웃 5초**, UDP-silent + 미확인 이벤트 10초(peer timeout) → UDP 바인딩 해제(논리 세션·역할은 보존).

## 7. 보안 경계 (기획서 §6 — 의도적 범위 제한)

LAN 신뢰 환경 전제. UdpHello의 64-bit 무작위 토큰이 UDP 바인딩의 유일한 인증이며, 토큰 추측·스니핑에 의한 세션 탈취 표면이 존재함을 인지한다. 암호화·HMAC은 범위 외로 선언하고, 실무라면 DTLS 또는 패킷 MAC이 들어갈 위치(헤더 뒤)를 명시해 둔다.

## 8. 불량환경 검증 (P3a 게이트)

| 계층 | 방법 | 검증 내용 |
|---|---|---|
| 순수 로직 | `tests/reliable_test.cpp` — 가상 시계 + 시드 고정 카오스 링크: 유실 0~35% × 중복 × 지터(재정렬) × 시드 스윕 | reliable **정확히 1회** 전달, in-flight 드레인, msg_id 65536 wrap, 쓰레기/절단 데이터그램 무해성, RTT 추정 |
| 실소켓 | `tools/netproxy` (자체 UDP 카오스 프록시 — dnctl/netem 대비 시드 재현성, 기획서 §10.3) + `tests/protocol_integration_test.cpp` | 10% 유실 + 5% 중복 + 지터 환경에서 스냅샷 흐름 유지 + 이벤트 무결성, UdpHello 재시도 생존 |
| 전체 조립 | SimServer + DummyClient E2E | 역할 협상/배타, 일제사→발사·판정 이벤트 전 콘솔 동일 multiset, 토큰 재접속, 입력 저널 기록 |

발견 사례 기록: 미수신 상태의 `ack=0` 가짜 확인응답(§3.1) — property 스윕이 시드 재현으로 격리, ack 유효 플래그로 수정. "퍼징을 채널 사용처 구현 전에 통과"시키는 게이트 순서(기획서 §11 리스크 표)가 의도대로 작동한 증거.

## 9. 스레딩과 배압 (P3b 조립)

- 기획서 §4.6의 2-스레드 구조 이행: I/O 스레드(이벤트 루프 + 프로토콜/세션) ↔ 시뮬 스레드(60Hz 고정 틱 + 입력 저널). 유일한 공유 상태는 **lock-free SPSC 큐 2개**(`core/spsc_queue.h` — Lamport + 캐시드 인덱스, 64B 라인 분리). "push 후 wakeup" 규약(네트워크 설계서 §4.3)으로 유실 없는 통지.
- 시뮬 스레드는 절대 블록하지 않는다: sim→net 큐가 가득 차면 해당 틱 출력을 **드롭+카운트**(틱 예산 보호가 우선; 그 시점이면 I/O 스레드가 수 초째 정지한 상태로 클라이언트들은 어차피 timeout 경로).
- 스냅샷·이벤트 페이로드는 **세션 무관하게 1회 인코딩** 후 세션별 엔드포인트가 자기 헤더로 포장(복제 비용은 ≤8 클라 × ~2KB × 30Hz — 측정 가치 없음).

## 9b. 서버 리플레이 모드 (P4, §5.8 "관측석 복기")

`seashield_server --scenario S --replay journal` — 파싱된 저널이 입력 큐를 **대체**한다: sim 스레드가 기록 틱에 명령을 적용하고, 라이브 FireRequest는 거부(reject 카운트), 스냅샷·이벤트는 평소처럼 송출되어 관측석이 교전을 복기한다. 결정론(§5.1) 덕분에 트랙 수명주기까지 원본과 동일하게 재현된다(determinism_test가 sim 수준에서, E2E가 와이어 수준에서 고정).

## 10. 부하 테스트 1차 (P3 DoD)

측정 환경: Apple Silicon macOS, 루프백, RelWithDebInfo 아님(Debug 빌드 — 보수적 수치). `seashield_server --scenario scenarios/crossing-asm.scn` + `dummyclient --clients 8 --duration 10 --salvo 16`.

| 항목 | 측정값 | 목표(기획서 §10.3) |
|---|---|---|
| 클라이언트 8개 동시 스냅샷 수신 | **8/8 전원 300/300 틱** (30Hz × 10s, 손실 0) | DoD: "더미 클라 N개 스냅샷 수신" |
| 발사 이벤트 (16발 × 8 클라) | 전원 정확히 1회, 중복 0 | exactly-once |
| 시뮬 틱 처리시간 (17 엔티티) | **avg 43µs / max 156µs / 8ms 초과 0/622** | p99 < 8ms |

스트레스 조건(엔티티 500, 의도적 저속 클라이언트, p99 정밀 측정·대역폭 계측)은 P6 성능 보고서의 범위 — 본 절은 1차 스모크 수치다. 측정 코드는 `SimServerStats`의 tick_busy_* 카운터(매 정지 시 로그).

## 11. 버전 이력

| 버전 | 시점 | 변경 |
|---|---|---|
| 1 | P3 | 최초: 핸드셰이크/스냅샷/이벤트/reliable 채널 |
| 2 | P4 | kTrack 엔티티(state·σ flags), EventKind 4/5, rocket_id→subject_id 개명(와이어 동일), FireRequest.track_id(+az/el 오프셋 재해석), kFireSolution 정의. **범프 근거**: strict 디코더는 미지 enum을 조용히 거부하므로 v1 클라이언트는 "트랙만 안 보이는 반쯤 동작"이 된다 — 버전 불일치의 시끄러운 실패(TCP reject, UDP 전량 폐기)로 바꾼다. 단일 레포 배포라 혼용 요구 없음 |
| 3 | P5 | ServerWelcome += 지상풍 벡터·강우·거스트 σ(f32×4 — 비주얼 구동), FireSolution 주기 송신 개시(기본 2Hz, §4 카탈로그의 근거 참조), coasting staleness 게이트(`track_max_coast_scans`). 범프 근거는 v2와 동일(strict 디코더 + 단일 레포) |
| 4 | P6 | 델타 압축(kSnapshotAck 22 / kSnapshotDelta 23, §5b), ServerWelcome.udp_nonce + UdpHello.nonce(incarnation 결속, §5c), kEventBacklog 5(TCP 캐치업, §5c). 범프 근거 동일 |

## 12. 한계와 확장 (정직성 절)

- **델타 압축 미적용**: 1차는 전체 스냅샷. P6에서 클라이언트별 ack 기준 차분과 대역폭 정량 비교(기획서 §6).
- **혼잡 제어 없음**: LAN ≤8 클라이언트 요구 규모에서 불필요(기획서 §11). reliable 채널의 유일한 흐름 제어는 in-flight 상한.
- **이벤트의 incarnation 경계**(§6): 재접속 전후 exactly-once는 연결 단위. AAR 수준의 전체 이력은 P4에서 TCP로.
- **스냅샷 우선순위 없음**: 엔티티 폭증 시 위협도 기반 송신은 백로그(기획서 §6 관심 관리 절).
- 클라이언트 보간 버퍼(기획서 §4.7)는 표현 계층 — 구현은 `client/core/`(std-only: 스냅샷 재조립·보간·외삽 상한·트랙 스냅 규칙)로 분리되어 헤드리스 테스트로 고정되며, UE5 모듈은 이를 그대로 컴파일한다. 더미 클라이언트는 수신 통계만 검증한다.

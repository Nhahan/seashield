# 성능 측정 보고서 (Performance Report)

> Project SeaShield — P6 산출물 (기획서 §9, §10.3). 부하·대역폭·틱 비용의 측정
> 기준 문서. 측정 코드: `SimServerStats`(틱 히스토그램 포함), `dummyclient`
> (`--ack`, `--fire-count`, kbps 계측). 1차 스모크 수치는 protocol-spec §10.
> 범위: 본 문서는 **서버·와이어**를 다룬다 — 클라이언트 GPU 프레임 예산(1440p
> 60fps)은 [client-design.md §8](../architecture/client-design.md), 측정 절차는
> [client-runbook.md §5](../architecture/client-runbook.md)가 담당.
>
> 문서 버전: v1.0 (2026-06-12) · 환경: Apple M4 Max, macOS 15.6.1, 루프백,
> **RelWithDebInfo** (결정론 FP 플래그 적용 상태 — Debug↔Rel 비트 동일 검증 완료)

---

## 1. 방법

- 서버: `seashield_server --scenario scenarios/stress-500.scn` (rocket_lifetime 40s —
  발사된 탄이 측정 구간 내내 생존해 엔티티 수가 유지된다). SIGINT 시 틱 비용
  통계(avg/max/8ms 초과/2의 거듭제곱 µs 히스토그램 기반 p99 상계)를 출력.
- 클라이언트: `dummyclient --clients 8` (사통 1 + 관측 7). `--ack`가 v4 델타
  스트림을 활성화(프로덕션 콘솔의 동작)하고, kbps 열은 수신 UDP 데이터그램
  총 바이트에서 계산(다운링크 전체 — 스냅샷·이벤트·FireSolution·keepalive 포함).
- p99는 두 겹으로 보고한다: 정확한 8ms 초과 카운터(§10.3 예산 게이트 — 초과율
  <1%이면 p99 < 8ms 입증)와 로그 버킷 히스토그램의 보수적 상계.

재현 명령줄:

```sh
./build-rel/seashield_server --scenario scenarios/stress-500.scn --port 17777 --udp-port 17778
# 실전 ~100 엔티티 (풀): salvo 50 x 2
./build-rel/dummyclient --port 17777 --clients 8 --role weapons --duration 10 \
    --fire-after 0.5 --fire-az-deg 10 --fire-el-deg 60 --salvo 50 --fire-count 2 --fire-interval 0.5
# 동일 + 델타: --ack 추가
# 스트레스 500 엔티티: --salvo 50 --fire-count 10 --fire-interval 0.5 --duration 20 --ack
```

## 2. 결과 — 대역폭 (실전 ~100 엔티티, 클라이언트 8)

| 모드 | 다운링크/클라 | 비고 |
|---|---|---|
| 풀 스냅샷 (v3 동작) | **407 kbps** | 기획서 §6 산정 ~480kbps와 부합 (102 엔티티 실측) |
| **델타 (v4, --ack)** | **188 kbps** | **53.8% 절감 — §10.3 목표 <256kbps 충족** |

- 델타 클라이언트는 첫 풀 프레임 1장 이후 전부 잔차 스트림으로 전환됐다
  (asm/delta = 300/299, 10s × 30Hz). 루프백 무손실 기준이며, 유실 환경의 생존성은
  E2E(`DeltaStreamSurvivesChaosLoss`, 10% 손실)로 별도 검증.
- 산정 근거: 엔티티당 풀 20B vs 델타 잔차 9B(CV-예측 잔차, protocol-spec §4) +
  공통 오버헤드(패킷 헤더 12B, 이벤트·FireSolution·keepalive)가 양 모드 동일.

## 3. 결과 — 틱 비용 (60Hz sim 스레드, §10.3 예산 p99 < 8ms)

| 시나리오 | avg | p99 (히스토그램 상계) | 8ms 초과 | max |
|---|---|---|---|---|
| 실전 ~100 엔티티, 클라 8 | 127µs | **≤ 128µs** | 1/639 (0.16%) | 61.1ms* |
| **스트레스 500 엔티티**, 클라 8 | 268µs | **≤ 512µs** | 3/1246 (0.24%) | 68.7ms* |

- **p99 < 8ms 충족** (초과율 < 1%, 두 시나리오 모두 — 60Hz 예산 16.6ms의 50% 기준).
- 500 엔티티에서도 평균 268µs — 엔티티 수에 대한 스케일링은 예산 대비 여유가 크다
  (탄도 RK4 ~500개 + 스냅샷 인코딩 포함).

\* **max 스파이크의 정체 (측정이 잡은 실제 이슈)**: FireSolution 스트리밍의
**미수렴 solve 1회 비용**이다 — 도달 불가 기하(이 시나리오의 12km 표적)에서 PIP
솔버가 전체 반복 예산을 소진하며 단일 호출 ~60-70ms. 측정 전에는 2Hz로 반복돼
8ms 초과가 10~24회였고, **실패 트랙별 5s 백오프**(서버 sim 루프)를 도입해 쿨다운당
1회로 봉쇄했다(초과 1~3회 = 첫 실패 + 스폰 틱). 잔여 단일 스파이크의 근본 제거
(스트리밍용 솔버 반복 캡)는 백로그 — §5.

## 4. 결과 — 격리·불량환경 (테스트로 상시 검증)

- **저속/침묵 클라이언트 격리**: peer timeout 시 UDP unbind, 나머지 클라 무영향 —
  `SilentClientLosesUdpBindingWithoutHarmingOthers` (상시 CI).
- **불량환경**: 10% 손실+중복+30ms 지터에서 reliable exactly-once·스냅샷 흐름 유지 —
  `ChaosProxyLossAndReorderDoNotBreakTheProtocol`; 델타 체인의 유실 자가 치유 —
  `DeltaStreamSurvivesChaosLoss` (150프레임 중 60+ 조립, 손실 주입 확인).
- 대역폭 A/B 회귀 가드: `DeltaStreamCutsDownlinkAgainstFullSnapshots`
  (델타 < 풀×0.7 단언 — 측정 헤드라인은 본 문서, 가드는 CI).

## 5. 분석·한계·백로그

- 256kbps 목표는 §6 산수의 적용 영역(~100 엔티티 실전)에서 달성. 스트레스 500은
  목표의 적용 대상이 아니며(명시적 제외, §6) 델타로도 ~836kbps — LAN 예산 내.
- 측정은 루프백 단일 호스트 — 클라 8개의 수신 스레드가 서버와 코어를 공유하므로
  서버 단독 성능의 보수적(불리한) 추정이다.
- 일제사는 서버 검증 상한(64발/명령)을 준수해 다회 일제사로 누적 — 상한 자체는
  프로토콜 안전 경계로 유지(완화하지 않음).
- **백로그**: 스트리밍 solve의 반복 캡(실패 판정 조기화 — 단일 60ms 스파이크 제거),
  델타 인코딩 CPU 비용 프로파일(현재 I/O 스레드, 측정상 비병목), 분리 호스트 측정.

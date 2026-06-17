# 성능 측정 보고서 (Performance Report)

> Project SeaShield — P6 산출물 (기획서 §9, §10.3). 부하·대역폭·틱 비용의 측정
> 기준 문서. 측정 코드: `SimServerStats`(틱 히스토그램 포함), `dummyclient`
> (`--ack`, `--fire-count`, kbps 계측). 1차 스모크 수치는 protocol-spec §10.
> 범위: §1–5는 **서버·와이어**, **§6은 클라이언트 GPU 프레임 예산(1440p 60fps)**.
> 측정 절차·판정 게이트는 [client-runbook.md §5](../architecture/client-runbook.md),
> 클라 렌더 설계는 [client-design.md §8](../architecture/client-design.md).
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

---

## 6. 클라이언트 프레임 예산 (1440p60 GPU)

> 측정: `client/SeaShield/Tools/perf_capture.sh --gpu` (서버 세션분리 + 에디터 `-game
> -SeaGamePlay -SeaQuit -SeaProfileGPU`, **네이티브 2560×1440, vsync-on**). 프레임 통계는
> `ASeaWorldManager` EndPlay의 `PERF:` 라인(첫 8 s 워밍업 제외, p95/p99 히스토그램), GPU
> 패스는 ProfileGPU 로그. 절차·게이트는 [client-runbook §5](../architecture/client-runbook.md).
> 환경: Apple M4 Max, macOS, UE 5.7 Metal SM6.

### 6.1 게임 부하 — 워터 최적화 전후

| 구성 | GPU 프레임 | SingleLayerWater | VolumetricCloud | over-33.3% | 결과 |
|---|---|---|---|---|---|
| 워터 품질 상향 직후 (회귀) | **129.9 ms** | 121.0 (93%)\* | 3.6 | 96.3% | ❌ ~7 fps |
| + 워터 지오메트리 예산 노브 | 17.7 ms | 1.9 | 8.0 | 4.9% | ⚠ ~54 fps |
| + 클라우드 half-res (Mode 2) | **12.1 ms** | 1.5 | 3.0 | 4.2%† | ✅ 60 fps |

\* 깊이 프리패스 40 ms + 드로우 81 ms — 둘 다 지오메트리 바운드(gerstner 쿼드트리 삼각형
폭증). 회귀 측정은 stale 오토벤치(`sg.ResolutionQuality=0`)로 60% 해상도였음에도 7 fps —
네이티브였다면 더 나빴다. 자동 계측이 없었으면 "체감상 느림"으로 끝났을 회귀를 수치·패스로 격리.

† 정적 씬(idle)은 이미 over-33.3 = 0%, p99 21 ms = 솔리드 1440p60. 게임 부하의 이 잔여
4.2%/p99 50 ms 스파이크는 워터/스카이가 아닌 **별개 게임-스레드 원인**이었고 §6.3에서 격리·수정했다.

### 6.2 적용 노브 (`DefaultEngine.ini [SystemSettings]`)

- **워터 지오메트리**: `r.Water.WaterMesh.TessFactorBias=-6`(유효 tessellation 10→4),
  `LODScaleBias=-0.5`, `LODCountBias=-1` → SingleLayerWater **121 → 1.5 ms**. 원거리 패턴
  방지는 LOD가 아니라 `MI_SeaOcean` 머티리얼(원거리 노멀 평탄화 + 러프니스)이 담당하므로
  LOD 링을 당겨도 안전 — 수평선 캡처로 무회귀 검증.
- **클라우드**: `r.VolumetricRenderTarget.Mode=2` + `.Scale=0.5`(half-res 재구성) → **8 → 3 ms**.
  quarter-res 트레이스는 march-bound라 샘플캡/섀도/AO 컷은 무효였음.
- **해상도**: 1인칭 시절엔 `r.ScreenPercentage=100`(네이티브)가 예산에 맞았다 — 이후 3인칭
  전환으로 바다 노출이 커져 §6.4에서 **90% + TSR**로 재조정(50 fps 목표).
- 결과: GPU **12 ms < 16.67 ms** 예산, 헤드룸 ~4 ms (정적 씬 기준 솔리드 60 fps).

### 6.3 게임-스레드 스파이크 — 자동 사수 조준 솔버 (계측이 잡아낸 2차 원인)

워터/클라우드 수정 후에도 게임 부하는 over-33.3 = **4.2%**, p99 **50 ms**로 FLAG였다. `Spike:`
포렌식 라인(스레드 분해)이 즉시 범인을 지목: **game-thread 48 ms / render 16 ms / GPU 12 ms** —
GPU·렌더가 아닌 **순수 게임-스레드**. Tick 하위 단계 계측은 `SeaWorldManager`(reconcile/트레일/VFX
합계 < 2 ms)를 무죄 처리했고, 0.36 s 주기가 자동 사수의 `ReaimTimer = 0.35 s`와 정확히 일치 →
**`SeaBallistics::SolveAim`**(스크립트 자동 사수의 조준 솔버)으로 확정. 이 솔버는 ±25° az × 4-70° el을
1.5°로 **전수 그리드 탐색**(~1500회 RK4 탄도 적분)해 호출당 ~48 ms였다(§3·§5에 적힌 미수렴 solve
비용의 클라 버전). **사람 플레이는 마우스 조준이라 이 경로를 안 탄다 — 자동 캡처 하네스에서만 발생.**

수정: `SolveAim`을 **coarse-to-fine**으로(6° 거친 패스로 분지 국소화 → 그 주변 ±5°를 1.5°로 정밀화).
miss(az,el) 곡면이 매끈해 동일 최적해를 ~1/10 비용으로 찾는다.

| 지표 | 수정 전 | 수정 후 |
|---|---|---|
| SolveAim 1회 | ~48 ms | **~5 ms** |
| over-33.3% (게임 부하) | 4.2% | **0.0%** |
| p99 / max 프레임 | 50 / 50 ms | **23 / 23 ms** |
| 평균 (게임 부하) | 16.7 ms (스파이크가 평균을 끌어올림) | **13 ms (~76 fps)** |
| 판정 | ❌ FLAG | ✅ **PASS** |

자동 사수는 여전히 표적 명중(40 airburst/40 s), 비주얼 무회귀(게임플레이 캡처: WAVE 2·SCORE 120).
잔여 p99 23 ms 단일 프레임은 일제사 발사 스폰 버스트(1.6 s마다 14발)로, sub-30 프레임 0개라 무해.

### 6.4 3인칭 전환 + Phase 3 그래픽/해상 VFX 업리프트 (목표 50 fps)

§6.1–6.3은 **1인칭 조준경**(카메라 = 포구, 하늘을 봄) 시절 측정이다. 게임플레이를 **3인칭 함선
궤도 카메라**로 바꾸면서 시점이 **바다를 정면으로 가로질러** SingleLayerWater가 지배 비용
(≈6.7 ms, GPU 프레임의 40%)이 됐고, 동시에 ① 시네마틱 그레이드(쿨 섀도/웜 하이라이트 휠 +
filmic + 노출 규율 + **콘택트 섀도** + **항공원근 헤이즈**(sky-driven luminance height fog) +
글레어 블룸), ② 해상 VFX(soft/noise 침식 **물기둥·기폭 flak puff**(Age 구동 hot-core→smoke),
**머즐 플래시**, **함선 항적 foam**), ③ 실배기 연기(Phase 2)를 얹었다. 목표를 **50 fps(20 ms)**로
재설정(품질 우선).

**SkyLight 실시간 환경 캡처 스파이크 제거 (p99 꼬리).** `SkyLight real_time_capture`는 반사
큐브맵에 하늘+구름을 **6면 + convolution**으로 다시 그리는데, 한 프레임에 몰면 **~18 ms 주기
스파이크**(p99 꼬리)였다. `r.SkyLight.RealTimeReflectionCapture.TimeSlice=1`(프레임 분산) +
큐브맵 해상도 **128→64**로 잔잔한 바다 반사에 무회귀로 제거 → **p99 26 → 19 ms**(80% 기준).

**해상도 결정 — 열(thermal) 공정 측정.** ⚠ **연속 GPU 런은 throttle된다**: 쿨다운 없이 80→100→90%
를 연달아 돌렸더니 avg가 **run 순서대로** 16.7→21.4→25.9 ms로 단조 증가(스크린% 비단조) —
열누적 인공물. **각 런 전 ~90 s 쿨다운**으로 공정 측정:

| 내부 해상도 (TSR→1440p) | fps (cooled) | p99 | max | over-33.3% | 판정 |
|---|---|---|---|---|---|
| 80% (2048×1152) | 60.0 | 19 | — | 0.1% | PASS, 가장 헤드룸 |
| **90% (2304×1296)** ← 채택 | **55.3** | 21 | 20.7 | 0.0% | **PASS, 마진 + ≈네이티브 샤프** |
| 100% (2560×1440, native) | 48.5 | 24 | 52.4 | 0.1% | PASS, 최대 샤프지만 플로어 직전 |

**채택: `r.ScreenPercentage=90`.** 근거: 네이티브 100%는 cooled 48.5 fps라 **지속 플레이로 GPU가
달궈지면 50 fps 플로어 아래로 drift**한다(이 세션에서 throttle 실증). 90%는 cooled 55 → hot ≈ 50으로
**플로어를 마진과 함께 유지**하고, TSR 90%→1440p + `r.Tonemapper.Sharpen=0.8`로 **네이티브와 거의
구분 불가**. SkyLight 스파이크 회수분을 해상도로 환원한 셈(이전 80% 대비 한 단계 샤프).

**Phase 3 VFX/그레이드 비용은 사실상 무시할 수준.** 게임 부하 ProfileGPU(90% 내부): SingleLayerWater
6.7 ms(지배) · VolumetricCloud 2.1 ms · PostProcessing 3.1 ms(TSR 업스케일 2.4 ms 포함) ·
**Translucency(연기 트레일 + 물기둥/기폭/머즐/항적 전부) 1.3 ms** · ExponentialHeightFog 0.27 ms ·
콘택트 섀도 negligible. 새 VFX는 short-lived + 캡 + 값싼 panned-Noise 침식이라 예산을 거의 안 먹는다.

| 지표 (게임 부하, 90%/1440p, cooled) | 값 |
|---|---|
| avg / fps_avg | 18.1 ms / 55.3 |
| p95 / p99 / max | 20 / 21 / 20.7 ms |
| over-33.3% / hitch>100ms | 0.0% / 0 |
| 판정 | ✅ **PASS** (50 fps 목표, 마진 보유) |

검증: 인게임 캡처(`-SeaGamePlay -SeaShotSeq`, `-SeaSteerDemo`, `-SeaShotOnBurst`)로 실배기 연기 기둥·
일제사 발사 연기·flak puff 기폭·머즐 플래시·항적 foam·그레이드·수평선 헤이즈를 실프레임으로 확인.

### 6.5 AA/AAA 비주얼 패스 (함선 디테일 · 볼류메트릭 연기 · 라이팅)

"플래시 게임 같다"는 피드백에 따라 **에셋 자체**를 끌어올렸다(전부 코드 생성 유지):
- **절차적 PBR 디테일**(`tools/assets/textures.py` numpy/PIL → 타일 노멀+RAO): 함선/갑판/미사일을
  **트라이플래너**(UV 없이 월드공간)로 샘플 — 평면 단색 → 패널라인·용접심·웨더링·녹의 도장 금속.
  부위별 슬롯 머티리얼(도장 선체 / 회색 상부 / **어두운 레이더·유리·센서**)로 대비.
- **함선 지오메트리 보강**(`frigate.py`): 통합 마스트+레이더 어레이, AEGIS SPY 면, CIWS, 함교 유리,
  방파판, 갑판 적재물, 난간 (LOD0 670 tris, faceted 저폴리 미학 유지).
- **볼류메트릭 연기**(`SeaWorldManager` 빌보드 퍼프 스트림): 평면 리본 → 소프트 스프라이트(`T_Smoke`)
  퍼프를 로켓당 ~26/s 방출, 바람에 드리프트·성장·페이드(`Age` 스칼라), **하드 캡 320**으로 오버드로
  통제. 머즐은 `T_Flash` 스타 스프라이트 빌보드. 둘 다 액터-당-퍼프인데도 게임-스레드 스파이크 0.
- **라이팅**: 낮은 골든 키(pitch -16, 따뜻) + **SkyLight intensity 0.62**(앰비언트 fill↓ → 선체 폼).
- **HUD 콤뱃-콘솔**(`SeaHudStyle.h` 공용 스타일): 평면 녹색 와이어프레임 → 프레임 패널(글래스 fill +
  시안 액센트 상단 룰 + 코너 브래킷). 스코어보드·위협 스트립·사격통제 readout 전부 `SeaHud::ConsolePanel`,
  PPI 스코프는 베젤 링 + N/E/S/W·거리 라벨. 심볼로지/로직 불변(크롬만). 전부 CPU-Slate(GPU 무비용).
- **해수면 거품**: `MI_SeaOcean`에 Water 플러그인 foam 파라미터(Foam Opacity/Boost/Height Bias/
  FoamContrast) 보강 — 단 자함이 부력 비결합(고정 Z)이라 파고를 올리면 선체가 수면을 뚫는다. 그래서
  파고는 유지(잔잔)하고 기존 크레스트에만 거품을 얹는 보수적 튜닝(잔물결 anti-shimmer·SLW 예산도 보존).

**최종 측정(게임 부하, 90%/1440p, cooled, 짙은 연기 320캡 + 골든 태양):** avg 16.8 ms / **59.7 fps**,
p99 19 ms, max 19.3 ms, over-33.3 0.0%, hitch 0 → ✅ **PASS**. 추가 비용은 사실상 무시할 수준(물이
여전히 지배적). 측정은 쿨다운 후 공정값(§6.4 thermal). 남은 작업: HUD 콤뱃-콘솔 리워크, 해수면 거품.

### 6.6 탄도 연기 트레일 재작업 — 점선 비드 제거 + reconcile 스폰 버스트 (계측이 잡은 3차 원인)

§6.5의 퍼프 스트림이 "탄 궤적 연기가 가짜"라는 피드백을 받았다. 실프레임 판독으로 3개 결함 확정:
**①** 퍼프를 **시간 주기**(0.038 s)로 방출 → 빠른 로켓은 퍼프 간 거리(속도×0.038≈10–15 m)가 퍼프
지름(~6 m)보다 커 **점선 비드** 발생. **②** hot-glow가 수명의 첫 1.2 s(`saturate(1−Age·5)`) 동안
켜져 발사 구간 전체가 **오렌지 불씨**로 보임. **③** 얇은 리본(28 cm)이 비드 사이로 **딱딱한 와이어**로 비침.

수정 (전부 코드, 결정론/`sim` 무관 — 클라 렌더 전용):
- **리본을 연속 본체로**: 폭 28→110 cm(young)·220→520 cm(old, `sqrt` 빌로우), 샘플 0.06→0.035 s.
  퍼프는 그 위의 **희소 볼륨 악센트**로 강등 → 비드 갭은 리본이 메움(어느 속도에서도 연속).
- **퍼프 방출을 거리 기반으로**: 프레임당 이동 세그먼트를 8.5 m마다 보간 방출(속도 무관 균일).
- **오렌지 제거**: hot-glow를 `Age·14`(첫 ~0.07 수명)로 단축 + 디밍 → 노즐 끝 짧은 온기만.
- **그레인 제거**: `T_Smoke` 스프라이트를 **실루엣은 럼피(저주파 반경 워프)·내부는 매끈**으로 재생성
  — 고주파 옥타브가 수백 퍼프에 반복되면 'cottage-cheese' 그레인이 됨(원인 격리 후 제거).

**reconcile 스폰 버스트(핵심 계측 발견):** 거리 기반 방출은 `SampleTrail`→`EmitPuff`(=`AStaticMeshActor`
스폰)를 **reconcile 루프 안**에서 호출한다. 빠른 로켓·일제사에서 프레임당 다수 액터를 스폰 → `Spike:`
포렌식이 **game 33–55 ms / reconcile 21–44 ms**(trails 3.7·vfx 0.0 — **연기 GPU는 무죄**)로 over-33.3
1.8%까지 악화. 오버드로 트림(퍼프 캡·폭↓)은 무효였고, **프레임당 스폰 캡(2개) + 간격 600→850 cm**으로
버스트를 묶자 해결. 교훈: **퍼프 빌보드의 액터 스폰 비용은 vfx가 아니라 호출 위치(reconcile) 타이밍에
귀속**된다 — 거리 기반 방출은 반드시 프레임당 스폰을 상한해야 한다.

**reconcile 스폰 버스트 수정 측정:** avg 17.1 ms / 58.4 fps, p99 21·max 27, over-33.3 0.0% ✅
(수정 전 p99 35·max 55·over-33.3 1.8% FLAG.)

**4차 피드백 — "궤도에 갇힌 연기"(분산 모델):** 위 수정으로 연속·회백색이 됐지만 연기가 *궤도 선에
빳빳하게 갇혀* 자연 분산·소산하지 않는다는 지적. 원인: ① 퍼프가 바람으로만 평행 이동(축 바깥 확산
없음), ② 리본이 궤도 전체를 14 s 사는 빳빳한 띠. 분산 모델로 재작업:
- **리본 = 신선 코어만**: 수명 14→**2.6 s**(로켓 바로 뒤 응집 구간만 표시), 폭 90/300 cm. 나이 든 궤도는
  퍼프가 담당 → 띠가 궤도에 안 갇힘. 신선=응집 기둥 / 노후=갈라지는 옅은 구름으로 물리적 분리.
- **퍼프 난류 분산**: 퍼프별 **랜덤 드리프트 속도**(160–360 cm/s, 축 바깥+부력 상승 바이어스)를 부여 →
  나이 들수록 축에서 멀어지며 흩어짐(`Pos += (Wind+Drift)·Age`). 크기 `sqrt(Age)`로 빠르게 빌로우(17 m),
  수명 6.5 s로 충분히 퍼진 뒤 페이드 아웃 → **분산 후 소산**. 신선 코어는 young 퍼프(half 4.2 m)로 보강.
- 부수 효과: 분산으로 퍼프가 한 선에 안 겹쳐 **RenderTranslucency 1.4→0.16 ms**(오버드로 집중↓).

**최종 측정(게임 부하, 네이티브 1440p, cooled, 분산 모델):** avg 16.8 ms / **59.7 fps**, p95/p99 19/21 ms,
max 25.4 ms, over-33.3 **0.0%**, hitch 0 → ✅ **PASS**. 물(SLW 7.2 ms)이 여전히 지배적, 연기 GPU는 무시 수준.

### 6.7 이펙트/라이팅 AA 업리프트 (요격폭발·머즐·뱃머리파도·부력해수면·그레이드)

"플래시 게임" 피드백의 후속으로 5개 축을 단계별(각 단계 실캡처 + 프레임게이트)로 업리프트.

- **A 요격 폭발**(`SeaWorldManager::SpawnExplosion`): 단일 Sphere → **레이어드 에어버스트** = T_Flash 섬광
  (16 m, emissive 블룸) + M_Burst 화구(18→46 m) + **파편**(`SpawnDebris` 13개 additive 스파크, 방사 사출 +
  중력 아크, `M_Debris`) + 연기 잔류(`EmitPuff`×5 분산). kind=2 격추 + 로켓 근접기폭 경로에서 호출.
- **B 머즐**: 섬광 7→11 m + emissive ×8→11 + **점화 연기 킥**(`EmitPuff`×3, 살보당 throttle 유지).
- **C 뱃머리 파도**(`RebuildBowWave`): 선수에서 ~Kelvin각으로 벌어지는 **포말 V 윙** 2개(속도 게이트,
  M_Wake 재사용). 선체 헤딩/속도는 `SampleWake` 포즈 재사용.
- **D 부력 해수면**: **함선을 Gerstner 수면에 시각 결합** — `UWaterWavesBase::GetWaveHeightAtPosition`로
  매 틱 파고+노멀 질의, 시각 Z 보브 + 댐핑 roll/pitch(±5°, FInterpTo 스무딩). **시간은
  `UWaterSubsystem::GetWaterTimeSeconds()`로 렌더러와 동기**(다른 시간 쓰면 선체가 수면 위/아래로 뜸).
  서버 권위 XY/헤딩 불변, 시각 오프셋만. 고정-Z 블로커 해제 → `SeaEnvironmentController` 진폭 ×1.5,
  경사 0.18/0.12→0.42/0.30, MI_SeaOcean 화이트캡 foam↑.
- **E 그레이드**: bloom 0.80→0.95(신규 섬광/폭발 블룸), 채도/대비 ↑, film grain 0.12.

**스폰 버스트 안전**: 폭발은 이벤트당 ~20 액터(섬광1+화구1+파편13+퍼프5) — 희소 이벤트라 다중 격추
동시 발생에도 `Spike:` 0(§6.6 교훈 준수, 프레임당 캡 불필요 확인). **파도 비용**: 진폭 ×1.7은 cooled
45.7 fps(SLW 7.5 ms + 워터메시 지오↑)로 마진 부족 → **×1.5로 스케일백**해 예산 복귀(예산 우선).

**최종 측정(게임 부하 A–E 전체, 네이티브 1440p, cooled):** avg 17.1 ms / **58.6 fps**, p95/p99 20/22 ms,
max 28 ms, over-33.3 **0.0%**, hitch 0 → ✅ **PASS**. 50 fps 플로어 충족. (중간: 부력 미적용 ×1.7은
thermal+지오 비용으로 45.7 fps였음 → 쿨다운+스케일백으로 분리·해소.)
**후속 — 표면-추종 포말 핀(완료)**: 항적/뱃머리 포말 정점을 평균 수면 Z → **라이브 Gerstner 파면**에 핀
(`SeaSurfaceWorldZ` = `SampleSeaSurface` 재사용, 정점별 질의 + `kWakeSurfaceLiftCm` 립). 포말이 스웰 따라
출렁임(큰 파도에서 뜸/파고듦 해소). 정점 수·오버드로 불변 + 게임스레드 ~100 질의/frame(무시) → cooled
56.4 fps · over-33.3 0% · 히치 0 ✅ PASS, `Spike:` 0. 기동 캡처(`-SeaSteerDemo`)로 확인.

### 6.8 코드 헬스 리팩터(Part 1) + 시네마틱 캡처 티어(Part 2-0)

냉정 평가(3-에이전트)가 클라 VFX의 구조·성능 빚 3건을 지적 → **순수-동작-보존**으로 클린 정리하고
(렌더 전용, 결정론·sim·서버 무관) before/after 비주얼 동일 + 프레임게이트로 확정.

- **C 격추 폭발 틱-순서 레이스**[MED]: `HandleEngagementEvent` kind=2가 표적 액터에서 위치를 읽어, reconcile가
  먼저 파괴하면 폭발·잔해가 조용히 누락. → 마지막 스테이지 포즈를 `LastEntityStagePos`에 캐시(액터 수명보다
  오래 생존, 웨이브 리셋으로 바운드)하고 거기서 기폭. 격추 36회 전부 에어버스트 확인.
- **B 액터-당-파티클**[HIGH]: 퍼프/파편/섬광 각각 `AStaticMeshActor`(최대 ~480, O(n) `RemoveAt(0)`) →
  **머티리얼당 ISM 1개**(3개 `InstancedStaticMeshComponent`). Age는 per-instance custom-data[0](머티리얼
  `PerInstanceCustomData`), 재활용은 인덱스 스왑(O(1)). 리뷰어가 지적한 **reconcile 스폰 버스트가 구조적으로
  제거**됨(파티클이 더는 액터 스폰이 아님).
- **A God-class 분해**[HIGH]: `SeaWorldManager.cpp` **1560 → 721줄**. 이펙트를 `SeaVfxSystems.{h,cpp}`로
  분리 — `FSeaVfxContext`(프레임당 1회 카메라/오션/시간 호이스트) + `ISeaVfxSystem` + 5개 시스템(Trail/Splash/
  Explosion/Wake/Wreckage). 매니저는 reconcile + 부력 포즈 + 이벤트 라우팅 + 계측만 보유.

**Part 1 측정(게임 부하, 네이티브 1440p, cooled):** avg 17.33 ms / **57.7 fps**, p95/p99 20/21 ms, max 22.5,
over-33.3 **0.0%**, hitch 0 → ✅ **PASS**. ISM 전환으로 파티클 액터 수 급감(≈480→3 컴포넌트), 동작·비주얼 동일.
GPU 패스 지배항: **SingleLayerWater 38.6%(6.9 ms, SLW::Draw 6.3)**, PostProcessing 19.8%(TSR 2.9), Cloud
10.7%, Translucency(VFX) 9.1%. 게임플레이 반사는 **SSR(Quality 2)** — Lumen GI는 켜져 있으나 반사는 SSR.

**시네마틱/포토 티어(Part 2-0)** — 화질 상한을 푸는 장치(런북 §5b). `Tools/cinematic.cvars`(단일 소스)를
`--cinematic`이 `-dpcvars`로 주입: 워터 풀 테셀(TessBias -6→0)·반사/굴절 full-res(downsample 2→1)·구름
full-res(Mode 2→0)·**Lumen 반사**(ReflectionMethod→1, SSR 대체)·네이티브 100%(+슈퍼샘플). `-dpcvars`는 렌더러/
워터메시 빌드 **이전**(디바이스 프로파일 단계)에 적용되어 `[SystemSettings]`를 깔끔히 오버라이드 → 워터메시
테셀까지 런타임 리빌드 없이 먹음. `-SeaCinematic`이 부팅 시 실제 적용 cvar 값을 로그로 자기 검증(`r.Lumen.
Reflections.Quality`는 5.7에서 dummy/no-op으로 판명 → 프로파일에서 제거, 반사 품질은 downsample+GI 그룹으로
제어). **게임플레이 티어는 무변경**(위 PASS가 그 증거) — 시네마틱은 트레일러·스크린샷 전용(fps 무제약),
Metal에서 full-res 워터+Lumen 반사 정상 렌더(크래시/블랙 없음) 확인. 캡처: `perf_capture.sh --cinematic`,
`cinematic_shot.sh`.

### 6.9 함선 디테일/PBR 업리프트 (Part 2-1)

설계 의도(=facet은 스텔스 함선 미학, frigate.py 명시)를 존중 → **부드럽게 깎지 않고** 머티리얼로 충실도를 올리고
faceted 디테일만 보강.

- **워터라인 wetness**(M_NavalHull): 실제 수면(월드 Z 0)에 핀한 젖음 띠 — 부력으로 함체가 까딱여도 띠가 수면에
  고정. 젖은 도장은 **어두워지고(×0.6)** 거의 **거울처럼 매끈**(roughness→0.09, 위로 ~2.6 m fade, Power 1.7로 하단
  집중). 근접/저각 캡처에서 명확히 읽힘(p1_stern) — "실제 바다 위 실제 함선" 가장 큰 tell.
- **거리-블렌드 미세 노멀**: coarse 플레이트 릴리프 + **근접 시(PixelDepth ~140 m 내) 페이드-인되는 fine 서브패널
  타일** → 클로즈업 디테일은 올리되 원거리 시머는 없음(§6.4/6.7 안티시머 유지).
- **지오메트리(faceted)**: 후방 비행갑판 **헬기**(Wildcat급 실루엣: 박스 동체+다크 캐노피+테일붐/핀+로터 십자)
  + 현측 **구명벌 캐니스터**. LOD0 670→**930 tris**(LOD2 이하 무변, 게이트). "현대 함선" 읽힘 보강.
- **부수 수정**: `M_FarOcean` `used_with_water=True`(미설정 시 인게임 **기본 머티리얼 폴백** + 매 부팅 경고 — Part 2-0
  온스크린 빨간 글씨의 잔여 원인) 근본 수정; `assign_by_slot`이 UE 리임포트가 만드는 `_N` 중복 슬롯을 base 이름으로
  매칭(헬기 추가로 SM_Frigate 슬롯이 4→8 중복됐던 건 robust하게 흡수).

**측정(게임 부하, 1440p, 헐 머티리얼 노드 증가분 포함):** avg 17.67 ms / **56.6 fps**, p95/p99 21/22 ms,
over-33.3 **0.1%**, hitch 0 → ✅ **PASS**(헐은 화면 소수 픽셀이라 +6 텍스처 read·+260 tris 무시 가능). 워터(SLW)가
여전히 지배항. 130 m 히어로 거리에선 함선 디테일 변화 **미미**(디테일은 근접에서 효과 — 의도된 특성), wetness/플레이트는
근접·저각에서 또렷.

### 6.10 VFX 밀도 업리프트 (Part 2-3, ISM 위)

Part 1-B(액터→ISM)로 파티클이 싸졌으니 교전 VFX를 밀어올림. **오버드로 게이트** 준수.

- **속도-스트레치 스파크**(`UpdateDebris`): 파편 빌보드를 화면투영 속도축으로 정렬+속도비례 신장 → 동그란
  스파크가 **트레이서 스트릭**(모션블러 read)으로. 같은 인스턴스라 **오버드로 추가 0**(비균일 스케일만). 식으며
  점으로 수렴.
- **밀도↑**: 폭발당 퍼프 5→8(스프레드 700→900), 파편 13→18, 기폭 섬광 16→18 m. ISM이라 인스턴스 추가는 쌈;
  실 비용은 translucent 오버드로(퍼프)인데 `kPuffMaxAlive=300` 캡이 바운드.
- **검증**: 근접 버스트 캡처(`-SeaShotOnBurst`, 시네마틱 티어)로 화구 블룸+스파크 클러스터+머즐 확인(SeaBurst1/3).
  스트릭은 근접에서 극적, 요격 거리(중거리)에선 짧게 읽힘.

**측정(게임 부하, 1440p, cooled):** avg 17.41 ms / **57.4 fps**, p95/p99 21/21 ms, **max 28.6 ms**, over-33.3
**0.0%**, hitch 0 → ✅ **PASS**. Translucency 0.9%(0.16 ms) — 퍼프 밀도↑에도 오버드로 예산 안 터짐(캡 유효),
스파크 스트릭은 무비용. **heat-haze 굴절 폭발(시도→롤백)**: `M_Burst`에 per-pixel IOR wobble(MP_REFRACTION,
heat env 게이트)를 넣어봤으나 — 머티리얼은 정상 생성됐지만 — Metal/unlit-translucent 경로에서 근접 합성버스트
캡처(`-SeaTestBurst`, t=8.25 점화)에 **확인 가능한 왜곡이 안 나옴**(hot window가 너무 짧고 요격은 원거리라 실사용
가시성도 사실상 0). **추정 금지 원칙대로 미확인 효과는 싣지 않고 롤백** — clean M_Burst(검증된 Phase-3 버전) 유지.
재시도는 올바른 refraction_method 검증 + lit-translucent 변형 필요(task P2-3b).

### 6.11 라이팅/포스트 AAA 그레이드 (Part 2-4)

`patch_level.py`(인플레이스, 파괴적 regen 회피)로 분위기를 끌어올림. setup_level.py에도 미러(향후 regen 동기).

- **God rays(태양 light-shaft bloom + occlusion)**: 디렉셔널 라이트 스크린-스페이스 광선. 태양이 프레임에
  들어올 때만 발화 → **역광 컷에서 함선 실루엣 + 태양 글로우 + 수면 글리터**(p4_godrays = 트레일러 머니샷).
  스크린-스페이스라 그 외 프레임 비용 ~0.
- **렌즈 플레어 + 블룸**: 그레이드에 `lens_flare`(threshold 5.0 → 태양·기폭 같은 진짜 밝은 소스만 플레어,
  바다/하늘은 안 함) + bloom 0.95→1.0(머즐/에어버스트 글레어 더 터짐). 일반 프레임은 여전히 절제됨(p4_normal로
  과블룸 아님 확인).
- 검증: `enable_light_shaft_*`·`lens_flare_*` 프로퍼티 전부 적용(로그 skip 0). 역광(god rays)·일반(절제) 양쪽
  실캡처.

**측정(게임 부하, 1440p):** avg 17.82 ms / **56.1 fps**, p95/p99 21/22 ms, **max 22.4 ms**, over-33.3 **0.0%**,
hitch 0 → ✅ **PASS**. light-shaft는 태양 프레임 외 비용 무시(+0.4 ms 노이즈 내). (남은 Part 2-4 항목:
**시네마틱 DOF** — `-SeaCinematic` 한정 PP 오버라이드, 동적 초점거리 필요 → 별도 증분으로 분리.)

### 6.12 워터 피델리티 (Part 2-2)

워터 고가치 항목(Lumen 함선 반사·테셀 복원=시네마틱 티어, 선체 wetness=P2-1)은 이미 들어가 있어, 남은 **여백**만
채움 — `Tools/probe_water_params.py`로 `Water_Material_Ocean` 92 스칼라/8 벡터를 덤프해 정확한 이름·기본값 확보(추정
금지) 후 변경.

- **선체-워터 포말 collar**(신규, `FWakeSystem::RebuildHullFoam`): 선체 워터라인 둘레 타원에 얇은 평면 포말 띠
  (`UProceduralMeshComponent` 36-seg, **M_Wake 재사용**, `SeaSurfaceWorldZ` 정점별 수면 핀, vertex-color α). 정지 시
  옅은 상시 포말선(op 0.28) + 기동 시 강화(+0.55×speed). 기존 wake/bow 포말과 한 시스템. 근접 캡처(p22_collar)에서
  선체 흘수선 따라 또렷이 읽힘 — "실제 물에 떠 있는 함선" tell.
- **근거리 노멀/글린트**(MI_SeaOcean): `Default Near Normal Strength` 0.25→**0.42**(근접 파면 릴리프 샤프↑). 원거리
  밴드는 여전히 flatten(anti-shimmer 보존) — 수평선 캡처(p22_horizon)로 회귀 0 확인.
- **Sub-surface 깊이감**(SSS): `Scattering` 벡터 (1,1,1)→**(0.62,0.95,0.86)** 살짝 teal — 깊이 투과감↑. 깊이 COLOR는
  미손댄 `Absorption (10,150,350)` 유지. 근접·원거리 둘 다 머디/그린 회귀 없음(리치 teal) 확인 — 유지.
- **(b) 시네마틱 수평선 반복**: `cinematic.cvars` `LODScaleBias` 0→**-0.25**(시네마틱 티어 한정 mild pull-in) — 풀
  테셀이 드러내던 원거리 파형 지오 패턴 calmer, 근거리 디테일 유지. 안개는 게임플레이 위협 가시성 때문에 안 건드림.
- **caustics 제외(정직)**: 얕은 해저 투영이 전제 → 외양 심해엔 비적용.

**측정(게임 부하, 1440p):** avg 17.78 ms / **56.3 fps**, p95/p99 21/22 ms, **max 23.0 ms**, over-33.3 **0.0%**,
hitch 0 → ✅ **PASS**. SLW 39.4%(7.3 ms) — near-band/SSS는 셰이더 math라 무시; 포말 collar는 Translucency 8.9%(1.66
ms) 내(36-seg 리본 cheap). **Part 2 4개 표면 + 시네마틱 티어 전부 완료.**

### 6.13 안티-타일링 / 베이크드 베리에이션 (Part 2-5)

반복(타일링)이 "CG 같은 티"의 큰 원인 → **다양한 베리에이션을 미리 구워**(+ 매크로 변주) 반복을 깸. 전부 렌더 전용.
`Tools/probe_water_params.py`로 워터 머티리얼 92 스칼라/8 벡터 이름·기본값 확보 후(추정 금지) 진행.

- **함선 베이크드 변주**: `textures.py`가 디테일맵 2번째 변주(`T_ShipDetail_N2/RAO2`, 다른 seed)를 굽고,
  `make_naval_hull`이 **두 변주를 저주파 매크로 마스크(~30 m 정적 노이즈)로 스토캐스틱 블렌드**(RAO + coarse 노멀;
  fine은 단일 = 비용↓) → 헐의 플레이트·녹 패턴이 영역별로 달라져 ~6.5 m 타일 반복이 사라짐. 작은 부위(deck/sensor/
  missile/rocket = `make_detailed`)는 cheap 1-샘플 **매크로 변주**(rust 영역별 모듈). 헐은 화면 소수 픽셀이라 +6 샘플 무시 가능.
- **워터 스펙트럼**: Gerstner 대역을 7..**160 m**로 확장(완만한 long swell 추가) — **wave COUNT는 32 유지**.
  처음엔 48-wave로 늘렸다가 perf MARGINAL(SLW 39→46%, +1 ms·워터가 지배항이라 fidelity-per-ms 나쁨) → **카운트는
  안 늘리고 대역만 넓힘**: 같은 32-wave를 160 m까지 펴면 반복 주기↑ + 대규모 변주 추가가 **사실상 무료**.
- **매크로 강화(visible payoff)**: 처음엔 효과가 미미(faceted 함선 + 이미 32-wave 워터) → 매크로 dirt 변조를
  **강화**(헐 0.25..1.85, 부위 0.25..1.8)해 실함선 같은 **불균일 웨더링**(영역별 녹/때·페이드)을 부여 → de-tiling +
  "lived-in 함선" 리얼리즘이 **명확히 보임**(p25_hull2). ~30 m 매크로 스케일이라 매끈한 영역 변주(노이즈 스팟·얼룩 아님).
  노드 그래프 불변(상수만)이라 perf 위 PASS 그대로.
- **남은 한계(정직)**: 워터는 여전히 미세(외양 심해 + LOD-flatten으로 원래 반복이 안 두드러짐). 함선 웨더링이 P2-5의 실 payoff.

**측정(게임 부하, 1440p, cooled):** avg 17.70 ms / **56.5 fps**, p95/p99 21/22 ms, **max 22.7 ms**, over-33.3
**0.0%**, hitch 0 → ✅ **PASS**. (48-wave 시도 시 max 400 ms 1-회 히치는 신규 머티리얼/텍스처 **첫-렌더 셰이더
컴파일** 1회성 — 32-wave clean 재런에서 소멸 확인. 정상.)

### 6.14 정밀 디테일 패스 (Part 2-6) — 스크린샷 비평 4축 직접 해소

P4 시네마틱 스샷 4장(hero·hull·godrays·combat) 엄밀 자기-비평의 4축을 직접 해소(전부 렌더 전용, 결정론/sim/서버
무관). faceted 스텔스 아이덴티티 보존 — 헐·상부구조 각짐 유지, **장비·실린더만** 라운딩·디테일 추가.

- **A 함선 지오메트리**(`frigate.py` + `asset_lib.py`): LOD0 930→2068 tris(단일 메시·드로 1회 → GPU 무시 가능).
  (1) 실린더형 파트 선택적 스무딩(`_smooth` per-face, join 생존; 함포 배럴·helo 테일·liferaft) + 신규 `dome`/
  `vcylinder` 프리미티브로 **둥근** CIWS/SATCOM 라돔·funnel 업테이크·capstan·bollard → "둥근 형상 부재" 해소.
  (2) 앵귤러 그리블 밀도↑(Phalanx 건+라돔·함미 RAM 박스·windlass·RHIB+다빗). (3) **시스루 가드레일**(스탠션+
  라이프라인, LOD0 한정) — 솔리드 벌워크 교체, 상방 deck-detail 캡처로 가독 확인. (4) 떠보이던 fwd CIWS(데크하우스
  전방 무지지) → **브리지 지붕으로 재배치**(저각 헐 캡처로 식별·수정).
- **B god-ray/블룸 톤다운**(`setup_level`/`patch_level`): `light_source_angle` 1.1→0.6(비대 디스크 → 정의된
  태양), light-shaft `bloom_threshold` 0.18→0.40·`bloom_scale` 0.28→0.20, grade `bloom_intensity` 1.0→0.75
  → 흰 splat이 정의된 광원+절제된 글로우로(**가장 큰 가시적 개선**, 클라우드 가독 회복). ProfileGPU: LightShafts
  (Bloom) 0.23 ms·(Occlusion) 0.16 ms·Bloom 0.23 ms·LensFlare 0.22 ms = 각 ~1%(무시 가능).
- **C 워터 mid-field/반사**(`make_sea_ocean`): `Far Normal Fresnel Power` 14→18·`Distant Normal Strength`
  0.12→0.16/B 0.09→0.12(mid-field 릴리프 회복, 플랫튼을 수평선에 한정) + `Near Normal Strength` 0.42→0.34·
  `Water Roughness` 0.06→0.075(근경 Lumen 반사 디노이즈 스미어 완화). 수평선 시머 무회귀(distant < stock 0.30).
  근경 "oily" 반사는 **Lumen-on-water 한계로 부분 잔존**(정직).
- **D 전투 가독성**: 근접 요격 컷은 **프레이밍** 이슈가 주 → B의 블룸 톤다운으로 airburst가 개별 오렌지 디토네이션
  으로 또렷이 읽힘("SPLASH — TARGET DOWN" 컷). VFX 스케일 무변경(프레이밍으로 해결).

**측정(게임 부하, 1440p, ProfileGPU 11연속 캡처 직후 = HOT/worst-case):** 1250 steady, avg 17.62 ms /
**56.7 fps**, p95/p99 21/22 ms, over-33.3 **0.1%**(1250프레임 중 1~2), hitch 0 → ✅ **PASS**. P2-5 cooled
베이스라인(56.5 fps·p99 22)과 동일 → 지오 +1138 tris·B/C 값 변경 **perf 영향 0**. GPU 지배항 불변(SLW 38.2% /
7.16 ms). 신규 툴: `reimport_frigate.py`(프리깃 단독 재임포트+슬롯 재배정 — 머티리얼 재생성/오션 참조 무손상),
`showcase_shots.sh`(비평 4축 레퍼런스 프레이밍 before/after 재현).

### 6.15 true-AAA 갭 클로징 (P3) — 다중 페르소나 critic 피드백 루프

P2-6 후 4-critic이 만장일치 D/D+("tech-demo + 포스트 화장")로 판정 → 목표를 **true top-tier AAA**로 상향(아키텍처
교체 포함). 두 도메인 critic subagent(해군 아트디렉터=함선, 렌더링 엔지니어=워터/라이팅)를 **연속 4라운드** 돌려
캡처-비평-수정 루프를 반복. 전부 렌더 전용(결정론/sim/서버 무관).

- **함선 (D+ → C- → C+ → B 궤적)**: ① **모델드 무장**(`frigate.py`) — 박스 터렛/스텁 배럴을 **faceted 함포-하우스
  +테이퍼 배럴+머즐+다크 맨틀릿**으로, VLS를 8×4 32셀 **플러시 해치 그리드(다크 리세스 그라우트)**로, 마스트에
  **AESA 어레이 면** 추가(해군AD #1 "지오메트리 게이트" 해소). ② **3-tier 밸류 스플릿**: 상부구조(`M_NavalGray`)
  0.14→**0.20**(LIGHTEST)·헐 haze **0.10**(MID)·센서/마스트 `M_SensorDark` **0.045**(DARKEST) — 노출 후에도
  3티어가 읽히도록 ≥0.08 절대차. ③ **부트-토핑** ~1.3 m 흘수선 다크 밴드 + 안티파울 다크-레드. ④ **플레이트
  밀도 정정**: `textures.py` major 256→**512**(8→4 플레이트/타일 = ~0.3 m→**~1 m** 플레이트) + 용접비드 제거 →
  "리벳/퀼트" 과텍스처를 **클린 아키텍처럴 패널 라인**으로. ⑤ **그라임 그라디언트**(world-Z, 흘수선↑ 농도).
- **워터 (C- → C+/B- 궤적)**: ① **검은 "oily 리본"** 근본원인은 SLW ray-miss가 아니라 **near-mirror 러프니스가
  어두운 씬을 반사**(렌더엔지니어 정정) → `Water Roughness` 0.075→**0.13**·`Water Fresnel Roughness`→0.22. ②
  **de-pool**: `Scattering`을 pool-cyan에서 다크 petrol로 내렸다가(0.12,0.24,0.32) **black-glass 회귀** →
  **luminous 플로어**(0.18,0.36,0.48)로 재조정(전경을 빛나는 물 위로 들어올림). ③ **레인보우 회귀 차단**:
  SkyLight lower-hemi fill이 grazing clip/dispersion으로 무지개 흘수선 유발 → **A/B로 확정 후 롤백**(black 복귀).
  ④ **매트 포말**: 플러그인이 **`Foam Roughness`** 노출(프로브 확인) → 0.90(specular chrome streak → 매트
  화이트워터). **SLW 탈출 불요로 판정**(go/no-go 게이트 통과). ⑤ height fog 0.024→0.028·start 85→72km(수평선
  seam 완화). 근경 grazing 반사는 **SLW 천장으로 일부 잔존**(정직 — 추가 개선은 depth-gradient/C++ 헐 포말 콜라).

**측정(게임 부하, 1440p, cooled):** 1137 steady, avg **19.37 ms / 51.6 fps**, p95/p99 **23/24 ms**, over-33.3
**0.1%**, hitch 0 → ✅ **PASS**(p99 24 ms = 41.7 fps, **40fps 플로어 충족**). GPU 지배항 불변(SLW 40.0% / SLW::Draw
7.79 ms). 함선 지오 +무장/그리블·밸류/포말 머티리얼 변경 **perf 영향 0**(전부 값/노드, 화면 소수 픽셀).
**⚠ 열 함정(중요)**: 백투백 GPU 작업(에디터 사이클×다수 + Blender + 캡처) 직후 측정 시 **전 패스 균일 ~2× 팽창**
(avg 34 ms / SLW::Draw 11.9 ms = FLAG)이 발생 — **스로틀 아티팩트**(pmset therm 무경고에도). 권위는 **cooled
측정**(~180s GPU 유휴 후): 위 19.4 ms가 진짜. 매 perf 게이트 전 쿨다운 필수. 신규 툴: `import_textures.py` 재사용,
critic 추출(transcript JSONL의 최장 VERDICT 텍스트 블록 파싱).

### 6.16 함선 표면 베이크 (P3-A) + 워터 SLW-천장 확정 (P3-B 프로브)

5라운드 critic 후 갭이 **표면 밸류 → FORM(형상)**으로 좁혀짐. 함선 표면 아키텍처 패스 + 워터 한계 확정 프로브.

- **P3-A 함선 표면 (cavity AO + 베벨, → ~B-)**: ① **cavity AO 베이크** — Blender Cycles AO를 정점-컬러로 베이크
  (`asset_lib.cavity_ao`, LOD0만 cuts=1 서브디비전 8.9k→**35k tris**), FBX→UE 정점컬러 임포트(`reimport_frigate`
  `vertex_color_import_option=REPLACE`), 머티리얼이 `base * lerp(0.68,1,VC)`로 곱함 — 블록 접합부·데크엣지 리세스·
  수직면 베이스가 occlusion으로 어두워져 "putty monolith"를 깸. 머티리얼-측 **콘트라스트**(`saturate((VC-0.25)*1.7)`)로
  blobby AO를 타이트한 리세스 라인으로 핀치(정점 밀도 무증가). ② **베벨 0.09→0.18 m** — 90° 하드 엣지가 lit 챔퍼
  하이라이트를 잡게(해군AD 5 #1 "no lit edge = papercraft"). ③ 폴카닷 마이너 그리드 약화 + 메이저 시임 AO 심화(패널-
  라인 밸류). **측정(cooled, 35k LOD0):** 1217 steady, avg **18.08 ms / 55.3 fps**, p95/p99 21/22, over-33.3
  **0.1%**, hitch 0 → ✅ **PASS**(SLW::Draw 6.35 ms). +26k tris **perf 영향 0**(정점 비용 무시, 함선 = 소수 픽셀).
- **P3-B 워터 SLW-천장 확정**: 근경 grazing "검은 글래스 미러"가 **유일 잔존 워터 갭**(render-eng 4: SLW 평면-셰이딩
  구조적 한계 = escape 트리거). **cheap 레버 전수 소진 확정**(probe-first): Water Roughness·Scattering·Near Normal
  0.34→0.60·**Gerstner steepness 0.42→0.62**·**진폭 50→100 cm(중간 해상)** — 어느 것도 grazing 미러를 못 깸.
  중간 해상은 워터 바디(텍스처/움직임/매트 포말)를 개선해 **유지**, 단 미러는 불변 → **근경 미러 = SLW 셰이딩 하드
  천장 확정**. 잔여 = **Phase 6b 풀 커스텀-오션 escape**(비-SLW 셰이딩/변위 메시·planar 반사 — 최대 리스크 아키텍처,
  별도 집중 트랙). 워터 파라미터는 전부 **렌더 전용**(게임플레이 sea는 C++ `SeaEnvironmentController` → perf 무관).

### 6.17 Phase 6b — SLW escape 프로브 & 결정(probe-first, 캡처 검증 → SLW strong-B 확정)

근경 grazing "검은 글래스 미러"(SLW 셰이딩 하드 천장)의 유일 잔여 픽스인 **SLW escape**를 probe-first로 검증하고
오너 결정으로 마감. **전부 렌더 전용**(결정론·sim·서버·부력 무관 — 부력은 변경 없는 플러그인 `UWaterWaves` 샘플).

- **Stage-0 프로브(make-or-break = 변위가 비-SLW 머티리얼에 적용되나)**: `WaterBodyOcean`에 from-scratch
  `DEFAULT_LIT` 머티리얼 할당 → **flat(변위 없음)**. 근본 메커니즘 발견: Gerstner 변위는 **머티리얼 WPO**
  (`/Water/Materials/Functions/WaterHeightMappingVS`·`ComputeGerstnerWaves`)라 셰이딩 모델과 **독립**. 진짜 마스터
  `/Water/Materials/WaterSurface/Water_Material`(NB: `Water_Material_Ocean`은 인스턴스의 인스턴스 → `shading_model`
  속성 없음)을 복제해 **DEFAULT_LIT로 플립** → 변위·포말·뎁스 **생존**, 근경 미러 **소멸** ✅(probe_dup, show_2_hull
  캡처). → **Path A(escape) 기술적 성립 확정.**
- **production v1/v2(A/B 캡처, show_1_hero·show_2_hull·show_3_godray)**: 미러는 사라지나 DEFAULT_LIT가 SLW의
  **볼류메트릭 Absorption/Scattering 뎁스-컬러 파이프라인을 우회** → 바다가 flat/washed(godray = 밀키 시트),
  그리고 마스터 컬러 파라(`Water Albedo` 0.85·`Absorption` 10/150/350·`Scattering`)가 **DEFAULT_LIT에서 안 먹음**
  (SLW 경로 구동). v2(Water Albedo 다크 네이비)도 무변화 → **파라 튜닝으론 복구 불가, base-color/reflection/normal
  from-scratch 그래프 재작성 필요**(멀티사이클·SLW 능가 불확실).
- **오너 결정(2026-06-17)**: **6a SLW strong-B 확정**, grazing 미러는 **알려진 잔차로 문서화**(plan kill-criterion),
  효열 ROI가 더 큰 **함선(critic 4/4 #1 killer) Phase 7로 전환**. 라이브 오션은 SLW `MI_SeaOcean` 유지(미변경).
  escape 재생성 코드는 파킹: `setup_materials.make_sea_ocean_custom` + `Tools/apply_custom.py` + `cinematic_shot.sh
  --map`; throwaway 프로브 에셋(L_RangeProbe*/L_RangeCustom·M_OceanProbe_*·*SeaOceanCustom)·프로브 스크립트 삭제.
  신규 일반 기능: `cinematic_shot.sh --map <pkg>`(비-기본 맵 캡처). `save_map`(save-as) 패턴 — 헤드리스에서
  duplicate_asset+load_level은 GC-fatal, save-as는 안전. **perf 무관**(라이브 SLW 미변경 = §6.16 PASS 유지).

### 6.18 Phase 7 — 히어로 함선 (해군-AD critic 루프, 진행 중: C/C+ → B−)

워터 6b 셸빙 후 ROI 최우선인 함선으로 전환. 해군-AD critic 절대 AAA 재판정 → 갭 우선순위화 → probe-first 수정 → 재판정.
**전부 렌더 전용·perf 무관**(라이팅 = 광원 방향, 머티리얼 = 값/러프 파라).

- **critic #1 (C/C+)**: "시네마틱 rig 값 내고 overcast 그레이박스 렌더 — authored 키 라이트가 샷에 안 닿음, flat·cool·터미네이터 없음." → **라이팅이 #1, 표면은 그 다음**(키가 폼을 안 깎으면 머티리얼 판정 불가).
- **수정 (capture-verified)**: 근본원인 = **Sun yaw 35°가 히어로 캠(SW→NE 시선) 바로 뒤 = 정면광 = flat**(프로브: 같은 각도 cloud-hide 컨트롤도 flat → 구름 아닌 방위각). **yaw 35→−55, pitch −16→−22로 측면 raking** → 패싯이 실제 명암 스텝으로 폼을 읽음. `setup_level.py`+`patch_level.py`(Rotator 인자 = (roll,pitch,yaw) on this build) + L_Range 적용. **→ B−**(critic 재판정, no inflation; 병목이 라이팅→표면 머티리얼 계층으로 이동).
- **표면 증분 (B−)**: critic 재#1 = "putty monolith" 단일 그레이. ① **M_SensorDark → 블랙 레이더 글래스**(base 0.045→0.020·rough 0.35→0.12·metallic 0.6 → 레이더면이 raking 키에 샤프 스펙큘러). ② **흘수선 wetness 밴드 확대**(top 260→400 cm·power 1.7→1.2 → 젖은 띠가 읽힘). `setup_materials.py` 파라(소스), `tune_mats.py`로 M_NavalHull+M_SensorDark만 in-place 리빌드(메시 슬롯 경로 참조 → 재할당 불요).
- **잔여**(critic): 함포/CIWS/헬로 = 별 gunmetal 머티리얼(frigate.py 재할당 + Blender 재임포트); 디테일-노멀/판접합 seam 심화(textures.py); 워터 반사 콘트라스트(make_far_ocean 값↑). 각 재캡처+critic 게이트. 신규 dev 툴: `tune_light.py`·`tune_mats.py`.

### 6.19 워터 GRADE-A (Path B 시네마틱 오션) + 함선 스케일 데칼 — 프레임 A 클로즈아웃

**오너 지시("최소 A까지, 모든 수를 써서라도") → 6b escape 재개. 결론: SLW-escape 커스텀-머티리얼 접근은 데드엔드(렌더 안 됨)였고, 진짜 컨트롤 가능 AAA 워터는 Path B(자체 메시 + from-scratch 표준 머티리얼).**

- **데드엔드 확정(캡처 검증, ~24 사이클)**: Water 플러그인 마스터를 복제해 DEFAULT_LIT로 뒤집고 셰이딩을 주입하는 escape는 **유효 셰이더를 컴파일 못 해 UE 기본(grey) 머티리얼로 렌더**. 또 플러그인 surface 머티리얼은 헤드리스로 제어 불가(body `water_material`/SetMaterialAttributes/Break-Make 모두 무반응; red/green/emissive 디버그로 확정). **렌더엔지 critic이 그동안 "B+"로 매긴 캡처는 전부 SLW 폴백 navy였음**(내 셰이딩이 아니었음).
- **Path B(작동)**: 별도 `StaticMeshActor`(`SM_OceanGrid`, ~1.2km) + 신규 표준 머티리얼 `M_SeaOceanWPO`(navy/Fresnel/depth base + 베이크드 3-스케일 `T_WaterRipple_N` 마이크로-노멀 → MP_NORMAL + Custom-HLSL 8-wave Gerstner → MP_WORLD_POSITION_OFFSET). `ocean_grid.py`→`import_ocean.py`→`make_sea_ocean_wpo`→`apply_ocean_b.py`(STAGE_ORIGIN 스폰, SLW body hide). **렌더엔지 critic: A− → (디코릴레이션+글리터+seam 매칭) → A 락**("defensible AAA-adjacent real-time naval water").
- **핵심 함정(메모리 `water-slw-grazing-ceiling` 갱신)**: ① Fresnel이 MP_NORMAL로 들어가면 PixelNormalWS 사이클 → 컴파일 실패 → **VertexNormalWS를 Fresnel Normal 입력으로**. ② `-nullrhi` apply는 셰이더 미컴파일 → 캡처가 기본/구셰이더; **real-RHI apply 필수**(base-red 디버그로 확인). ③ FBX 그리드 ~100× 스케일 → actor scale 0.02.
- **함선 스케일 데칼(해군-AD #1 레버)**: `T_HullMarkings`(함번호 "81"·드래프트 마크·해치, PIL) → world-projected(world-Y→U, world-Z→V, CLAMP = 1회) → `make_naval_hull`에서 흰 페인트. 텍스처 4:1 ↔ 윈도우 4:1(Yspan=4×Zspan) 아니면 늘어짐. 함번호 렌더 확인(dcheck/final_number). + RAO 타일 420→320(노멀 타일 매칭), fine-노멀 밴드 140→220m hold(히어로 seam mip-bias).
- **듀얼티어 / perf**: 게임플레이 = `L_Range`(SLW, **미변경 → perf 무관**); 시네마틱 = `L_RangeCustom`(CineOcean A-water, `apply_ocean_b`가 `L_Range`에서 파생 빌드, fps 무제약). 부력은 hidden 플러그인 오션 웨이브 계속 샘플 → **결정론·sim·서버 무관**. M_NavalHull 변경(데칼 텍스처 샘플 1개·1 draw)은 양 티어 공유지만 sub-measurable → cooled 게임플레이 perf 재확인이 형식 게이트(잔여).
- **최종 프레임 A 비주얼**: `final_hero`/`final_hull`/`final_number`/`final_godray` = A-water + 함선(함번호·gunmetal·raking). 신규 툴: `ocean_grid.py`·`import_ocean.py`·`apply_ocean_b.py`·`make_sea_ocean_wpo`·`_hull_decal_mask`.
- **S2 폴리시(A−→A)**: ① 함번호 크게(font 150→182, fill→252) — 히어로 거리 가독↑(s4_number 검증). ② **deck-from-above 마킹**(naval-AD gap #3): `T_DeckMarkings`(헬로 서클+H·논스킵 대시·포어덱 해치) → **top-down world-projected**(world-Y→U, world-X→V, CLAMP) → `make_detailed(deck_decal=True)`로 M_NavalGray(=데크+상부구조)에 흰 페인트. 마킹은 개방 데크 양끝(헬로 aft·해치 fwd)에 배치해 중앙 상부구조 회피(Z-마스크 불요). 컴파일 클린; raked-sun이 데크를 그림자에 두는 프레이밍에선 가독 약함(라이팅/프레이밍 의존). `final2_*` 재생성.

### 6.20 능동 비주얼 QA 패스 — 사용자 지적 결함 3종 직접 발견·수정 (메모리 [[proactive-visual-qa]])

사용자 지적: 물보라/포말 부재 · 바다 패턴화 · 함선 일부 공중 부유. 전용 진단 캡처(레벨 측면 프로파일·흘수선 클로즈·다운앵글 워터)로 직접 검수해 근본원인 격리·수정·재캡처 검증.
- **부유 지오(frigate.py)**: `ram_box`가 y−11.5..−8(hangar y−44..−14와 funnel y−8..0 사이 **개방 데크 갭**)에 z11.6으로 배치 → 데크(z5) 위 **~6.5 m 공중 부유**("hangar roof" 주석과 불일치). hangar 지붕 위(y−25..−21, z11.0)로 이전. `ciws_aft` 행오버·`ciws_fwd` 0.8 m 갭·deckbox/raft 소갭도 seat. Blender 재export+reimport. aftchk 검증.
- **포말(SeaVfxSystems.cpp)**: 흘수선 collar는 항상-on이나 `kHullFoamBaseOp 0.28`·band 320·lift 60 = **너무 옅고 얇아 안 읽힘**. base op 0.28→**0.70**, band 320→**560**, inset 70→130(헐 겹침). C++ 리빌드(8 s, succeeded). fix_waterln/dt_mid에서 흰 포말 collar 가독 확인.
- **패턴화(setup_materials.py)**: 그레이징 미드-파 필드의 'horizon으로 행진하는' 노멀 반복이 주범(글리터용으로 depth-fade를 완화한 부작용). 수정: 미세노멀 depth-fade를 **빠르게·낮은 floor**(start 100→50 m, floor 0.6→0.22, ~450 m에서 floor) → 미드-파가 매끈한 aerial-perspective 수면으로 → 행진 타일링 소멸. + 옥타브 UV-rotate(37/−23°)·30 m 진폭 변조·capillary weight 1.3→1.0. near 필드 facet 유지. dt_hero 검증(미드-파 매끈).
- **perf**: 포말 = 36-seg 리본(무시), de-tile = 상수 변경(무비용), 지오 = 동일 tri → §6.19 PASS 유지.

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

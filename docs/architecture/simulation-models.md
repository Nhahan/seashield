# 시뮬레이션 모델 명세 (Simulation Models)

> Project SeaShield — 시뮬레이션 수학의 단일 참조점. P2(환경·외탄도·PIP)와 P4(레이더·칼만 추적·기동 표적)의 모델을 수식 유도와 함께 정리하고, 각 수식이 어느 테스트로 고정되어 있는지를 추적한다(부록 C).
> 상위 문서: [01-기획서.md](../01-기획서.md) §5 (시뮬레이션 설계)
>
> 문서 버전: v1.1 (2026-06-14) · 상태: P4 기준 + P7+ 자함 기동·회피(§8b)
> 모든 파라미터는 공개 자료 수준의 교육용 값이다 (기획서 §2 면책).

---

## 1. 범위와 충실도 선언

- **3DOF 질점 + CV 칼만 + 무유도 일제사**가 동결 범위다 (기획서 §11 리스크 표 이행). 6DOF 자세 동역학, IMM 다중모델, JPDA 연관은 백로그.
- 본 문서의 모델은 "실존 체계의 모사"가 아니라 **무유도 요격의 한계를 정량 탐구하는 실험 장치**의 구성 요소다 (기획서 §2.4).
- 구현 좌표: `sim/` 전체. 모든 초월함수는 `seashield::math::` 경유 (libm 교체 가능성 확보, 기획서 §5.1).

## 2. 좌표계·시간·결정론

- 로컬 ENU 직교좌표: x=동, y=북, z=상. 원점 = 자함. 방위각 az는 북 기준 시계방향, `direction_from_az_el(az, el) = (sin az·cos el, cos az·cos el, sin el)`.
- 시간: 60Hz 고정 틱(`kTickDt = 1/60s`), 시뮬레이션 시간은 틱 카운터로만 진행.
- 결정론 조건: 갱신 순서 고정(`World::step()`의 단계 번호 주석), 시드 고정 PCG32 스트림 분리(weather=1, gust=3, dispersion=10, pk=11, radar 탐지=12, radar 노이즈=13), 모든 가변 상태+RNG state의 FNV-1a 해시(§10.2 골든 회귀).
- FP 통제: `-ffp-contract=off`(FMA 융합 금지) + `-fno-builtin-sin -fno-builtin-cos`. 후자는 Darwin clang이 -O1+에서 동일 인자 sin/cos 쌍을 `__sincos`로 융합해 -O0 대비 1ulp 차이를 만들던 실제 골든 회귀에서 나온 잠금이다.

## 3. 대기·바람·거스트 (P2)

**ISA 간이 대기**: 해면 온도 T₀, 기압 p₀, 감률 L에 대해

- T(h) = T₀ − L·h
- p(h) = p₀·(T(h)/T₀)^(g/(R_d·L))  (정역학 + 이상기체에서 유도)
- ρ(h) = p(h)/(R_d·T_v(h)), 습도는 Magnus 식의 수증기 분압으로 가상온도 T_v 보정.

**바람**: 고도 레이어(0/0.5/1.5/3/6km) 벡터 선형 보간, 양끝 클램프. 기상 생성기는 멱법칙 증속과 Ekman 전향을 시드 하나로 샘플링.

**거스트(OU 과정)**: dX = −X/τ·dt + σ√(2/τ)·dW 의 **정확 이산화**

> X(t+Δt) = X(t)·e^(−Δt/τ) + σ·√(1 − e^(−2Δt/τ))·N(0,1)

Euler–Maruyama(X·(1−Δt/τ) + ...)는 스텝 크기에 의존하는 분산 편향을 가지므로 쓰지 않는다 — 정확 이산화는 임의 Δt에서 정상분포 N(0, σ²)를 정확히 보존한다. **사격 솔루션은 평균 바람만 보상하고 거스트는 잔여 오차로 남는다 — 일제사가 존재하는 물리적 이유** (기획서 §5.6).

## 4. 외탄도 (P2)

질점 운동방정식 (`sim/ballistics.cpp`):

> a = g·ẑ⁻ + (T(t)/m)·d̂ − (ρ(h)·C_dA/2m)·|v_rel|·v_rel,  v_rel = v − w(h) − gust

- 추력 프로파일: t < burn_time 동안 상수 추력(boost), 이후 활공(glide). 추력 방향 d̂는 속도가 충분해지기 전까지 발사 방향.
- 적분: 고정 스텝 **RK4** (틱당 1스텝). 검증 3중주: ① 진공 해석해(포물선) 대조, ② 반스텝 수렴(스텝 절반 시 오차 4차 감소 확인), ③ 독립 Python/NumPy 구현 골든 CSV 대조(`tools/reference/ballistics_ref.py`).

## 5. 레이더 (P4, `sim/radar.cpp`)

**스캔 기하**: 빔 위상은 정수 틱에서 유도 — `phase = tick mod scan_ticks`, 표적 방위 빈 `b = ⌊az/2π·scan_ticks⌋`, **b == phase일 때만 탐지 기회**(회전당 정확 1회). 부동소수 누적 위상이 없으므로 해시할 빔 상태도 없다.

**탐지 확률**: SNR ∝ 1/R⁴를 dB 도메인으로 옮기면 기준거리 R_ref(임계 SNR에서 Pd=0.5)에 대해

> SNR_dB(R) = 40·log₁₀(R_ref/R) − 2·(R/1km)·α_rain·I_rain,  Pd = 1/(1 + e^(−SNR_dB/k))

k는 시그모이드 기울기(dB), α_rain은 양방향 강우 감쇠(dB/km). R = R_ref에서 정확히 0.5.

**레이더 호라이즌 (4/3 지구 모델)**: 표준 대기 굴절을 유효 지구 반경 kRe(k=4/3)로 흡수하면 안테나 높이 h_a, 표적 고도 h_t의 기하 한계는

> R_h = √(2·k·R_e·h_a) + √(2·k·R_e·h_t)

h_a=20m, 시스키머 h_t=10m → R_h ≈ 31.5km. **이 너머에선 Pd 롤 이전에 기하로 차단** — 시스키밍 위협이 "늦게 보이는" 메커니즘이며, 호라이즌이 교전 가용 시간을 직접 결정한다(실험 보고서 horizon 축).

**측정과 변환 공분산**: 구면 측정 (r, az, el)에 가우시안 노이즈(σ_r, σ_az, σ_el) 부가 후 직교 변환. 필터에 줄 측정 공분산은 변환의 1차 전파(converted measurement):

> R = J·diag(σ_r², σ_az², σ_el²)·Jᵀ,  J = ∂(x,y,z)/∂(r,az,el) (측정점에서 평가)

J의 성분은 x=r·sin(az)cos(el) 등에서 직접 미분(`radar.cpp`). 횡거리 분산이 r²σ_az²로 거리 제곱 성장하는 것이 원거리 트랙 품질 저하의 원천. **레이더가 R을 플롯에 동봉하므로 "모델과 필터의 정합성"(§5.5)이 구조적으로 보장된다** — 같은 σ가 한 곳에서 노이즈 생성과 R 산출에 쓰인다.

**RNG 규율**: 탐지 기회 1회 = 항상 7 draw(Pd 롤 1 + 가우시안 3축×2), 성패와 무관 — 서브시스템 내부의 조건부 소비를 제거. 빔 교차 조건 자체는 해시되는 결정론적 상태의 순수 함수이므로 "교차 시만 draw"는 안전하다(스트림 분리가 서브시스템 간 소비 밀림을 차단).

**범위 외**: 클러터(허위 플롯)는 P4에 없다 — Pd<1의 미탐지와 호라이즌만으로 트랙 관리가 충분히 운동되며, 클러터는 연관 모호성(JPDA 영역)을 끌어들인다. 연관 루프는 다중 플롯 일반형으로 작성되어 자료구조 변경 없이 추가 가능(백로그).

## 6. 칼만 필터 (P4, `sim/tracking.cpp`)

**상태공간**: x = [p, v] ∈ R⁶, 등속(CV) 모델.

> F = [[I₃, Δt·I₃], [0, I₃]],  H = [I₃ | 0]

**프로세스 노이즈 (DWNA)**: 틱 동안 상수인 백색 가속 a ~ N(0, σ_a²)를 적분하면 축별 2×2 블록

> Q_axis = σ_a²·[[Δt⁴/4, Δt³/2], [Δt³/2, Δt²]]

σ_a(기본 30 m/s²)는 ASM 위빙(수 g)을 흡수할 크기 — Q/R 튜닝이 트랙 품질→탄착 오차로 전파되는 §5.6의 시스템 결합이 실험 축이다.

**Predict / Update** (최소분산 선형 추정):

> x⁻ = F·x, P⁻ = F·P·Fᵀ + Q
> S = H·P⁻·Hᵀ + R, K = P⁻·Hᵀ·S⁻¹
> x = x⁻ + K·(z − H·x⁻)
> P = (I−KH)·P⁻·(I−KH)ᵀ + K·R·Kᵀ  (Joseph form) → P ← (P+Pᵀ)/2

- Joseph form은 (I−KH)P⁻ 단순형과 달리 반올림 하에서도 양반정치성을 보존한다; 명시적 대칭화가 잔여를 정리한다. **P의 36개 원소 전부가 월드 해시에 들어가므로** 비대칭/수치 드리프트는 결정론 회귀로 즉시 발화한다.
- 역행렬은 **S(3×3)만** 필요 — K = P Hᵀ S⁻¹. 일반 6×6 역행렬 루틴 없이 SPD 3×3 폐형(adjugate/det + det 가드)으로 충분(`core/matrix.h`). det 가드 실패 = 공분산 붕괴 → 해당 플롯 폐기.
- 검증: 독립 NumPy 구현과 60스텝 상태·공분산 대조(`tools/reference/kalman_ref.py` — 측정 시퀀스는 PRNG가 아닌 결정식: 언어 간 RNG 차이를 원천 배제하고 **필터 대수만** 비교).

**일관성 통계 (NEES/NIS)**: 추정 오차 e에 대해 NEES = eᵀP⁻¹e는 일관 필터에서 χ²(상태 차원) 분포. 본 검증은 위치 블록(3 자유도)만 사용(6×6 역행렬 회피, P_pos는 SPD 3×3). T회 시간 평균의 기대값은 3, 분산은 6/T.

- **위험한 방향은 과신**(NEES ≫ 3: P가 실제 오차를 과소평가 → 잘못된 상태를 확신) — 상한을 3.8로 조임.
- 보수적(NEES < 3)은 정보 낭비일 뿐 안전 — Q>0 필터가 무기동 진실을 추적하면 구조적으로 보수가 되므로 하한은 0.5로 느슨하게.
- 검출력의 증명: Q를 1/600로 줄인 과신 필터 + 위빙 표적에서 NEES가 상한을 돌파함을 별도 테스트로 고정.

## 7. 게이팅·연관·트랙 수명주기 (P4)

- **마할라노비스 게이트**: d² = νᵀS⁻¹ν ≤ γ. γ = 16.27은 χ²(3)의 0.999 분위수 — 정상 측정의 0.1%만 억울하게 기각. 기동 표적이 게이트를 이탈하면 트랙은 미스를 누적하고 코스팅(coast)한다 — 버그가 아니라 CV 한계의 발현(실험 대상).
- **연관**: 플롯별로 게이트 통과 트랙 중 d² 최소(NN). 동률은 낮은 id — 결정론 명시. 미연관 플롯은 단일 플롯 개시: 위치 = 플롯, 속도 = 0 (사전 σ_v = 400 m/s). 2점 차분 개시 대비 단순하며, 넓은 속도 사전분산이 다음 스캔의 게이트를 자연히 열어 준다(주: 매 틱 predict가 이 사전분산을 위치 불확실성으로 전파하는 것이 이동 표적 재연관의 메커니즘).
- **M-of-N 개시**: 스캔 경계에서 히트 비트마스크 갱신, 최근 N=5 스캔 중 M=3 히트면 confirmed. 확립 확률은 Pd의 이항 누적 — Pd=0.9에서 5스캔 내 확정 확률 ≈ 0.991, 평균 확정 시간 ≈ 3.3스캔.
- **소실**: 연속 3미스 스캔. 히트/미스는 **스캔 단위**다 — 빔이 표적을 지나지 않은 틱은 관측 기회가 아니므로 틱 단위 미스는 무의미.
- 격파된 표적은 레이더 입력에서 제외 → 에코 소멸 → 트랙 자연 소실: **요격평가도 센서 기반**(기획서 §2.1 킬체인의 마지막 칸).

## 8. PIP 사격통제 (P2 솔버 + P4 추정 입력)

표적 CV 외삽 p_t(t) = p₀ + v₀·t와 탄의 비행시간이 만나는 고정점:

> t* = ToF(aim(p_t(t*)))  — 닫힌 해 없는 비선형 연립

- 외측: PIP 시간 고정점 반복 t_{k+1} = (1−β)·t_k + β·ToF(...), **damping β = 0.5**. 수렴 조건은 축약 사상 |g'(t)| < 1 — 닫히는 표적에서 비감쇠 반복은 진동·발산했다(P2의 PIP 발산 디버깅: 무풍 655m 계통 오차의 정체).
- 내측: 조준점에 대한 az/el 슈팅(수치 미분 Newton). 외측 반복마다 **직사(저각) 탄도로 재초기화** — 고각(lofted) 분기로 새는 것을 차단.
- 수렴 판정: 시간 잔차 < 5ms **및** 예측 miss < 1m. 미수렴은 정직하게 nullopt(추월 불가 기하, 사거리 밖, **탄 수명 내 도달 불가** — 수명 6s 시나리오에서 ToF 8s 해가 없는 것은 올바른 거동).
- **P4의 유일한 변경은 입력의 출처다**: solve(위치, 속도)에 진실 대신 트랙 추정을 넣는다(`World::solve_for_track`, 솔버 무변경). 호출은 const·RNG 무소비 — 라이브 런과 리플레이의 호출 횟수가 달라도 해시가 갈라질 수 없다(테스트로 고정).
- 미확정(tentative) 트랙 사격은 절차적으로 거부 — CIC 교전 절차의 모사.

## 8b. 자함 기동·회피 (P7+, `sim/world.cpp` `OwnShip`)

플레이어가 조타하는 이동 플랫폼. 보유 set-point(rudder∈[-1,1], throttle∈[0,1])에서
매 틱 적분(RNG 무소비):

> ψ ← ψ + rudder·ω_max·dt,  v ← v + clamp(throttle·v_max − v, ±a·dt),
> p ← p + v·(sin ψ, cos ψ, 0)·dt   (ψ = 북기준 시계방향 침로)

- **원점 가정 해제**: 발사점 = 자함 갑판(p + 갑판오프셋), 로켓은 **자함 속도를 승계**
  (`world.cpp` launch). 레이더 안테나·표적 종말호밍·누수 판정도 전부 자함 자세 기준으로
  재정렬. 솔버는 `solve(target, launch_pos, launch_vel)`로 같은 보정을 받는다(const·RNG
  무소비 유지 — 해시 불변). 클라 탄착예측기(`SeaBallistics`)도 동일 항을 받아 화면 리드와
  서버 궤적이 일치.
- **회피(dodge) = 종말 선회율 제한**: 종말 침로를 자함 방위로 **율 제한 슬루**
  (`target.cpp`), 캡 `terminal_turn_rate_max`. 기본 0 = 무제한(즉시 스냅 = 레거시 비트동일).
  게임 모드는 유한 캡 + **늦은 종말 전환**(작은 `popup_range`)으로, 순항 중 누적된 자함
  오프셋을 짧은 종말 구간에서 미사일이 다 못 따라잡게 만든다 → CPA가 피격 반경을 넘으면
  회피 성립. 무기동 시 미사일은 정조준해 직격하므로 피격 반경 축소는 회피하는 함선에만 작용.
- **결정론**: 순수 운동학·무RNG, 기본값=원점 고정 플랫폼이라 N=1 단일교전은 비트동일,
  state_hash에 자함 자세 필드만 추가(양플랫폼 골든 1회 재생성). 조타도 외부 입력이므로
  입력 저널에 `steer` 줄로 기록되어 리플레이가 기동까지 재현(`determinism_test`).

## 9. 일제사·산포·Pk (P2)

- 발사각에 mil 단위 가우시안 섭동(시드 고정), 일제사 N발.
- 판정: 틱 내 선형 상대운동의 최근접점(세그먼트 PCA) 보간 → miss. `miss < r_fuze`면 Pk(miss) = 0.95·(1 − (miss/r_fuze)²) 확률 기폭.
- 근접신관은 표적 생존 여부를 모른다 — **모든 신관 반경 통과가 기폭·Pk 롤을 수행**하며 그 결과가 `would_kill`(비캡 발당 격추 확률, P6 추가 — 롤 순서는 결정론 계약의 일부라 골든 재생성 동반). `killed`는 교전당 1회 캡 판정(첫 격추가 교전 종료). 비캡 p₁로 만든 독립 예측 1−(1−p₁)^N은 **상한**으로 작동한다: 조준 바이어스(트랙 오차)와 거스트는 전탄 공통모드라 탄착이 상관되고, 실측 일제사율은 N≥8에서 이를 명확히 하회한다(16발: 19.0% vs 25.2%) — 실험 보고서 §5.

## 10. 추정 오차의 전파 (P4의 요지)

발사 순간 트랙 속도 오차 δv는 보정 기회 없이 그대로 탄착 오차가 된다:

> miss ≈ |δp + δv·ToF| + (산포·거스트 항)

- ToF ~ 10s에서 δv 5 m/s → 50m: **속도 추정 품질이 지배항**. 확정 직후(3~4 스캔) δv는 수십 m/s → 수백 m 빗나감(실측: 즉시 사격 626m vs 6s 안정화 후 25.8m — 샌드박스 데모 수치).
- "확정 후 안정화 대기(settle)"는 조준 품질과 반응시간의 트레이드오프 — 닫히는 표적은 기다릴수록 ToF가 줄어 이중으로 유리하나 교전 창을 소모한다. 실험 settle 축이 이 곡선을 그린다.
- 원거리일수록 r²σ_az² 횡분산(§5)이 커져 같은 안정화 시간으로도 δv가 크다 — 거리 축 실험에서 추정 사격의 miss가 진실 사격보다 빠르게 악화되는 이유.

## 11. 한계와 확장 (정직성 절)

- **CV vs 기동**: 위빙·선회는 CV 외삽을 구조적으로 깬다. IMM(기동/비기동 모델 혼합)이 자연스러운 다음 단계(백로그).
- **NN vs JPDA**: 단일 표적에선 동치에 가깝다. 다표적·클러터에서 NN은 트랙 스왑을 일으킨다 — 클러터 모델과 함께 백로그.
- **Converted-measurement 바이어스**: 구면→직교 1차 변환은 σ_az·r이 클 때 평균 바이어스를 남긴다. 본 영역(mrad 단위 σ, ≤40km)에서 σ_r·σ_az 곱 차수로 무시 가능; debiased 변환은 백로그.
- **단일 표적**: 다표적 동시 교전(트랙-무장 할당 문제)은 P4 범위 외.
- 모든 파라미터는 교육용 — 절대 수치가 아닌 **경향과 메커니즘**이 본 시뮬레이터의 주장이다.

---

## 부록 A. 기호

| 기호 | 의미 | 기호 | 의미 |
|---|---|---|---|
| Δt | 틱 간격 1/60s | σ_a | DWNA 가속 노이즈 |
| ρ(h) | 공기밀도 | σ_r/az/el | 레이더 구면 측정 σ |
| τ | OU 시정수 (2s) | γ | 마할라노비스 게이트 임계 |
| R_ref | Pd=0.5 기준거리 | β | PIP 고정점 damping (0.5) |
| k·R_e | 4/3 유효 지구 반경 | p₁ | 단발 요격 확률 |

## 부록 B. 기본 파라미터 (전부 시나리오 키로 오버라이드 가능)

| 파라미터 | 기본값 | 키 |
|---|---|---|
| 스캔 주기 | 2.0 s | radar_scan_period_s |
| 안테나 높이 | 20 m | radar_height_m |
| Pd=0.5 거리 | 30 km | radar_ref_range_m |
| σ_r / σ_az / σ_el | 30 m / 3 mrad / 5 mrad | radar_sigma_* |
| 강우 감쇠 | 0.3 dB/km | radar_rain_atten_db_per_km |
| σ_a | 30 m/s² | track_accel_noise |
| 게이트 γ | 16.27 (χ²₃ 0.999) | track_gate_gamma |
| M-of-N / 소실 | 3-of-5 / 3미스 | track_confirm_m/n, track_drop_misses |
| 개시 속도 σ | 400 m/s | track_init_vel_sigma |
| ASM 팝업/위빙 | off (0) | asm_popup_*, asm_weave_* |

## 부록 C. 수식 ↔ 테스트 추적표 (V-model)

| 모델 절 | 핵심 주장 | 고정하는 테스트 |
|---|---|---|
| §3 OU 정확 이산화 | 정상분산 보존 | environment_test: 거스트 분산/시드 결정론 |
| §4 RK4 외탄도 | 해석해·수렴·교차언어 | ballistics_test: VacuumParabola·HalfStep·MatchesIndependentPythonReference |
| §5 Pd 곡선 | 단조·기준점·우천 | radar_test: PdMonotonically·PdIsHalf·RainAttenuation |
| §5 호라이즌 | 시스키머 늦은 탐지 | radar_test: HorizonMasks·SeaSkimmerIsDetectedLaterThanHighFlyer |
| §5 변환 공분산 | SPD·r² 성장 | radar_test: ConvertedCovarianceIsSpd |
| §5 RNG 규율 | 기회당 고정 7-draw | radar_test: MissedDetectionStillConsumes |
| §6 predict/update | NumPy 대조 (1e-9) | tracking_test: MatchesIndependentNumpyReference |
| §6 Joseph form | P 대칭·양정치 유지 | tracking_test: CovarianceStaysSymmetric |
| §6 NEES/NIS | 일관성 + 과신 검출력 | tracking_test: NeesAndNis·OverconfidentFilterFails |
| §6 CV 한계 | 기동 시 추정 지연 | tracking_test: ManeuverInducesLag |
| §7 M-of-N·소실 | 경계 정확성 | tracking_test: ConfirmsExactlyAtMOfN·DropsAfterExactMissStreak |
| §7 게이트/NN | 기각·재개시·결정론 | tracking_test: GateRejects·NearestNeighbour·IdenticalPlotSequences |
| §8 PIP 고정점 | 수렴·입력 무관성 | fire_control_test 전체 (ClosingTargetConvergesOnDirectArcAndHits 등) |
| §8 추정 입력 | const·해시 불변 | world_test: SolveForTrackEnforcesConfirmation |
| §9 판정 | PCA·Pk 경계 | engagement_test 전체 |
| §10 오차 전파 | 추정 사격 열화의 유계 | fire_control_test: EstimateFedSolutionDegradesGracefully |
| §2 결정론 전체 | 골든 해시·저널 재현 | determinism_test 전체 (트랙 이력 재현 포함) |

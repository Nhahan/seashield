# 요구사항 추적성 매트릭스 (V-model)

> Project SeaShield — 요구·약속 → 설계 문서 → 구현 → 검증 증빙의 추적표 (P6 산출물,
> 기획서 §9). 수식 단위의 세부 추적은 simulation-models.md 부록 C가 담당하고, 본
> 문서는 체계 수준을 묶는다. 문서 버전: v1.3 (2026-06-14 — R16 자동 프레임 예산 계측·
> 워터 GPU 회귀 수정 반영; P7+ 자함 기동·회피 R19; 테스트 224개)

| # | 요구/약속 (출처) | 설계 | 구현 | 검증 증빙 |
|---|---|---|---|---|
| R1 | C++ / TCP·UDP 멀티클라이언트 게임서버 (하드 요구사항) | 기획서 §3.1·§4 | `net/`(Reactor kqueue/epoll), `server/sim_server` 2-스레드+SPSC | `SnapshotsFlowToEveryRole`(8클라), loadclient 데모, CI ubuntu/macos 6 jobs |
| R2 | 멀티클라 = CIC 운용석 역할 분담 (기획서 §3.2) | protocol-spec §6 | `protocol::Role`, 역할 배타·kSolo, 서버 role_available | `ExclusiveRolesAreRejectedWhileSeatIsLive` |
| R3 | 60Hz 결정론: 시드+저널 = 전체 재현 (기획서 §5.1) | simulation-models §2 | PCG32 스트림 분리, FNV-1a 전 상태 해시, journal %.17g | `DeterminismTest.*` 골든(darwin/linux), `ReplayFromJournalMatchesOriginalRun` |
| R4 | FP 결정론 (플랫폼·최적화 무관) | simulation-models §2 | `-ffp-contract=off -fno-builtin-sin/cos` | Debug↔Rel 비트 동일(실험 CSV 교차 검증), 골든 양플랫폼 일치 |
| R5 | 무유도 사격통제 체인: 예측→외탄도→PIP→일제사 (기획서 §5.6) | simulation-models §4·§8·§9 | `sim/ballistics`(RK4), `sim/fire_control`(PIP 고정점+damping) | `FireControlTest.*`, Python 레퍼런스 대조, 부록 C |
| R6 | 센서 시점: 레이더→칼만→추정 기반 사격 (기획서 §5.4-5.5) | simulation-models §5-§7 | `sim/radar`(Pd·호라이즌·야코비안 R), `sim/tracking`(Joseph KF·M-of-N) | `RadarTest.*`, `TrackingTest.*`(NumPy 1e-9, NEES/NIS), `SolveForTrackEnforcesConfirmation` |
| R7 | 연구 질문: 무유도 요격의 한계 정량화 (기획서 §2.4) | reports/fire-control-experiments.md §1 | `tools/experiment/`(시드 재현 그리드), would_kill 비캡 지표 | 보고서 §2-§9 (행당 시드 재현), `ExperimentSmokeTest` |
| R8 | 와이어: 스냅샷 30Hz + reliable 이벤트 exactly-once (기획서 §4.3-4.4) | protocol-spec §3-§5 | `protocol/`(std-only), ReliableEndpoint(ack_bits·RTO·디덥) | `ReliableTest.*`, `SalvoEventsArriveExactlyOnce`, 카오스 주입 E2E |
| R9 | 대역폭: 실전 ~100 엔티티 <256kbps/클라 (기획서 §10.3) | protocol-spec §5b | v4 델타(CV 잔차, acked-baseline) | performance-report §2 (407→**188kbps**), `DeltaStreamCutsDownlink...` 가드 |
| R10 | 틱 예산: p99 < 8ms @ 스트레스 500 엔티티 (기획서 §10.3) | performance-report §1 | 틱 히스토그램+8ms 카운터, solve 백오프 | performance-report §3 (p99 ≤ 512µs, 초과 0.24%) |
| R11 | 불량환경 내성: 유실·중복·재정렬 (기획서 §10.3) | protocol-spec §8 | `tools/net_chaos_proxy`(시드 고정) | `ChaosProxy...`, `DeltaStreamSurvivesChaosLoss` |
| R12 | 재접속: 토큰 좌석 복구 + AAR-완전 이력 (기획서 §4.8·§5.8) | protocol-spec §5c·§6 | 토큰 세션, incarnation nonce, kEventBacklog | `TokenReconnect...`, `StaleUdpHello...`, `LateJoinerCatchesUpViaEventBacklog` |
| R13 | 리플레이/AAR: 저널 재생 비트 동일 (기획서 §5.8) | simulation-models §2, protocol-spec §9b | 샌드박스 --journal-in/out, 서버 --replay | `--expect-final-hash` 데모, `ReplayModeStreamsTheRecordedEngagement` |
| R14 | 클라 표현 계층 분리 + 보간 (기획서 §7) | client-design.md | `client/core/`(std-only 세션·재조립·보간), UE 래퍼 | `ClientCoordsTest/SnapshotAssemblerTest/InterpolationBufferTest`, `ClientSessionTest`(실서버 E2E) |
| R15 | 검증 문화: 매트릭스 + CI (기획서 §10) | 기획서 §10.1 | Debug/Rel/ASan/TSan/Docker + CI 6 jobs | 테스트 224개(양플랫폼 224/224), 본 표의 증빙 전부 CI 상시 |
| R16 | 클라 프레임 예산: 1440p 60fps, "예산 내 최대 품질" (기획서 §11) | client-design §8, **client-runbook §5(자동 계측)**, performance-report §6 | 측정 자동화: `perf_capture.sh`/`perf_summary.sh`(1440p60 PASS/FLAG 판정 — 매 인게임 테스트의 마감 단계), `ASeaWorldManager` `PERF:` 라인(p95/p99 히스토그램·**over-33.3 게이트**)+>100ms 히치 타임스탬프, `-SeaProfileGPU`(패스별 GPU 로그 덤프); 예산 노브 `DefaultEngine.ini [SystemSettings]`(워터 `TessFactorBias`/`LODScaleBias`/`LODCountBias`, 클라우드 `VolumetricRenderTarget.Mode2/Scale`, `ScreenPercentage`), VFX 워밍업 | **performance-report §6**: 워터 품질-상향 회귀 129.9ms→**12.1ms**(네이티브 1440p, 정적 씬 **60.0fps PASS**) — SingleLayerWater 121→1.5ms·클라우드 8→3ms, 수평선 무패턴 캡처 검증; 게임부하 2차 스파이크(over-33.3 4.2%→**0%**)도 자동 계측이 격리(`Spike:` 스레드 분해)·수정 — 자동 사수 `SeaBallistics::SolveAim` 전수 그리드(48 ms)를 coarse-to-fine(~5 ms)으로, 게임부하 **PASS(76 fps, sub-30 0개)**, §6.3 |
| R17 | 운용 콘솔은 서버 *추정* 스트림만 표시 — 진실 비표시 (기획서 §5.5·§7) | client-design §6 | `USeaPpiWidget`/`USeaFireControlPanel`(SLeafWidget), (kind,subject,tick) 이벤트 디덥, 역할/welcome 게이팅 | 인게임 캡처(NTDS 심볼·σ링·PIP·산포원·TALLY), 실교전 이벤트 정합(확인1+발사16+착수16, 중복·유실 0) |
| R18 | 시드 날씨가 비주얼을 구동 — 환경 모델의 시각화 (기획서 §5.6·§13) | client-design §6 | `ASeaEnvironmentController`: 풍속·거스트→거스트너 스펙트럼, 강우→안개+스트릭 볼륨(낙하 벡터=종단속도+시뮬 바람), 항적 리본 바람 드리프트 | 잔잔(2.1m/s)↔강풍(15.8m/s) A/B 캡처, 강우(38%) 캡처, 시연 영상(`client/SeaShield/Saved/Demo/`) |
| R19 | 플레이어 조타 함선 + 회피 — 고정 플랫폼→기동 (P7+ 생존게임) | simulation-models §8b, protocol-spec §4.1·§11(v5), client-runbook §3 | sim `OwnShip` 운동학 + 발사/레이더/종말호밍/누수의 자함 재정렬, `terminal_turn_rate_max`(회피), `steer` 저널; protocol `kOwnShip`/`kShipCommand`(v5); server `handle_steer`; 클라 `SteerShip`/`GetOwnShip`·A/D/W/S 타·카메라+프리깃 추종·`SeaBallistics` 자함속도 승계 | `ManeuveringShipDodgesTurnRateLimitedASM`(메커니즘)·`ManeuveringDodgesAsmsAcrossManyGeometries`(24지오메트리: 정지 24/24 피격↔빔 회피 0/24)·`OwnShipIntegratesFromSteerCommands`·`FixedPlatformIgnoresSteering`(N=1 비트불변), `OwnShipSteeringMovesTheShipEntityOverTheWire`(와이어 E2E), `SteerJournalRoundTrips`/`ReplayWithSteeringMatchesOriginalRun`, `ShipCommandRoundTrips`/`OwnShipEntityRoundTrips`; 양플랫폼 골든 재생성, 인게임 쿼터뷰 캡처(프리깃·카메라 기동) |

**읽는 법**: 모든 행의 증빙은 레포에서 재현 가능하다 — 테스트는 `ctest`, 수치는 해당
보고서의 명령줄, 골든은 `SEASHIELD_UPDATE_GOLDEN` 절차(변경 시 커밋 메시지에 사유).

# 요구사항 추적성 매트릭스 (V-model)

> Project SeaShield — 요구·약속 → 설계 문서 → 구현 → 검증 증빙의 추적표 (P6 산출물,
> 기획서 §9). 수식 단위의 세부 추적은 simulation-models.md 부록 C가 담당하고, 본
> 문서는 체계 수준을 묶는다. 문서 버전: v1.0 (2026-06-12)

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
| R15 | 검증 문화: 매트릭스 + CI (기획서 §10) | 기획서 §10.1 | Debug/Rel/ASan/TSan/Docker + CI 6 jobs | 테스트 213개, 본 표의 증빙 전부 CI 상시 |

**읽는 법**: 모든 행의 증빙은 레포에서 재현 가능하다 — 테스트는 `ctest`, 수치는 해당
보고서의 명령줄, 골든은 `SEASHIELD_UPDATE_GOLDEN` 절차(변경 시 커밋 메시지에 사유).

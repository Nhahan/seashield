# UE5 클라이언트 설계 (Client Design)

> Project SeaShield — P5 클라이언트(표현 계층)의 구현 기준 문서.
> 상위 문서: [01-기획서.md](../01-기획서.md) §7 (UE5 클라이언트 개요), §3.2 (운용석), [protocol-spec.md](protocol-spec.md) v1.3.
> 운용 절차(빌드·에셋·검증·계측·패키징·녹화)는 [client-runbook.md](client-runbook.md).
>
> 문서 버전: v1.2 (2026-06-13) · 상태: **K0~K6 완료** — HUD(PPI+사통 콘솔)·날씨 비주얼·교전 VFX·1440p 60 fps 계측·패키징·시연 영상까지. 잔여는 후속 백로그(§9).
> v1.1: K4/K5 핵심(SLeafWidget HUD·코드 VFX·스테이지 오프셋) 반영. v1.2: K5 완결+K6
> (사통 패널, 강우, far skirt, 계측 이력·패키징, 런북 분리) 반영.

---

## 1. 설계 원칙 — "클라이언트는 표현 계층이다" (기획서 §7)

판단 로직은 전부 서버에 있고, 클라이언트는 **수신 → 보간 → 표시 → 명령 송신**만 한다.
이를 구조로 강제하기 위해 클라이언트 로직을 두 층으로 쪼갠다:

| 층 | 위치 | 의존성 | 검증 |
|---|---|---|---|
| **클라 코어** (세션·재조립·보간·좌표) | `client/core/` | std + `protocol/` (POSIX 소켓) | **gtest 헤드리스** — 실서버 E2E 포함 |
| **UE 래퍼** (스레드 마샬링·액터·UI) | `client/SeaShield/Source/` | UE5 + 클라 코어 | UE 에디터/PIE |

UE가 없어도 클라이언트의 *판단 가능한 전부*가 테스트된다 — 서버의 sim/net 분리(§4.1)와
같은 철학의 클라이언트판.

## 2. 모듈 구조와 소스 공유

```
client/SeaShield/
├ SeaShield.uproject            (UE 5.7, Water·Niagara 플러그인)
└ Source/
  ├ SeaShieldCore/              ← 브리지 모듈: 레포 소스를 직접 컴파일
  │  ├ repo_protocol -> ../../../../protocol      (심볼릭 링크)
  │  ├ repo_client_core -> ../../../../client/core (심볼릭 링크)
  │  └ SeaShieldCore.Build.cs   (C++20, PCH/유니티 빌드 끔, 레포 루트 include)
  └ SeaShield/                  ← 게임 모듈 (UE 의존 코드만)
```

- **단일 진실원**: protocol/클라 코어 소스는 복사·서브모듈 없이 심볼릭 링크로 UBT에
  노출된다. CMake와 UBT가 같은 파일을 컴파일하므로 와이어 코드가 갈라질 수 없다
  (기획서 §11 "UE↔protocol 링크 스파이크"의 이행 형태). 제약: 심볼릭 링크는
  macOS/Linux 체크아웃 전제 — Windows 클라가 필요해지면 UBT 사전 빌드 스텝으로 교체.
- 공유 소스는 예외/RTTI 불사용(`throw` 0개)이라 UE 기본 빌드 플래그와 충돌하지 않는다.

## 3. 스레딩 모델

```
[네트워크 스레드 (FRunnable)]                  [게임 스레드]
 ClientSession::run()                          USeaNetSubsystem::Tick()
   TCP 핸드셰이크 → UDP 바인드 → 수신 루프      ├ TQueue 드레인 (SPSC)
   콜백: welcome/snapshot/event/solution  ───►  ├ SnapshotAssembler → InterpolationBuffer
   request_fire() ◄───────────────────────────  ├ FireSolution 캐시 (트랙별 최신)
   (뮤텍스 큐, TCP로 송신)                      └ 델리게이트 브로드캐스트(이벤트)
```

- 와이어는 네트워크 스레드에만 존재한다. 게임 스레드는 protocol 구조체를 큐로 받아
  소비할 뿐 소켓을 모른다 — 서버의 sim↔I/O 분리(§4.6)와 대칭 구조.
- `ClientSession`은 더미 클라이언트와 동일한 검증된 상태기계(TCP 프레이밍 → UDP 헬로
  반복 → keepalive·flush·poll 루프)다. 실서버 상대 E2E 테스트
  (`tests/client_session_test.cpp`)가 핸드셰이크→스냅샷→발사→이벤트 왕복을 고정한다.

## 4. 스냅샷 파이프라인 (기획서 §4.7)

1. **SnapshotAssembler**: 배치(`first_index/total_entities`)를 틱 단위로 재조립.
   비신뢰 전송 전제 — 순서 역전·중복은 흡수, 완성된 최신 틱보다 낡은 프레임은 폐기
   (완성이 늦은 과거 프레임이 화면을 되감으면 안 된다), 미완성 보류는 64틱 상한.
2. **InterpolationBuffer**: 표시 시계 = 최신 틱 − 6틱(~100ms). 괄호 스냅샷 2점 lerp,
   피드 정지 시 속도 외삽을 **0.25s에서 동결**(멈춘 피드는 멈춰 보여야 한다).
3. **트랙(kTrack)은 보간하지 않는다** — 스캔 주기로만 갱신되는 센서 추정을 매끈하게
   잇는 것은 레이더에 없는 연속성을 날조하는 것(§5.5). 스코프·3D 모두 스냅 갱신.
4. **델타 스트림 (protocol v4, protocol-spec §5b)**: 어셈블러가 틱을 완성할 때마다
   `ClientSession::ack_snapshot` → 서버가 그 베이스라인 기준 CV-잔차 델타로 전환
   (다운링크 407→188kbps/클라 실측). `push_delta`는 보유한 베이스라인에만 적용하고,
   미보유면 프레임을 버린다 — 서버의 풀 스냅샷 폴백이 스트림을 치유한다. 바인드
   시점의 TCP 이벤트 백로그(§5c)와 라이브 이벤트의 경계 중복은 (kind, subject, tick)
   디덥으로 흡수(UE 쪽도 동일 키 디덥 필요 — K5 잔여 작업에 포함).

## 5. 좌표 규약 (`client/core/coords.h`)

| | sim (ENU) | UE |
|---|---|---|
| 단위 | m | cm |
| 축 | x동 y북 z상 (우수계) | X전방 Y우 Z상 (좌수계) |
| 매핑 | — | **X=북, Y=동, Z=상** (축 스왑이 곧 손좌표 변환) |
| 방위각 | 북 기준 시계방향 rad | yaw deg — 단위 변환만 |

테스트가 관례를 고정한다(`ClientCoordsTest`) — "어느 축이 어디였더라"가 회귀로 발화.

## 6. 게임 모듈 구성

| 클래스 | 역할 |
|---|---|
| `USeaNetSubsystem` | GameInstance 수명: 네트 스레드 소유, 큐 드레인, Blueprint API(Connect/SampleEntities/FireAtTrack/GetFireSolution/GetWeather/OnEngagementEvent) |
| `ASeaWorldManager` | 보간 샘플 → 액터 스폰·소멸·트랜스폼 + **코드 리본 항적**(위치 이력×바람 드리프트 — 바람에 휘는 연기가 환경 모델의 시각화)·물기둥·에어버스트(이벤트 구동) |
| `USeaPpiWidget`(+`SSeaPpiScope`) | **PPI 스코프 전체를 SLeafWidget 벡터 드로잉으로**(UE 5.7에서 UUserWidget::NativePaint 경유 요소가 화면에 도달하지 않는 문제를 격리하고 슬레이트 리프로 이전): 거리 링·방위 눈금·회전 스윕(트레일 페이드), NTDS풍 트랙 심볼(tentative 흐림/confirmed/coasting 황색), σ 링(실척), 속도 리더, 지정 트랙의 PIP×마커+산포 원(FireSolution 스트림), 클릭 지정(스코프 리프 OnMouseButtonDown → 최근접 트랙) |
| `USeaFireControlPanel`(+`SSeaFcPanel`) | 사통 콘솔 읽기 패널(우하단, 같은 SLeafWidget 패턴): LINK/ROLE, 날씨(풍속·풍향·거스트·강우), 지정 트랙 기동(거리·방위·σ·상태), 사격제원(PIP 거리·방위, ToF, 산포 반경 — 무효 시 "NO SOLUTION" 정직 표시), 교전 TALLY(발사·착수·기폭·격추·최종 miss — reliable 이벤트 누적). 화면의 모든 수치는 서버 추정 스트림 |
| `ASeaEnvironmentController` | ServerWelcome v3 날씨 → 거스트너 스펙트럼 재생성 + MPC 파라미터 + **강우 = 코드 스트릭 볼륨**(카메라 추종 ProceduralMesh — 낙하 벡터 = 종단속도+시뮬 바람이라 항적 리본과 같은 방향으로 기움, 근접 렌즈 페이드·거리 페이드) + 강우→HeightFog 밀도(시계 손실) — **비주얼이 환경 모델의 시각화**라는 v2.1 차별점의 이행 지점 |
| `ASeaShieldGameModeBase` | HUD 생성(PPI+사통 패널 — 뷰포트 슬롯은 AddToViewport 기본 풀스크린 그대로; 5.7에서 SetPositionInViewport가 스트레치를 교란), SpectatorPawn(DefaultPawn의 가시 구체 제거), 레벨 무관 PlayerStart 보장(함교 시점 — 월드 제로 결함지대 스폰 차단), `-SeaShot[FromPawn]` 캡처 경로 |

역할 분기(§3.2): `ServerWelcome.role` 기준 — welcome 전과 Observer는 HUD 숨김.
게이팅은 내부 호스트 위젯의 Collapsed로 구현(외부 UserWidget을 접으면 tick이 멎어
스스로 복귀 불가 — 외부는 항상 보이게 유지). 이벤트는 (kind, subject, tick) 셋
디덥(v4 바인드 백로그와 라이브 UDP의 경계 중복 — client_session이 의도적으로
소비자에 위임, E2E에서 확인 1+발사 16+착수 16=33 정합 검증). 사격 입력 폼
UI(살보·산포 슬라이더)는 후속; 데이터·명령 경로는 서브시스템 API로 노출되어 있다.

## 7. 절차적 에셋 파이프라인 (`tools/assets/`)

`asset_lib.py`(로프트·경사 박스·회전체·핀·LOD·FBX·프리뷰 렌더) + 생성기 4종.
**LOD가 생성기의 파라미터**라 폴리 버짓이 사후 작업이 아니라 사양이다(§11 v2.1).

| 생성기 | LOD0~3 tris | 형상 |
|---|---|---|
| frigate.py (122m) | 418/214/178/130 | 로프트 차인 선체+텀블홈, 경사 패싯 상부구조·통합 마스트·VLS 4×4·함포 |
| missile.py (5.2m) | 1440/576/264/128 | 오자이브+X 날개·핀, 라돔 슬롯 분리 |
| rocket.py (2.4m) | 816/304/168/60 | 의도적 단순 — §2.4 전제 |
| launcher.py | 1008/624/60/60 | Base/Mount/Tubes 3분할(선회·고각 구동), 4×4=16발 |

루프: 스크립트 → 헤드리스 렌더 → 이미지 검수 → 수정. 산출물(out/)은 비커밋,
스크립트만 버전 관리 — "물리부터 에셋까지 전부 코드".

## 8. 성능 계측 (기준·이력·결과)

- 기준: **1440p 60fps 고정** (기획서 §11 v2.1). 시나리오: 우천 + 16발 일제사 동시 항적.
- 방법: 기능 단위 채택 전후 `stat unit / stat gpu` + Unreal Insights — 서버 틱 비용
  계측(§10.3)과 같은 규율. 기능별 ms 표를 본 절에 기록한다.
- scalability Low~Epic 유지(사양 타협 설계의 증빙).

계측 이력 (2026-06-12, 2560×1440 윈도우, 에디터 -game 개발 빌드, demo-fire
16발 일제사 + 풀 HUD, steady-state = 첫 8s 워밍업 제외):

| 구성 | 평균 프레임 | 최악 | 비고 |
|---|---|---|---|
| 초기(스크린샷 포함 런) | 18.75 ms (53.3 fps) | 400 ms | max의 정체는 아래 참조 |
| 스크린샷 제거(`-SeaQuit`), 쿼터뷰 | 18.47 ms (54.1 fps) | 28.4 ms | **히치 0건** |
| + 구름 ViewSampleCountScale 0.5 | **16.67 ms (60.0 fps)** | 26.2 ms | **1440p 60fps 목표 도달** |

**계측이 잡은 것 2건** (서버 틱 계측과 같은 "측정→격리→교정" 규율):
1. **"400 ms 일제사 히치"는 계측 아티팩트였다** — >100 ms 프레임에 타임스탬프
   로그를 달아 격리하니 발생 시각이 정확히 `-SeaShot`의 스크린샷 GPU 리드백
   (t=55.41)이었고, 일제사(t≈20)는 단 한 프레임도 100 ms를 넘지 않았다.
   측정 런은 스크린샷을 찍지 않는 `-SeaQuit`로 분리(도구가 측정을 오염시킨 사례).
   초기 가설이었던 첫 사용 PSO 컴파일 히치는 실존하지 않았으나, 예방용 VFX
   워밍업(전 교전 머티리얼을 부팅 시 서브픽셀 스펙으로 1회 드로우)은 유지.
2. **GPU 예산의 최대 소비자는 볼류메트릭 구름** — `stat gpu`(인게임 캡처로 판독):
   총 17.4 ms 중 구름 6.2 / 워터 4.2(프리패스 포함) / 포스트 3.1 / TSR 2.6.
   구름 컴포넌트 ViewSampleCountScale 1.0→0.5로 -1.8 ms, 캡처 대조로 시각 열화
   없음 확인(뭉게구름 형태·라이팅 유지). 르멘은 비활성(SSR/SSAO 경로)이며 이
   장면 특성상 의도적 선택으로 문서화.

| 기능 | GPU ms (stat gpu, 1440p, 튜닝 전) |
|---|---|
| 볼류메트릭 구름 | 6.22 → 튜닝 후 ~3.4 |
| 바다(SingleLayerWater+프리패스) | 4.17 |
| 포스트프로세싱 | 3.13 |
| TSR | 2.64 |
| 스카이 캡처/라이팅/기타 | ~1.8 |
| BasePass(절차 메시 전체) | 0.03 — LOD 사양의 증빙 |

**패키징 빌드 검증** (BuildCookRun Mac Development, pak 스테이징 1.1 GB —
쿡 블로커였던 Water 플러그인 WaterBodyCollision 프로파일은 DefaultEngine.ini에
기입, 에디터의 대화상자가 unattended 쿡에선 하드 에러가 되는 케이스):

| 구성 | 평균 프레임 | 최악 | 비고 |
|---|---|---|---|
| 패키징, demo-fire 16발 + 풀 HUD, 쿼터뷰 | **16.67 ms (60.0 fps)** | 26.5 ms | 에디터 -game과 동일 — 병목이 GPU라 에디터 오버헤드는 측정 한계 미만 |

측정 노트: ① 16.67 ms 고정은 vsync 락(60 Hz) — 미세 초과(over-16.7 ≈ 40%)는
프레젠트 경계의 ±ε. ② vsync 해제(`r.VSync=0`)는 오히려 23.5 ms로 **느려짐** —
macOS Metal 윈도우드의 즉시 present 경로 오버헤드가 측정을 지배(출하 구성=
vsync-on이 측정 기준). ③ scalability는 sg.* cvar로 Low~Epic 전환·렌더 동작
확인 — vsync 락 아래에선 평균 차이가 가려지므로 품질군 비교는 over-budget
비율/stat gpu로 볼 것.

## 9. 현재 상태와 잔여 (P5 체크리스트)

- ✅ K0 서버측 protocol v3 (전 매트릭스 그린) / ✅ K3 에셋 파이프라인 /
  ✅ K1 — SeaShieldEditor 빌드 그린(심볼 가시성 force-include 1건 수정) +
  헤드리스 -game 스모크 + 실서버 E2E(`-SeaServer=host[:port] -SeaRole=` 자동
  접속 → welcome·스냅샷 파이프라인 로그) / ✅ K2 — 액터 측 포함: 클래스 미지정
  킨드는 절차 메시 폴백(AStaticMeshActor, 기수 +Y → -90° yaw 보정), 게임모드가
  매니저·환경 컨트롤러를 BeginPlay에 보장(레벨은 장식만 담당), E2E에서 액터
  스폰·ENU→UE 위치 일치 확인
- 헤드리스 에디터 자동화: `Tools/import_assets.py`(메시 6종 × LOD4 임포트·체인
  조립 — 풀 에디터 `-nullrhi -ExecCmds="py ..."`로 실행, 커맨드릿은 Slate 부재로
  불가), `Tools/setup_level.py`(L_Range 기본 무대 — 실 RHI 필요)
- ✅ K4 핵심(2026-06-12): 날씨→해상 상태 런타임 연동(`AssignGeneratedOceanWaves`
  시드 스펙트럼 — 잔잔 2.1m/s↔강풍 15.8m/s A/B 인게임 검증), 절차 머티리얼 7종
  (`Tools/setup_materials.py` — 헤이즈그레이+월드Z 흘수선 방오 도식), 교전 VFX
  3종(발사·강하 항적 리본, 착수 물기둥, 에어버스트 퍼프 — 전부 인게임 캡처 검수),
  스테이지 오프셋(`SeaWorldFrame::Origin`) — 월드 제로의 Water 결함 회피
- ✅ K5 1단계: PPI 스코프 화면 표출(실서버 트랙·σ링·스윕 라이브), `-SeaFire`
  데모 일제사(솔루션 settle 후), `-SeaShot[OnBurst/OnSplash]` 캡처 인프라
- ✅ K5 완결 + K4 폴리시(2026-06-12 저녁, 전부 인게임 캡처 검증): 사통 패널
  (C++ SLeafWidget 콘솔 — TALLY LCH 16/SPL 16/MISS까지 라이브 검증), PPI 좌하단
  배치(뷰포트 기본 슬롯), 역할/welcome 게이팅, 이벤트 (kind,subject,tick) 디덥,
  강우 스트릭+안개(rain-asm 38% 검증, 3회 튜닝 — 렌즈 스미어 페이드), 수평선
  far-distance 스커트(M_FarOcean 40km — 존 경계 띠 소멸 검증), PlayerStart 함교
  시점+SpectatorPawn, movable 라이트(라이팅 배너 제거), `Tools/patch_level.py`
- ✅ K6 계측(2026-06-13 새벽): 히치 포렌식("400 ms 일제사 히치" = 스크린샷
  리드백 아티팩트로 격리), stat gpu 예산표, 구름 튜닝으로 **1440p 60.0 fps
  도달**, 패키징 빌드(BuildCookRun) 동일 수치 재확인, `-SeaQuit`/`-SeaStat`
  계측 플래그, VFX 워밍업
- ✅ 시연 영상(2026-06-13): `Saved/Demo/seashield_demo.mp4` — 69 s 1440p,
  부팅→HUD 라이브→16발 일제사→항적→착수 탤리. **윈도우 단독 캡처**
  (`Tools/record_window.swift`, ScreenCaptureKit `desktopIndependentWindow`)
  라 OS 다이얼로그/알림이 위에 떠도 녹화에 들어가지 않는다(디스플레이 캡처의
  오염 문제를 격리 후 교체). 재촬영은 `Tools/record_demo.sh` 원터치.
- ⏳ 잔여(후속): 사격 입력 폼 UI(살보·산포 슬라이더)

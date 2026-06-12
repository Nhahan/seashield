# UE5 클라이언트 설계 (Client Design)

> Project SeaShield — P5 클라이언트(표현 계층)의 구현 기준 문서.
> 상위 문서: [01-기획서.md](../01-기획서.md) §7 (UE5 클라이언트 개요), §3.2 (운용석), [protocol-spec.md](protocol-spec.md) v1.2.
>
> 문서 버전: v1.0 (2026-06-12) · 상태: K0~K3 완료(K1 UE 빌드·실서버 E2E, K2 액터 측 포함) / K4~K6 에디터 작업 대기

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

## 6. 게임 모듈 구성 (작성 완료, UE 빌드 대기)

| 클래스 | 역할 |
|---|---|
| `USeaNetSubsystem` | GameInstance 수명: 네트 스레드 소유, 큐 드레인, Blueprint API(Connect/SampleEntities/FireAtTrack/GetFireSolution/GetWeather/OnEngagementEvent) |
| `ASeaWorldManager` | 보간 샘플 → 액터 스폰·소멸·트랜스폼 (클래스는 에디터에서 절차 에셋 BP로 지정) |
| `USeaPpiWidget` | **PPI 스코프 전체를 NativePaint 벡터 드로잉으로**: 거리 링·방위 눈금·회전 스윕(트레일 페이드), NTDS풍 트랙 심볼(tentative 흐림/confirmed/coasting 황색), σ 링(실척), 속도 리더, 지정 트랙의 PIP×마커+산포 원(FireSolution 스트림), 클릭 지정(TrackAtPosition) |
| `ASeaEnvironmentController` | ServerWelcome v3 날씨 → MPC 파라미터(WindDir/WindSpeed/Rain/GustSigma) + 강우 Niagara 스폰 — **비주얼이 환경 모델의 시각화**라는 v2.1 차별점의 이행 지점 |
| `ASeaShieldGameModeBase` | 최소 게임모드 |

역할 분기(§3.2): `ServerWelcome.role` 기준 — Weapons는 PPI+사통 패널+3D, Observer는
자유 시점, 겸임(Solo)은 전체. 사통 패널의 폼 UI(슬라이더·버튼)는 UMG 에디터 작업(K5
잔여)으로, 데이터·명령 경로는 위 서브시스템 API로 이미 노출되어 있다.

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

## 8. 성능 계측 계획 (K6에서 수치 기입)

- 기준: **1440p 60fps 고정** (기획서 §11 v2.1). 시나리오: 우천 + 16발 일제사 동시 항적.
- 방법: 기능 단위 채택 전후 `stat unit / stat gpu` + Unreal Insights — 서버 틱 비용
  계측(§10.3)과 같은 규율. 기능별 ms 표를 본 절에 기록한다.
- scalability Low~Epic 유지(사양 타협 설계의 증빙).

| 기능 | GPU ms (측정 예정) |
|---|---|
| 바다(Water+Gerstner) | — |
| 볼류메트릭 구름 | — |
| Lumen GI/반사 | — |
| Niagara 항적 ×16 | — |
| PPI(NativePaint) | — |

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
- ⏳ 에디터 작업(실 RHI): L_Range 채우기 → 바다/하늘/Niagara → 머티리얼 →
  사통 패널 UMG → 60fps 계측 → 협동 시연 영상

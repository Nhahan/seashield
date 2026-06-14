# UE5 클라이언트 운용 런북 (빌드·에셋·검증·계측·패키징·녹화)

> Project SeaShield — 클라이언트 작업의 **재현 가능한 절차** 모음. 설계 근거는
> [client-design.md](client-design.md), 본 문서는 "어떻게 돌리고 어떻게 검증하나"만
> 다룬다. 모든 절차는 macOS(Apple Silicon, Metal) + UE 5.7 기준이며, 수작업 에디터
> 조작 없이 **CLI만으로 전 과정이 재현**되도록 유지한다 (서버 쪽 ctest 규율의 클라 판).
>
> 문서 버전: v1.0 (2026-06-13)

경로 약칭: `$UE` = `/Users/Shared/Epic Games/UE_5.7`, `$PROJ` =
`client/SeaShield/SeaShield.uproject` (절대 경로 권장 — 셸 cwd에 의존하지 말 것).

---

## 1. 빌드

```sh
"$UE/Engine/Build/BatchFiles/Mac/Build.sh" SeaShieldEditor Mac Development -project="$PROJ"
```

- `SeaShieldCore` 모듈은 레포의 `protocol/`·`client/core/`를 **심볼릭 링크로 직접
  컴파일**한다(단일 진실원). 레포 소스에는 `*_API` 매크로가 없으므로 모듈러 빌드의
  심볼 가시성은 `SeaShieldCoreSymbols.h`(visibility pragma)를 Build.cs
  `ForceIncludeFiles`로 강제 include해서 해결되어 있다 — 이 줄을 지우면 게임 모듈이
  dylib 링크에 실패한다.
- UE 5.7의 `FString::Printf`는 **컴파일타임 포맷 검사**를 한다: 대문자 `%F` 거부,
  `FMath::RoundToInt(double)`은 int64를 돌려주므로 `%d`에 넣으려면 int32 캐스트.

## 2. 에셋 파이프라인 — "전부 코드" (순서 고정)

| 단계 | 도구 | 출력 | 비고 |
|---|---|---|---|
| 0 | `tools/assets/*.py` (Blender bpy) | FBX + 프리뷰 렌더 | LOD가 생성기 파라미터 |
| 1 | `Tools/import_assets.py` | `Content/.../Meshes/SM_*` | LOD 체인 조립 포함 |
| 2 | `Tools/setup_materials.py` | 머티리얼 9종 + 메시 슬롯 할당 | 재실행 = 전부 재생성 |
| 3 | `Tools/setup_level.py` | `L_Range` 전체 재생성 | **파괴적**, 워터 settle 30 s |
| 4 | `Tools/patch_level.py` | 기존 레벨 인플레이스 패치 | 멱등, 3의 재생성 회피용 |

에디터 파이썬 실행법(전 스크립트 공통):

```sh
"$UE/Engine/Binaries/Mac/UnrealEditor" "$PROJ" -nullrhi -unattended -nosplash \
    -ExecCmds="py <레포>/client/SeaShield/Tools/<script>.py"
```

- `-run=pythonscript` **커맨드릿은 쓰지 말 것** — Slate 의존 에디터 API(임포트 완료
  콜백 등)에서 크래시한다. 풀 에디터 + `-ExecCmds`가 정답이며, `-ExecCmds`는 엔진
  init 중에 발화하므로 각 스크립트는 slate post-tick 디퍼로 1틱 미룬 뒤 작업한다.
- 레벨 저장은 워터 인포 메시의 **비동기 빌드 settle 후**에 해야 한다(스크립트가
  처리). 종료 시 teardown 크래시 1건은 무해(에셋은 이미 저장됨).
- 로그는 `~/Library/Logs/Unreal Engine/SeaShieldEditor/SeaShield.log`. 성공 마커:
  `SeaShieldLevel:`/`SeaShieldMaterials:`/`SeaShieldPatch:` 라인.

## 3. 실행·데모 플래그 레퍼런스

서버 먼저(레포 루트): `./build/seashield_server --scenario scenarios/demo-fire.scn`
— **시나리오 `duration_s`가 다하면 스트림이 끝난다**: 클라 부팅(~10 s)을 감안해
런마다 서버를 새로 띄우는 것이 기본 절차다(만료 서버에 붙으면 welcome 게이팅 때문에
HUD가 비어 보인다 — 버그가 아니라 게이팅 동작).

```sh
"$UE/Engine/Binaries/Mac/UnrealEditor" "$PROJ" -game -windowed -ResX=2560 -ResY=1440 \
    -nosplash [플래그...]
```

| 플래그 | 효과 | 구현 위치 |
|---|---|---|
| `-SeaServer=host[:port]` | 부팅 시 자동 접속 (기본 포트 7777) | USeaNetSubsystem |
| `-SeaRole=observer\|commander\|weapons\|solo` | 좌석 요청 | USeaNetSubsystem |
| `-SeaFire=N` | 유효 솔루션 16회 settle 후 N발 일제사 (첫 솔루션 즉시 사격은 전탄 빗남 — 실험 보고서 §2) | USeaNetSubsystem |
| `-SeaShot=초` | 쿼터뷰 카메라 + 스크린샷 + (+3 s) 종료 | GameMode |
| `-SeaShotX/Y/Z/Pitch/Yaw=` | 캡처 카메라 오버라이드 (스테이지 좌표) | GameMode |
| `-SeaShotFromPawn` | 카메라 스폰 없이 함교(PlayerStart) 시점으로 촬영 | GameMode |
| `-SeaShotOnBurst` / `-SeaShotOnSplash` | 첫 기폭/착수 이벤트에 정렬된 캡처 (폴백 종료 100 s) | SeaWorldManager |
| `-SeaTestBurst` | 합성 기폭+물기둥 1회 (비주얼 QA를 교전 운에서 분리) | SeaWorldManager |
| `-SeaQuit=초` | **스크린샷 없이** 종료 — 계측 전용 (§5) | GameMode |
| `-SeaStat=gpu\|unit\|...` | stat 표시 토글 (`-ExecCmds="stat ..."`는 -game에서 무효) | GameMode |
| `-SeaGamePlay` | 생존 게임 자동 사수: 탄착예측기로 요격해를 풀어 추적·사격(웨이브 진행). 마우스 없이 게임 루프를 캡처 검증 | PlayerController |
| `-SeaManualPlay` | **사람 입력 경로** 검증용 자동 사수: 실제 마우스룩(Turn/LookUp)·Fire() 핸들러를 구동하고 화면 SOLUTION(탄착=리드)에서만 발사 — "HUD만으로 사람이 맞출 수 있는가" 검증 | PlayerController |
| `-SeaShotSeq=초` | 지정 간격마다 함교 시점 스크린샷(게임 루프 필름스트립), `-SeaShotSeqQuit=초`(기본 42)에 종료 | GameMode |

### 3b. 생존 게임 모드 (플레이어블)

`scenarios/game.scn`(`game_mode = 1`)은 무유도 로켓으로 끝없이 침투하는 ASM 웨이브를
요격하는 **플레이 가능한 생존 게임**이다. 서버는 단일교전 결정론 경로(리플레이·골든·
전체 테스트)를 그대로 두고, 게임 모드에서만 `game_thread_main`이 웨이브마다 World를
재시드 생성한다(방위/거리/고도/속도·날씨가 매번 다름, 난이도는 웨이브로 램프). 표적이
함선 근방(`kShipHitRangeM` 250 m)에 도달하면 생명 1 감소, 0이면 게임 종료(AAR).
와이어는 라운드 무관 monotonic tick + 풀 스냅샷(델타 off)로 라운드 경계를 안전화.

직접 플레이(마우스 조준): **`client/SeaShield/Tools/play_game.sh`** — 서버를 띄우고
함교 콘솔(마우스 선회, F/Space/LMB 사격, `[`/`]` 살보, `;`/`'` 산포)을 연다. HUD:
조준선·탄착 pipper(앰버)·리드 고스트(시안, 겹치면 SOLUTION 녹색)·위협 readout(거리/접근
속도/TTI)·화면밖 위협 화살표·웨이브/SPLASHED/생명·웨이브 배너.

캡처 검증(마우스 없이): 서버를 **세션 분리**(`python3 -c 'import os;os.setsid()...'`)로
띄우고 에디터를 `-SeaGamePlay -SeaShotSeq=2.5`로 실행, **에디터 생존은 PID가 아니라
명령줄(`pgrep -f SeaServer=...`)로 감시**한다(런처 PID 조기 종료 함정). 서버는 에디터가
끝난 뒤에만 포트 매칭으로 정리 — broad `pkill`은 동시 실행 중인 다른 캡처의 서버까지
죽이므로 캡처 도중 절대 쓰지 말 것.

스크린샷은 `client/SeaShield/Saved/Screenshots/MacEditor/SeaShot.png` — 프로세스
종료 후 2~4 s 늦게 기록되므로(쓰기 레이스) 항상 sleep 후 읽고, 런 전에 이전 파일을
지워 stale 판독을 막는다.

## 4. 시각 검증 절차 (캡처 루프)

1. 서버 새로 기동 (시나리오 선택: `calm-asm`(잔잔)/`storm-asm`(강풍)/`rain-asm`(강우)
   /`demo-fire`(원거리 사격, 120 s)).
2. 클라 `-SeaShot...` 런 → PNG 판독(확대 크롭 포함) → 수정 → 재캡처.
3. 이벤트 정렬이 필요한 장면(기폭·물기둥)은 타이머가 아니라
   `-SeaShotOnBurst/-SeaShotOnSplash`로 찍는다 — 런마다 부팅 선행시간이 달라
   고정 시각 캡처는 빗나간다.
4. 에디터 오프스크린 캡처(`Tools/capture_level.py`, SceneCapture2D)는 레벨 검수용 —
   워터 인포 등 **실 파이프라인 검증은 반드시 인게임**(-game) 캡처로 한다
   (unattended 에디터는 뷰포트를 렌더하지 않아 HighResShot이 항상 검정).

## 5. 성능 계측 절차

- **측정 런은 `-SeaQuit`로 끝낸다.** `-SeaShot`의 스크린샷 GPU 리드백 자체가
  ~400 ms 프레임이라 max/avg를 오염시킨다(원인 격리 이력: client-design §8).
  카메라 구도는 `-SeaShot=999 -SeaQuit=55`처럼 조합하면 "카메라만 설정, 촬영 없음".
- 프레임 통계는 SeaWorldManager가 EndPlay에 로그(steady-state: 첫 8 s 워밍업 제외,
  100 ms 초과 프레임은 타임스탬프와 함께 별도 로그 — 히치의 *시각*이 원인 격리의
  핵심이다). 로그 수집: 에디터 -game은 `-abslog=경로`, 패키징 빌드는 stdout.
- GPU 내역은 `-SeaStat=gpu` + `-SeaShot` 캡처로 판독한다.
- **vsync 주의 2건**: ① 60 Hz 락 환경에서 평균 16.67 ms는 "정확히 60 fps"라는 뜻 —
  품질군 비교는 평균이 아니라 over-budget 비율/stat gpu로. ② `r.VSync=0`은 macOS
  Metal 윈도우드에서 즉시 present 경로가 오히려 느려(23 ms대) 헤드룸 측정에
  부적합 — vsync-on(출하 구성)이 측정 기준.
- scalability 스위치: `-dpcvars="sg.PostProcessQuality=0,..."` (sg.* 10종).

## 6. 패키징 (BuildCookRun)

```sh
"$UE/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun -project="$PROJ" -noP4 \
    -platform=Mac -clientconfig=Development -build -cook -stage -pak \
    -archive -archivedirectory=<적당한 경로>
```

- **실행물은 `Saved/StagedBuilds/Mac/SeaShield.app`(pak 포함, ~1.1 GB)** —
  archive 디렉토리에는 콘텐츠 없는 바이너리 .app만 복사되니 헷갈리지 말 것.
- 쿡 전제: Water 플러그인의 `WaterBodyCollision` 콜리전 프로파일이
  DefaultEngine.ini에 있어야 한다(에디터에선 "추가할까요?" 대화상자지만 unattended
  쿡에선 하드 에러 — ini에 기입 완료). 쿡 에러는
  `~/Library/Logs/Unreal Engine/LocalBuildLogs/Cook-*.txt`에서 `Error:`로 찾는다.
- 패키징 빌드도 §3의 Sea* 플래그를 전부 받는다(검증·계측 절차 동일).

## 7. 시연 영상

```sh
client/SeaShield/Tools/record_demo.sh   # 원터치: 컴파일→서버→게임→녹화→인코딩
```

- 캡처는 `Tools/record_window.swift`(ScreenCaptureKit, 윈도우 단독) — 게임 윈도우의
  레이어만 합성하므로 **OS 다이얼로그·알림이 위에 떠 있어도 녹화에 들어가지
  않는다**. 디스플레이 전체 캡처(`screencapture -v`)는 떠 있는 오버레이를 그대로
  담으므로 쓰지 않는다.
- 제약: 화면 기록 권한 + WindowServer 접근 가능한 셸 필요(headless/ssh/샌드박스
  셸에선 CGS_REQUIRE_INIT abort). CLI에서 ScreenCaptureKit 초기화 전
  `NSApplication.shared` 선행 필수(간헐 크래시 방지 — 소스 주석 참조).
- 캡처는 윈도우 크롬 포함(높이 +32 px) → 인코딩 단계에서 crop. 오프라인 프레임
  덤프(-DumpMovie)는 고정 타임스텝이라 실서버 스트림과 어긋나 이벤트 구동 VFX가
  디싱크되므로 **서버 동기 클라에는 부적합**.

## 8. 플랫폼 함정 카탈로그 (Metal / UE 5.7 — 전부 실증·격리됨)

| 증상 | 원인 | 대응 |
|---|---|---|
| 월드 원점 ~512 m 수면 결함 | Water 존 데이터 GPU 버퍼의 월드 제로 고정 결함(20여 회 프로브로 카메라·존·스플라인·메시 무관 입증) | 스테이지 전체를 `SeaWorldFrame::Origin`(3 km,3 km)으로 오프셋. 좌표 규약은 client-design §5 |
| 수면을 가리는 "구덩이" | 오션의 *dilated* 워터-인포 메시가 메인 depth 패스에 누설 | 해당 컴포넌트만 `hidden_in_game`(setup_level.py) |
| 존 중심 사각 결함(별건) | CustomRenderPasses 워터-인포 경로 | `r.Water.WaterInfo.RenderMethod=1` (ini) |
| 원거리 수면 타일 붕괴 | 쿼드트리 타일 상한(256) 초과 | `r.Water.WaterMesh.MaxWidthInTiles=1024` (ini) |
| UMG 드로잉이 화면에 안 나옴 | `UUserWidget::NativePaint` 경유 요소 미도달(스톡 UImage는 정상 — 대조로 격리) | SLeafWidget으로 페인터 이전 (PPI·사통 패널 패턴) |
| HUD가 좌상단에 뭉침 | `SetPositionInViewport`류가 풀스크린 스트레치 슬롯을 교란 | AddToViewport 기본 슬롯 + 위젯 내부 캔버스 슬롯만 사용 |
| HUD가 영영 안 나타남 | 접힌 UserWidget은 tick이 멎어 스스로 복귀 불가 | 게이팅은 내부 호스트 위젯만 Collapsed |
| `stat gpu`가 안 켜짐 | `-ExecCmds="stat ..."`가 -game에서 무효 | `-SeaStat=` 플래그(타이머 후 GEngine->Exec) |
| 측정 max가 항상 ~400 ms | 스크린샷 GPU 리드백(계측 도구의 오염) | 측정 런은 `-SeaQuit` (§5) |
| 에디터 파이썬 크래시 | pythonscript 커맨드릿의 Slate 부재 / -ExecCmds의 init 중 발화 | 풀 에디터 + post-tick 디퍼 (§2) |
| 태양이 수평선 아래로 | `unreal.Rotator` 인자 순서는 (roll, pitch, yaw) | 캡처 루프로 검출 — 각도류는 반드시 렌더 검수 |
| 라이팅 리빌드 배너 | Stationary 라이트 + 베이크 데이터 없음 | 전 라이트 Movable(완전 동적 — patch/setup_level.py) |
| 일제사 직후 첫 VFX 히치 가능성 | 첫 드로우 시점 셰이더/PSO 컴파일 (frustum 밖 워밍업은 무효 — Metal은 그릴 때 만든다) | 부팅 시 카메라 앞 서브픽셀 스펙으로 전 교전 머티리얼 1회 드로우(WarmupVfx) |
| 메시 치수 1/100 | FBX 단위 체인 | 생성기 `FBX_SCALE_ALL` + 임포트 치수 assert (import_assets.py) |

런북 갱신 규칙: 새 함정을 격리하면 **증상-원인-대응 한 줄**을 이 표에 추가하고,
원인 격리에 쓴 실험(대조 프로브·캡처 쌍)은 해당 설계 문서에 남긴다.

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
| 0 | `tools/assets/*.py` (Blender bpy) | FBX + 프리뷰 렌더 | LOD가 생성기 파라미터. frigate=함선 지오메트리(레이더면·마스트·CIWS·난간 등 디테일) |
| 0b | `tools/assets/textures.py` (numpy/PIL) | 절차적 PBR 디테일맵 + 스프라이트(`out/textures/*.png`) | 타일 노멀+RAO(패널/웨더링) · T_Smoke/T_Flash(연기/머즐 스프라이트). 결정론 시드 |
| 1 | `Tools/import_assets.py` | `Content/.../Meshes/SM_*` | LOD 체인 조립 포함 |
| 1b | `Tools/import_textures.py` | `Content/.../Textures/T_*` | 노멀/마스크/스프라이트 압축·sRGB 설정. 머티리얼이 트라이플래너로 샘플 |
| 2 | `Tools/setup_materials.py` | 머티리얼 15종(트라이플래너 PBR 디테일·부위별 슬롯 할당·연기 빌보드 퍼프·머즐 스프라이트 포함) | 재실행 = 전부 재생성. 순서: assets→textures→import_assets→import_textures→materials |
| 1+2′ | `Tools/reimport_frigate.py` | `SM_Frigate`만 재임포트 + 슬롯 재배정 | **빠른 함선 지오 반복용 단축** — 머티리얼 재생성/오션 참조를 안 건드림(설정상 `setup_materials` 풀 재실행은 `MI_SeaOcean`을 delete/recreate해 레벨 참조를 댕글시킴). frigate.py만 고쳤을 때 1·2 대신 사용 |
| 3 | `Tools/setup_level.py` | `L_Range` 전체 재생성 | **파괴적**, 워터 settle 30 s |
| 4 | `Tools/patch_level.py` | 기존 레벨 인플레이스 패치 | 멱등, 3의 재생성 회피용 |
| 5 | `Tools/build_niagara.py` | `Content/.../VFX/NS_Spray` (Niagara 공중 스프레이) | **표준 -ExecCmds 아님** — 풀 에디터 + 저작-타임 에디터 브리지(별도 설치) 경유 선언적 저작. 멱등(재실행=재생성). 런타임은 1st-party Niagara만 의존. 수면 포말은 머티리얼(M_Ocean)+레거시 mesh wake가 담당, Niagara는 공중 스프레이만 |
| 6 | `Tools/apply_ocean.py` (**real-RHI**, `-windowed` 비-nullrhi) | from-scratch 오션 결선(SM_Ocean near patch + M_OceanFar far-skirt, SLW WaterBody 숨김) | env로 타겟·변형 제어. **기본값 = 기존 throwaway `L_RangeCustom` 캡처맵 경로(무변경)**. `SEA_TARGET_MAP=/Game/SeaShield/Maps/L_Range` = **게임 기본 맵을 M_Ocean으로 통일**(부력↔보이는 파도 동기). `SEA_PLANAR=0` = PlanarReflection 생략(Metal forward-translucent엔 무효+per-frame 비용). `SEA_FOG_RETUNE=0` = 게임플레이 fog 유지(캡처용 god-ray 재튠 생략). `SEA_OCEAN_LITE=1` = **굴절 제거 게임플레이 트윈 `M_OceanGame`**을 오션 메시에 결선(Distortion 패스 제거 = 1440p60 게이트 통과). 통일 호출: `SEA_TARGET_MAP=…/L_Range SEA_PLANAR=0 SEA_FOG_RETUNE=0 SEA_OCEAN_LITE=1` (real-RHI라 translucent 셰이더 컴파일 필요 → `-windowed`, NOT `-nullrhi`). perf 근거 = perf-report §6.23 |

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
  `SeaShieldLevel:`/`SeaShieldMaterials:`/`SeaShieldPatch:`/`SeaShieldReimport:` 라인.
  헤드리스 실행 시 `-abslog=<file>`로 전체 엔진 로그를 파일로 스트리밍하면 부팅 침묵 구간에도
  성장하는 출력으로 stall 오탐을 피하고 마커를 폴링할 수 있다(`run_watched` 200 s stall이 콜드
  부팅 중 stdout 침묵에 오작동한 사례). `-ExecCmds="py <path>"`의 경로에 **내부 따옴표 금지**
  (경로에 공백 없음) — `py "<path>"`는 UE 파서가 `py ` 빈 명령으로 깨뜨린다.

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
| `-SeaShotTrack` | 캡처 카메라가 **항행 자선을 매 프레임 추적**(look-at + 라이브 focal). 정적 쿼터뷰는 이동 함선을 놓침(focal/프레이밍이 함선=Origin 가정). 이때 `-SeaShotX/Y/Z`는 ship-상대 오프셋, Pitch/Yaw는 무시(look-at이 산출). `cinematic_shot.sh --track` | GameMode |
| `-SeaShotOnBurst` / `-SeaShotOnSplash` | 첫 기폭/착수 이벤트에 정렬된 캡처 (폴백 종료 100 s) | SeaWorldManager |
| `-SeaTestBurst` | 합성 기폭+물기둥 1회 (비주얼 QA를 교전 운에서 분리) | SeaWorldManager |
| `-SeaQuit=초` | **스크린샷 없이** 종료 — 계측 전용 (§5) | GameMode |
| `-SeaStat=gpu\|unit\|...` | stat 표시 토글 (`-ExecCmds="stat ..."`는 -game에서 무효) | GameMode |
| `-SeaProfileGPU=초` | 워밍업 후 ProfileGPU 1회를 **로그로** 덤프(패스별 GPU ms — 오버레이/스크린샷 판독 불필요). `-SeaQuit`와 병용, `perf_capture.sh --gpu`가 사용 | GameMode |
| `-SeaGamePlay` | 생존 게임 자동 사수: 탄착예측기로 요격해를 풀어 추적·사격(웨이브 진행). 마우스 없이 게임 루프를 캡처 검증 | PlayerController |
| `-SeaManualPlay` | **사람 입력 경로** 검증용 자동 사수: 실제 마우스룩(Turn/LookUp)·Fire() 핸들러를 구동하고 화면 SOLUTION(탄착=리드)에서만 발사 — "HUD만으로 사람이 맞출 수 있는가" 검증 | PlayerController |
| `-SeaShotSeq=초` | 지정 간격마다 함교 시점 스크린샷(게임 루프 필름스트립), `-SeaShotSeqQuit=초`(기본 42)에 종료 | GameMode |
| `-SeaSteerDemo` | 무인 조타 데모: 전속 전진 + 6 s마다 타 반전(S-위빙), **사격 없음** — 함체/카메라 기동과 선회율 제한 ASM의 회피(dodge)를 캡처 검증 | PlayerController |
| `-SeaCinematic` | 시네마틱/포토 티어 마커: 적용된 cvar(해상도·워터 테셀·반사·Lumen)을 로그로 확인. 실제 프로파일은 `--cinematic`이 `-dpcvars`로 주입(`Tools/cinematic.cvars`) | GameMode |

### 3b. 생존 게임 모드 (플레이어블)

`scenarios/game.scn`(`game_mode = 1`)은 무유도 로켓으로 끝없이 침투하는 ASM 웨이브를
요격하는 **플레이 가능한 생존 게임**이다. 서버는 단일교전 결정론 경로(리플레이·골든·
전체 테스트)를 그대로 두고, 게임 모드에서만 `game_thread_main`이 웨이브마다 World를
재시드 생성한다(방위/거리/고도/속도·날씨가 매번 다름, 난이도는 웨이브로 램프). 자함은
플레이어가 조타하는 기동 플랫폼(A/D 타·W/S 스로틀)이라, 종말 선회율이 제한된 ASM은
하드 빔 기동으로 회피(dodge)할 수 있다(simulation-models §8b). 적의 공격(함선 피격)은
`game_enemy_attack`로 on/off: **on**이면 표적이 함선 근방(`kShipHitRangeM` 140 m)에
도달 시 생명 1 감소·0이면 게임 종료(AAR); **off(현재 game.scn 기본)**면 함선 무적 —
표적이 슬쩍 지나가도 무피해, 끝없는 런(함선 피해 모델 정식화 전 개발용).
와이어는 라운드 무관 monotonic tick + 풀 스냅샷(델타 off)로 라운드 경계를 안전화.

직접 플레이(마우스 조준): **`client/SeaShield/Tools/play_game.sh`** — 서버를 띄우고
함교 콘솔(마우스 선회, F/Space/LMB 사격, `[`/`]` 살보, `;`/`'` 산포, **A/D 타(조타)·W/S
스로틀**로 함선 기동/회피)을 연다. HUD:
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
3b. **항행 중 함선의 히어로/wake 샷**은 `cinematic_shot.sh --track`(=`-SeaShotTrack`) — 정적
   `--cam`은 이동 함선을 놓치므로(focal/프레이밍이 함선=Origin 가정), 트래킹 캠이 자선을 매
   프레임 look-at + 라이브 focal로 따라간다. `--cam X,Y,Z`는 ship-상대 오프셋으로 해석.
4. 에디터 오프스크린 캡처(`Tools/capture_level.py`, SceneCapture2D)는 레벨 검수용 —
   워터 인포 등 **실 파이프라인 검증은 반드시 인게임**(-game) 캡처로 한다
   (unattended 에디터는 뷰포트를 렌더하지 않아 HighResShot이 항상 검정).
5. **프레임 예산 판정으로 마감**: 시각이 OK면 `perf_capture.sh --gpu`(§5)로 1440p60
   PASS/FLAG를 확인한다 — 비주얼을 바꾸는 변경(머티리얼·LOD·라이팅)은 거의 항상 GPU
   비용을 건드리므로, 캡처 루프는 "보기 좋은가 + 예산 안에 드는가" 둘 다로 닫는다.

## 5. 성능 계측 절차 (자동 — 매 인게임 테스트의 마지막 단계)

**모든 인게임 테스트는 프레임 판정으로 끝낸다.** `perf_capture.sh` 한 줄이 측정 런을
돌리고 1440p60 예산 대비 **PASS / MARGIN / FLAG** 판정을 출력한다 — "최적화 진행 여부"를
캡처 루프에 내장한 것(§4 4번):

```sh
client/SeaShield/Tools/perf_capture.sh --gpu          # 게임 부하, 네이티브 1440p, GPU 패스 내역
client/SeaShield/Tools/perf_capture.sh --idle --gpu   # 정적 씬(워터/스카이/클라우드 격리)
client/SeaShield/Tools/perf_summary.sh <기존 -abslog>  # 임의 -game 로그를 사후 판정
```

- 동작: 서버 **세션 분리** 기동 → 에디터 `-game -SeaGamePlay -SeaQuit -abslog`(스크린샷
  없음) → `perf_summary.sh`가 `PERF:`/`Hitch:`/ProfileGPU를 파싱해 판정. 옵션: `--dur`(기본
  30, ≥12 필요), `--res`, `--idle`, `--gpu`, `--port`, `--cvars`(=`-dpcvars` 패스스루 — 워터
  비용 노브 실험용).
- **판정 게이트(중요)**: vsync 환경에서 프레임 DeltaTime은 16.667 ms 주기로 지터한다 →
  정상 60 fps도 ~40%가 16.7 ms를 살짝 넘게 찍히므로 **over-16.7%는 참고용**. 진짜 "60 미스"는
  한 vsync를 통째로 놓쳐 ~33 ms로 떨어진 프레임 = **over-33.3%**와 **p99**로 본다. 절대
  헤드룸은 ProfileGPU의 **GPU 프레임 ms**(vsync 무관).
- **프레임 통계 출처**: `ASeaWorldManager`가 EndPlay에 `PERF:` 한 줄(frames/avg/p95/p99/max/
  over16.7/over33.3, 첫 8 s 워밍업 제외) + >100 ms 히치는 타임스탬프와 함께 로그. >60 steady
  프레임이 있어야 출력되므로 측정 런은 `--dur` 큰 값으로.
- **GPU 패스 내역**: `--gpu`(=`-SeaProfileGPU`)가 ProfileGPU를 **로그로** 덤프 →
  `perf_summary.sh`가 상위 패스(ms)를 출력. 스크린샷 오버레이 판독 불필요(단발 오버레이는
  여전히 `-SeaStat=gpu`로 가능).
- **측정 런은 절대 스크린샷을 찍지 않는다**(`-SeaQuit`): `-SeaShot`의 GPU 리드백이 ~400 ms
  프레임이라 max/avg를 오염시킨다(client-design §8). 시각 검수와 성능 계측은 분리한다.
- **macOS 윈도우드 present 캡**: WindowServer가 윈도우드 -game을 60 Hz로 vsync한다(앱
  `bUseVSync` 무관) → GPU < 16.67 ms면 자동 60 fps 고정(avg=16.67). 헤드룸은 avg가 아니라
  GPU ms로 읽는다. `r.VSync=0`은 Metal 윈도우드에서 오히려 느려 헤드룸 측정에 부적합.
- **열(thermal) 공정 측정 — 스윕엔 쿨다운 필수**: M4 Max GPU는 무거운 런을 연속으로 돌리면
  throttle된다. 쿨다운 없이 80→100→90% 같은 해상도 스윕을 연달아 돌리면 avg가 **스크린%가
  아니라 런 순서대로** 단조 증가해 비교가 뒤집힌다(인공물 — 성능 리포트 §6.4에서 실증). 노브/
  해상도 **비교 스윕은 각 런 전 ~90 s 유휴 쿨다운**으로 측정한다. 첫 런이 가장 cool.
- **현 목표 = ~50 fps(20 ms), 1440p 출력**(3인칭 + 무거운 바다, 품질 우선 — §6.4). 기존
  PASS 게이트(over-33.3 < 1% && p99 ≤ 25 ms)는 60 fps 기준이라 50 fps 목표엔 **보수적 프록시**:
  통과하면 50 fps는 자명히 확보. 내부 해상도는 `r.ScreenPercentage=90`(TSR→1440p)로 굽혀 있다.
- scalability 실험: `--cvars`/`-dpcvars=`로 `r.Water.WaterMesh.TessFactorBias`,
  `r.VolumetricRenderTarget.Mode/Scale`, `r.ScreenPercentage`, `sg.*` 등. 확정값은
  `DefaultEngine.ini [SystemSettings]`에 굽는다(현 워터/클라우드 예산 노브가 거기 있음).

## 5b. 이중 티어 — 게임플레이(예산) vs 시네마틱(화질 무제약)

품질을 올리되 실시간 fps도 지키기 위해 렌더는 **두 티어**로 운용한다.

- **게임플레이 티어(기본)**: `DefaultEngine.ini [SystemSettings]`의 비용-최적화 예산(워터
  테셀 바이어스↓·반사/굴절 half-res·구름 quarter-res·`ScreenPercentage=90`+TSR). 매 변경은
  `perf_capture.sh`로 1440p ~50/60 fps 게이트(§5)를 지킨다. **이 티어는 손대지 않는다.**
- **시네마틱/포토 티어(트레일러·스크린샷 전용, fps 무제약)**: `Tools/cinematic.cvars`가
  **단일 소스**. 워터 풀 테셀·반사/굴절 full-res·구름 full-res·Lumen 반사·네이티브+슈퍼샘플을
  복원한다. `-dpcvars`는 디바이스 프로파일 단계(렌더러/워터메시 빌드 **이전**)에 적용되어
  `[SystemSettings]`를 깔끔히 오버라이드하므로 워터메시 테셀 바이어스까지 런타임 리빌드
  없이 먹는다.

```sh
# 시네마틱 티어 측정/패스 내역(품질만 판정, fps 무제약):
client/SeaShield/Tools/perf_capture.sh --cinematic --gpu --idle
# 트레일러/스크린샷:
client/SeaShield/Tools/cinematic_shot.sh --seq 3 --dur 45          # 게임 루프 필름스트립
client/SeaShield/Tools/cinematic_shot.sh --shot 16 --sp 150        # 프린트급 단발 히어로컷(150% 슈퍼샘플)
client/SeaShield/Tools/showcase_shots.sh _after                    # 비평 4축 레퍼런스 4컷 before/after 재현
```

- `perf_capture.sh --cinematic`: `cinematic.cvars`를 `-dpcvars`로 주입 + `-SeaCinematic`.
  `--cvars`를 병기하면 그 값이 뒤에 붙어 충돌 시 이긴다(-dpcvars는 cvar별 **마지막 값** 채택).
- `cinematic_shot.sh`: 스크린샷 경로(측정 아님). `--seq I`(필름스트립)·`--shot T`(쿼터뷰
  단발)·`--sp PCT`(내부해상도 슈퍼샘플)·`--idle`(정적 바다/하늘). 출력은
  `Saved/Screenshots/MacEditor/`.
- **자기 검증**: `-SeaCinematic`은 부팅 시 실제 적용된 cvar 값(해상도·워터 테셀·반사
  downsample·구름 RT 모드·ReflectionMethod·Lumen 반사 downsample)을 `LogSeaShieldGame`로 1줄
  덤프 → 프로파일이 실제로 먹었는지 ProfileGPU 없이도 확인된다.
- **클린 스틸**: `-SeaCinematic`은 `DisableAllScreenMessages`로 엔진 온스크린 디버그 텍스트
  (dev 경고의 빨간 글씨)를 끈다 — 트레일러 프레임에 오버레이가 안 찍힘. 티어 한정(게임플레이/
  dev 캡처는 경고 표시 유지). UMG 전술 HUD는 영향 없음(별개). 경고는 여전히 로그에 남음.
- **알려진 이슈(→ Part 2 워터에서 수정)**: `M_FarOcean`이 `bUsedWithWater=True` 미설정 →
  인게임에서 **기본 머티리얼로 폴백**(원거리 바다 링이 M_FarOcean 아님). 로그 경고로 노출,
  온스크린은 위 suppression으로 가림. `setup_materials.make_far_ocean`에 플래그 추가 + 리세이브 필요.
- cvar 추가/수정은 **`Tools/cinematic.cvars` 한 곳만** 고친다(두 스크립트가 공유 파싱).

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

## 7. 시연 영상 · 멀티클라 증명

### 7.1 단일 클라 데모 영상

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

### 7.2 협동(멀티클라) 데모 영상

```sh
client/SeaShield/Tools/record_coop.sh [out.mp4]   # 1 서버 + 3 역할 UE 클라 + N 더미 → 4-패널 몽타주
```

- 한 서버에 **역할이 다른 UE 클라 3개**(Commander/Weapons/Observer)를 동시에 띄워
  같은 교전을 보여주고, 여기에 **헤드리스 더미 클라 몇 개**를 같은 서버에 추가로
  붙여(서버가 실제로 여러 클라를 받아 처리한다는 증거를 화면 밖에서도 늘린다)
  스테이션 수를 키운다. **4번째 패널은 라이브 서버 로그 tail**(`role … attached` +
  `transport … closed`)을 PIL 타이틀바 + ffmpeg `hstack`/`vstack` 파이프라인으로
  합성 — 영상 자체가 멀티클라 처리의 자기증명이 된다. 하드 요구사항("여러 클라를
  받아 처리하는 C++ 서버")의 **시각 증명**이자 P5 협동 DoD.
- 캡처는 **반드시 순차(SCStream 하나씩)** — ScreenCaptureKit는 3개 스트림을 동시에
  시작하면 `startCapture`가 영영 반환하지 않고 멈춘다. 클라는 `-SeaQuit`로 길게
  살려 두므로 세 콘솔이 클립 전체에 살아 있고, 클립들은 같은 연속 교전의 몇 초
  간격 샷이다.
- **제약(7.1과 동일)**: 화면 기록 권한 + WindowServer 접근 셸 필요 →
  **headless/ssh/샌드박스 셸에서는 실행 불가**(GUI/Screen-Recording 셸 전용).
  서버 측 증명만 필요하면 §7.3을 쓴다.

### 7.3 헤드리스 멀티클라 증명 (GUI 불필요)

```sh
scripts/multiclient_proof.sh            # 1 서버 + ~11 동시 소켓 → PASS/FAIL 자동 판정
```

- **완전 헤드리스·자기검증** — UE·GUI 없이 권위 sim 서버 하나를 띄우고 실제 C++
  클라(`dummyclient`)를 동시에 다수 붙여, 서버 로그 + 각 클라의 자기보고를
  **PASS/FAIL 평결**로 파싱한다. ctest 규율의 CLI 판이자, 하드 요구사항의 감사
  가능한 증거. 네 가지를 실증한다:
  1. **동시성** — 역할 3개 + 8-클라 부하 배치 = 한 순간 ~11 소켓. 피크는 서버
     로그(`attached`/`closed`)를 시간순 재생해 재구성.
  2. **역할 어태치** — commander/weapons/observer 각각 attach(무장석 1 + 관전 N).
  3. **역할 배타성** — 두 번째 weapons 클라는 `kRoleTaken`으로 거부:
     서버가 `transport N closed: handshake rejected`를 찍고 클라는 비정상 종료.
  4. **느린 클라 격리** — netproxy로 UDP 경로를 손상시킨 클라는 다운링크가 손실로
     열화(스냅샷 수↓·kbps↓)하지만 나머지 정상 클라는 계속 진행(graceful
     degradation + 격리). **권위 있는 send-cap 축출**은
     `SlowClientIsEvictedWithoutHarmingOthers` 통합 테스트로 별도 증명(스크립트가
     `ctest -R`로 즉석 재실행). CLI 더미는 소켓을 즉시 읽으므로 라이브 send-cap
     close를 스스로 유발하지 못함 — 이 점은 요약에 명시된다.
- 유니크 고포트(tcp=7801/udp=7802/proxy=7903)로 다른 서버와 충돌 회피, `trap`으로
  pid/포트매칭 정리(broad `pkill` 금지), 전체 180 s 하드 타임아웃 — **블라인드
  대기 없음**. 산출물: `docs/reports/data/multiclient-proof.log`(전체 서버 로그) +
  `multiclient-proof.summary.txt`(평결). 모든 검사 통과 시에만 exit 0.

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
| Niagara 스프라이트 머티리얼 컴파일 중 에디터 크래시 | lit-volumetric TRANSLUCENT(`TLM_VOLUMETRIC_NON_DIRECTIONAL`) 머티리얼을 **Niagara 스프라이트 정점팩토리 퍼뮤테이션**으로 컴파일 → Metal 셰이더 컴파일러 크래시 | Niagara 스프라이트/리본 머티리얼은 **UNLIT**(선-틴트 emissive, splash/wake 경로와 동일)로. (동일 lit-volumetric이 **ISM 빌보드**(M_Spray·M_RocketTrail)에선 정상 — 정점팩토리 차이) |
| 메시 치수 1/100 | FBX 단위 체인 | 생성기 `FBX_SCALE_ALL` + 임포트 치수 assert (import_assets.py) |
| 1440p에서 7 fps, 워터가 GPU의 93% | 워터 품질 상향(tessellation 10·LODScaleBias +0.5·lod_scale 1.5)이 gerstner 쿼드트리 삼각형 폭증 → SingleLayerWater 121 ms(깊이 프리패스 40 + 드로우 81) | `r.Water.WaterMesh.TessFactorBias/-LODScaleBias/-LODCountBias`로 지오메트리 예산 축소(→~1.5 ms). 원거리 패턴 방지는 LOD가 아니라 MI_SeaOcean 머티리얼이 담당하므로 안전 (perf-report 클라 §) |
| 네이티브가 아닌 ~60% 해상도로 렌더(흐릿) | 첫 실행 하드웨어 오토벤치가 (워터가 느릴 때) 로컬 `GameUserSettings`에 `sg.ResolutionQuality=0` 기입 → 스크린% ≈60 | `DefaultEngine.ini [SystemSettings] r.ScreenPercentage=100`(SystemSettings 우선순위가 scalability를 이김). TSR 업스케일 라인(`1552x873 -> 2560x1440`)으로 검출 |
| 볼류메트릭 클라우드 8 ms(네이티브) | quarter-res 트레이스가 march-bound(샘플캡·섀도·AO 컷 무효) | `r.VolumetricRenderTarget.Mode=2`+`.Scale=0.5`(half-res 재구성)로 ~3 ms. 수평선 상공 구름은 불투명체와 교차 안 해 mode 2 제약 무영향 |
| avg가 항상 16.67 ms인데 GPU는 12 ms | macOS WindowServer가 윈도우드 present를 60 Hz로 vsync(앱 vsync 설정 무관) | 헤드룸은 avg 아닌 GPU ms로 읽기(§5). over-16.7%는 vsync 지터로 부풀음 → 게이트는 over-33.3%/p99 |
| 게임 부하 0.36 s 주기 48 ms **게임-스레드** 스파이크(GPU·렌더 정상) | `-SeaGamePlay` 자동 사수가 `ReaimTimer 0.35 s`마다 `SeaBallistics::SolveAim`(±25°×4-70° 1.5° 전수 그리드 = ~1500 RK4 적분)을 호출 — 사람 마우스 조준은 안 타는 캡처 하네스 전용 경로 | `SolveAim` coarse-to-fine(6°→±5°/1.5°) → 48→5 ms, over-33.3 0%. 스파이크 스레드/단계 귀속은 `Spike:` 포렌식 라인(game/render/reconcile/trails/vfx ms). performance-report §6.3 |
| 일제사 중 over-33.3 1.8%, p99 35·max 55 ms(연기 트레일 재작업 후) | 연기 퍼프 **거리 기반 방출**이 `SampleTrail`→`EmitPuff`(`AStaticMeshActor` 스폰)를 **reconcile 루프 안**에서 호출 → 빠른 로켓이 프레임당 다수 액터 스폰. `Spike:`가 **reconcile 21–44 ms**로 귀속(trails 3.7·**vfx 0.0** = 연기 GPU 무죄). 오버드로 트림은 무효 | **프레임당 스폰 캡**(`kPuffMaxPerCall=2`) + 방출 간격 600→850 cm으로 버스트 상한 → p99 21·max 27·over-33.3 0%. 교훈: 퍼프 액터 스폰 비용은 vfx가 아니라 **호출 위치(reconcile) 타이밍**에 귀속. performance-report §6.6 |
| 부력 결합 시 선체가 수면 위/아래로 떠 어긋남 | `UWaterWavesBase::GetWaveHeightAtPosition`의 `InTime`을 `GetWorld()->GetTimeSeconds()`로 주면 **렌더러가 쓰는 Water 시간과 불일치** → 질의한 파면과 렌더 파면이 다른 위상 | `UWaterSubsystem::GetWaterSubsystem(World)->GetWaterTimeSeconds()`(인스턴스 메서드, static 아님)로 질의 → 동일 위상. 보브/틸트는 `FInterpTo` 저역통과 + 틸트 ±5° 클램프(대형 함선은 평균 슬로프). 큰 파도 GPU 비용(SLW+워터메시)은 진폭으로 조절(×1.7 마진부족 → ×1.5). performance-report §6.7 |

런북 갱신 규칙: 새 함정을 격리하면 **증상-원인-대응 한 줄**을 이 표에 추가하고,
원인 격리에 쓴 실험(대조 프로브·캡처 쌍)은 해당 설계 문서에 남긴다.

# Project SeaShield

[![CI](https://github.com/Nhahan/seashield/actions/workflows/ci.yml/badge.svg)](https://github.com/Nhahan/seashield/actions/workflows/ci.yml)

**함대공 교전 시뮬레이터** — 함정 전투정보실(CIC) 관점에서 공중 위협의 탐지·추적·요격 전 과정을 재현하는 C++ / UDP·TCP 멀티클라이언트 서버 기반 정밀 3D 시뮬레이터.

- 서버: C++20, kqueue/epoll Reactor, TCP+UDP 하이브리드 프로토콜, 60Hz 고정 틱 결정론 시뮬레이션
- 클라이언트: Unreal Engine 5 (C++) — 운용석(지휘/사격통제/관측) 역할 분담
- 시뮬레이션: 레이더 탐지 → 칼만 필터 추적·예측 → 사격통제(PIP·외탄도) → **무유도 로켓탄 일제사** 요격 → 교전 판정 → 리플레이/AAR

## 빌드와 실행

요구사항: CMake ≥ 3.24, C++20 컴파일러 (macOS/Linux). 코어는 외부 의존성 0 (테스트만 GoogleTest).

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure        # 단위 + 통합 + 결정론 회귀 테스트

./scripts/linux-test.sh                           # Docker로 Linux(epoll) 검증
cmake -S . -B build-tsan -DSEASHIELD_SANITIZE=thread \
  && cmake --build build-tsan -j && ctest --test-dir build-tsan   # TSan (address 프리셋 동일)
```

### P2 데모 — 탄도 샌드박스 (외탄도 + 랜덤 현실 기상)

```bash
# 시드 기반 랜덤 기상에서 자동 사격 솔루션으로 일제사
./build/ballistic_sandbox --scenario scenarios/crossing-asm.scn --auto --salvo 8 --dispersion 2

# 같은 절차, 날씨만 3종 바꿔 비교 — 환경이 탄착을 바꾼다
./build/ballistic_sandbox --scenario scenarios/crossing-asm.scn --auto --salvo 8 --compare-weather 7,21,42

# 수동 사격 제원 + 보정, 파라미터 스윕(요격률 표), 궤적 CSV 덤프
./build/ballistic_sandbox --az 30 --el 24 --offset-az 0.5 --sweep el=15:35:2 --dump traj.csv
```

### P1 데모 — 멀티클라이언트 서버 (느린 클라이언트 격리)

```bash
./build/seashield_server --port 7901 --udp-port 7902 --mode broadcast &
./build/loadclient --port 7901 --clients 8 --slow 1 --messages 300 --payload 512
# → 슬로우 클라이언트만 "send queue overflow"로 격리 절단, 나머지 7개는 정상 완주
```

## 구조

```
core/      수학(Vec3)·PRNG(PCG32)·RAII fd·로깅    net/     Reactor(kqueue/epoll), 프레이밍, 세션, UDP
sim/       환경(대기·바람·거스트·기상생성)·외탄도   server/  에코/브로드캐스트 데모 서버
           ·표적·교전판정·사격통제(PIP)·월드        tools/   부하 클라이언트, 탄도 샌드박스, 레퍼런스
           ·저널·상태해시·시나리오                  tests/   GoogleTest (단위·통합·결정론 회귀·골든)
scenarios/ 시나리오 파일                            docs/    기획·설계 문서
```

## 문서

| 문서 | 내용 | 상태 |
|---|---|---|
| [docs/01-기획서.md](docs/01-기획서.md) | 프로젝트 기획서 — 컨셉, 아키텍처, 시뮬레이션 명세, 로드맵, 검증 전략 | ✅ v1.0 |
| [docs/02-어필포인트-면접매핑.md](docs/02-어필포인트-면접매핑.md) | 포트폴리오 어필 요소 → 예상 면접질문 → 증빙 매핑 | ✅ v1.0 |
| [docs/architecture/network-design.md](docs/architecture/network-design.md) | 네트워크 레이어 설계서 — P1 구현 기준, 리뷰 반영 | ✅ v1.1 |
| docs/architecture/ | 프로토콜(P3)/시뮬레이션(P4) 설계 문서 | 예정 |
| docs/reports/ | 유도 성능 실험, 성능 측정 보고서 | 예정 (P4, P6) |

> 본 프로젝트의 모든 도메인 정보는 공개 자료 수준이며, 시뮬레이션 파라미터는 임의의 교육용 값입니다.

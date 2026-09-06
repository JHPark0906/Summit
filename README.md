# Summit

Summit은 직접 만든 GameEngine을 사용하는 학습용 2D 플랫포머의 게임 클라이언트입니다. 이동·점프·캐릭터 프로필, 한글 채팅과 말풍선, TCP/UDP 멀티플레이 연동을 구현합니다.

개발 과정에서 생성형 AI의 도움을 받은 프로젝트입니다.

**이 저장소는 게임 코드 공개본입니다.** `Source/`, `Tests/`, CMake 설정과 문서를 포함하며 `Content/`의 씬, `.gameproject`, `.meta`, 글꼴, 이미지와 오디오는 포함하지 않습니다. 엔진과 연결해 코드를 컴파일할 수 있지만, 플레이와 콘텐츠 회귀 테스트에는 별도로 준비한 호환 자산이 필요합니다. 빈 콘텐츠에서 플레이 가능한 씬을 자동 생성하지 않습니다.

## 의존 프로젝트

| 프로젝트 | 역할 |
| --- | --- |
| [GameEngine](https://github.com/JHPark0906/GameEngine) | 실행 진입점, 렌더링·에셋·입력·UI·오디오·물리와 공통 CMake 규칙. GameEditor와 GameBuilder도 이 저장소에 있습니다. |
| [ServerCore](https://github.com/JHPark0906/ServerCore) | 클라이언트가 사용하는 `FrameCodec.h`와 `DatagramCodec.h`. ServerCore 서버 라이브러리를 설치하거나 링크하지 않습니다. |
| 호환 SummitServer | 멀티플레이 가입·프로필·채팅·명단·이동 중계 서버. 클라이언트 컴파일에는 실행 중인 서버가 필요하지 않습니다. |

현재 플랫폼은 Windows x64입니다. MSVC C++20 컴파일러, Windows SDK, CMake와 Ninja가 필요합니다. 도구 버전과 설치 절차는 [GameEngine 빌드 안내](https://github.com/JHPark0906/GameEngine#빌드-환경)를 함께 확인합니다. 의존 저장소는 자동으로 내려받지 않습니다.

## 코드 빌드

다음처럼 세 저장소를 같은 부모 아래에 둡니다.

```text
workspace/
  GameEngine/
  ServerCore/
  Summit/
```

**MSVC x64 개발자 PowerShell에서 GameEngine 루트로 이동한 뒤** 실행합니다. Summit은 엔진의 `gameengine_add_game_project`를 사용하므로, Summit 루트만 대상으로 하는 독립 CMake 빌드는 제공하지 않습니다.

```powershell
$summitSource = (Resolve-Path ../Summit).Path
$serverCoreSource = (Resolve-Path ../ServerCore).Path

cmake --preset ninja-debug "-DGAMEEDITOR_PROJECT_DIRECTORY=$summitSource" "-DSUMMIT_SERVERCORE_ROOT=$serverCoreSource" -DSUMMIT_BUILD_TESTS=OFF
cmake --build --preset ninja-debug --target Summit

cmake --preset ninja-release "-DGAMEEDITOR_PROJECT_DIRECTORY=$summitSource" "-DSUMMIT_SERVERCORE_ROOT=$serverCoreSource" -DSUMMIT_BUILD_TESTS=OFF
cmake --build --preset ninja-release --target Summit
```

각 configure가 성공한 뒤 해당 build를 실행합니다. 기본 출력은 GameEngine 아래 `x64-ninja/Debug/Summit/`와 `x64-ninja/Release/Summit/`입니다. 이 실행 파일만으로 공개본에 없는 게임 콘텐츠를 대신할 수는 없습니다.

`GAMEEDITOR_PROJECT_DIRECTORY`는 Summit의 소스 루트, `SUMMIT_SERVERCORE_ROOT`는 ServerCore의 소스 루트입니다. GameEngine의 `GameEditor/`, `GameBuilder/`, `GameEngineTests/`, `cmake/`를 포함한 저장소 구조를 유지합니다. 설치된 엔진을 `find_package`로 소비하는 구성은 제공하지 않습니다.

Visual Studio 구성, 콘텐츠 준비, 테스트와 실행 파일 배포는 [빌드와 콘텐츠 준비](Docs/Distribution.md)를 참고합니다. 실제 빌드 검증의 소스·환경·결과는 [검증 결과](Docs/VALIDATION.md)에 기록합니다.

## 코드가 구현하는 기능

- **플레이어:** 좌우 이동, 지면 점프, 캐릭터별 애니메이션, 단방향 발판, 낙사 복귀와 닉네임 표시.
- **프로필·채팅:** 최초 프로필 확정, 서버가 승인한 온라인 프로필 변경, 한글 조합 입력, 채팅 기록과 일시적인 말풍선.
- **멀티플레이:** 비차단 TCP 연결, UDP 이동 협상, 전역 프로필 명단과 가시 범위의 원격 객체, 상태 보간과 제한된 외삽.
- **카메라·오디오:** 플레이어를 따르는 카메라, 화면 비율을 유지하며 채우는 배경, 점프 효과음과 BGM 조절.

입력과 컴포넌트가 연결된 씬에서는 다음 조작을 사용합니다.

| 입력 | 동작 |
| --- | --- |
| `A` / `D`, `←` / `→` | 좌우 이동 |
| `Space` | 지면 점프 |
| `Enter` | 채팅 시작·전송 |
| `Esc` | 프로필·오디오 설정 열기/닫기, 채팅 입력 중에는 취소 |
| 채팅 기록 위 마우스 휠 | 기록 스크롤 |

프로필·채팅이 입력을 사용하는 동안 수평 조작은 멈추지만 중력은 계속 적용됩니다. 서버 연결이 없으면 로컬 플레이를 유지하고 채팅은 로컬 기록과 말풍선에만 표시합니다. 이러한 동작에는 코드가 조회하는 씬 객체와 자산을 준비해야 합니다.

## 소스 구조

| 경로 | 책임 |
| --- | --- |
| [Source/Bootstrap](Source/Bootstrap) | 독립 실행용 글꼴 등록과 bootstrap 팩토리 |
| [Source/Gameplay](Source/Gameplay) | 플레이어 조작·애니메이션·프로필·자산 참조 |
| [Source/Camera](Source/Camera) | 카메라 추적과 배경 크기 조절 |
| [Source/Network](Source/Network) | TCP/UDP, 가입·명단·가시성·원격 상태와 UI 연동 |
| [Source/UI](Source/UI) | 채팅 입력·기록과 말풍선 |
| [Tests](Tests) | 로컬 피어를 사용하는 네트워크 회귀와 콘텐츠·그래픽 회귀 |
| [CMakeLists.txt](CMakeLists.txt) | 게임·공유 컴포넌트·선택적 테스트 타깃 |

`Summit_Components`는 게임과 Editor가 공유하는 OBJECT 라이브러리입니다. `Source/Bootstrap/SummitBootstrap.cpp`는 그 목록에서 제외하고 `Summit` 실행 타깃에만 연결합니다. Editor는 자체 bootstrap을 등록합니다. 컴포넌트 변경 후에는 사용하는 게임·Editor 실행 파일을 다시 빌드합니다.

## 문서와 범위

- [빌드와 콘텐츠 준비](Docs/Distribution.md): 코드 공개본, 선택적 테스트, 플레이에 필요한 자산과 배포 구조.
- [서버 연결과 프로토콜](Docs/ServerConnection.md): 접속 설정, schema 6과 TCP/UDP의 책임.
- [말풍선](Docs/SpeechBubbles.md): 텍스트 측정, 수명, 캐릭터 앵커와 교체 자산 조건.

로컬 이동과 물리는 각 클라이언트가 계산합니다. 서버 권위 물리, 계정 인증·영구 저장, 부정행위 방지 또는 특정 동시 접속 수를 이 클라이언트가 보장하지 않습니다. UDP 세션 토큰은 암호화나 전송 경로의 변조 방지를 대신하지 않습니다.

프로젝트 코드의 라이선스는 [LICENSE](LICENSE)를 따릅니다. 별도로 준비하는 자산의 사용·배포 조건도 확인해야 합니다.

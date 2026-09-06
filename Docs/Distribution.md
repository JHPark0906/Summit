# 빌드와 콘텐츠 준비

공개본은 C++ 소스와 테스트·빌드 설정을 제공한다. `Content/` 전체와 실행 파일, 배포 ZIP, 이전 Git 이력, 로컬 빌드 결과는 포함하지 않는다. 이 문서는 현재 빌드·콘텐츠 계약을 설명하며, 실제 실행 결과는 [검증 결과](VALIDATION.md)에 기록한다.

## CMake 연결

[README의 코드 빌드](../README.md#코드-빌드)는 GameEngine 루트에서 Summit을 하위 프로젝트로 구성한다. Summit의 [CMakeLists.txt](../CMakeLists.txt)는 엔진이 제공하는 `gameengine_add_game_project`를 사용하고 ServerCore의 코덱 헤더를 포함한다. ServerCore의 설치 패키지나 정적 라이브러리는 요구하지 않는다.

| 설정 | 의미 |
| --- | --- |
| `GAMEEDITOR_PROJECT_DIRECTORY` | Summit의 `CMakeLists.txt`가 있는 소스 디렉터리 |
| `SUMMIT_SERVERCORE_ROOT` | 코덱 헤더가 있는 ServerCore 소스 디렉터리. 기본값은 Summit의 형제 `ServerCore` 디렉터리 |
| `SUMMIT_BUILD_TESTS` | 콘텐츠를 사용하는 SummitTests와 두 CTest 항목 생성 여부 |
| `GAMEENGINE_OUTPUT_ROOT` | 엔진과 게임의 구성별 출력 루트. 상대 실행 경로를 해석할 때 현재 CMakeCache 값을 확인 |

Visual Studio를 사용할 때도 GameEngine 루트에서 같은 두 소스 경로를 지정한다.

```powershell
$summitSource = (Resolve-Path ../Summit).Path
$serverCoreSource = (Resolve-Path ../ServerCore).Path
cmake --preset vs "-DGAMEEDITOR_PROJECT_DIRECTORY=$summitSource" "-DSUMMIT_SERVERCORE_ROOT=$serverCoreSource" -DSUMMIT_BUILD_TESTS=OFF
cmake --build --preset debug --target Summit
cmake --build --preset release --target Summit
```

GameEngine의 `vs` 프리셋은 Visual Studio 2026을 지정하므로 해당 생성기를 지원하는 CMake와 도구 설치가 필요하다. 기본 출력은 GameEngine 아래 `x64/Debug/Summit/`, `x64/Release/Summit/`다. Ninja와 Visual Studio는 서로 다른 빌드 트리를 사용한다.

`Source/Bootstrap/SummitBootstrap.cpp`는 Summit 실행 파일에만 들어간다. 나머지 컴포넌트는 `Summit_Components`로 게임과 Editor에 연결된다. 같은 구성에서 `GameEditor` 타깃을 빌드하면 Editor의 bootstrap과 Summit의 컴포넌트를 함께 사용할 수 있다.

## 플레이에 필요한 콘텐츠

게임을 실행하려면 프로젝트 설명자, 시작 씬과 그 씬이 참조하는 자산을 별도로 준비해야 한다. GameEngine의 프로젝트·씬 형식과 에셋 GUID 계약에 맞아야 하며, 이미지나 오디오 파일만 임의로 넣는 것으로 기존 참조가 채워지지는 않는다.

- `.gameproject`는 콘텐츠 루트에 두고 `projectName`, 시작 씬과 렌더링 설정을 구성한다. 코드 루트가 그 부모이면 `sourceRootPath`를 `..`로 지정한다.
- 플레이어·카메라·채팅·네트워크 컴포넌트를 씬에 연결한다. 코드는 `Player`, `NetworkManager`, `BgmAudio` 등 이름으로 객체를 조회하므로 관련 구현과 맞춘다.
- [PlayerController](../Source/Gameplay/PlayerController.h)의 캐릭터·상태별 아틀라스 참조, 프레임 설정과 자산 메타데이터를 함께 구성한다. 교체한 그림의 발·머리 위치와 배경 크기도 확인한다.
- [SummitBootstrap](../Source/Bootstrap/SummitBootstrap.cpp)은 배포 콘텐츠의 `Fonts/NanumSquareNeoOTF-Bd.otf`를 읽어 `Summit UI` 글꼴로 등록한다. 그 바이트를 읽거나 등록하지 못하면 시작을 거절한다. 다른 글꼴을 사용하려면 이 등록 코드와 필요한 문자의 지원 범위를 맞춘다.
- 점프·BGM 등 오디오와 화면 배경도 씬이 참조하는 자산으로 준비한다. 서버 주소와 포트는 [접속 설정](ServerConnection.md)에 맞춘다.

빌드는 콘텐츠를 실행 파일 옆에 스테이징하고, 컴포넌트 스키마를 `Content/Components.schema.json`에 생성할 수 있다. 생성된 스키마는 에셋이나 플레이 가능한 씬을 대신하지 않으며 공개 소스에 넣을 입력 파일도 아니다.

## 선택적 콘텐츠 회귀

새 CMake 캐시에서는 다음 핵심 입력 세 개가 모두 파일로 존재해야 `SUMMIT_BUILD_TESTS` 기본값이 ON이다. 하나라도 없거나 디렉터리이면 기본값은 OFF다.

- `Content/Summit.gameproject`
- `Content/Scenes/InGame.scene`
- `Content/Fonts/NanumSquareNeoOTF-Bd.otf`

명시적 또는 기존 캐시의 ON인데 핵심 입력이 없으면 구성 단계에서 누락 목록을 보고한다. `.meta`를 포함한 전체 회귀 콘텐츠를 복원하거나 OFF로 다시 구성한다. 기존 캐시의 OFF도 콘텐츠를 나중에 추가했다고 자동으로 바뀌지 않으므로, 검사할 때는 ON을 명시한다. 이 핵심 파일 확인은 아틀라스·오디오·씬의 전체 자산 참조가 완전하다는 검증과 다르며, 나머지는 실제 회귀 검사가 확인한다.

회귀 콘텐츠를 준비한 다음 GameEngine 루트에서 실행한다.

```powershell
cmake --preset ninja-debug "-DGAMEEDITOR_PROJECT_DIRECTORY=$summitSource" "-DSUMMIT_SERVERCORE_ROOT=$serverCoreSource" -DSUMMIT_BUILD_TESTS=ON
cmake --build --preset ninja-debug --target Summit SummitTests
ctest --test-dir build/ninja-debug --output-on-failure -R '^Summit\.'
```

위의 두 경로 변수는 앞 절 또는 README처럼 먼저 설정한다. Release는 `ninja-release`와 `build/ninja-release`를 사용한다. Visual Studio 트리는 `build/vs`에 해당하며 CTest에 `-C Debug` 또는 `-C Release`를 전달한다.

| CTest 이름 | 범위 |
| --- | --- |
| `Summit.CharacterProfiles` | 프로필·입력·채팅·말풍선·물리·오디오·카메라와 실제 loopback TCP/UDP 클라이언트 회귀 |
| `Summit.ChatVisuals` | D3D11/12와 UI 배율 1·2에서 채팅·말풍선의 크기·줄바꿈·클리핑 검사 |

이 테스트들은 `Scenes/InGame.scene`, 글꼴, 캐릭터 그림과 오디오 등 로컬 회귀 콘텐츠를 사용한다. 네트워크 검사는 테스트가 만드는 로컬 피어를 사용하므로 별도 서버 실행이 필요하지 않다. 그래픽 검사에는 지원하는 장치가 필요하고, 캡처는 빌드 디렉터리의 부모 아래 `chat-visual-captures/`에 남긴다. 공개본의 테스트 OFF는 이 검사들의 통과를 의미하지 않는다.

## 실행 파일 배포

콘텐츠를 갖춘 게임을 배포할 때는 Release 출력의 Summit 디렉터리를 렌더링 파일과 콘텐츠까지 함께 전달한다. 실행 파일만 복사하면 설정·씬·자산·글꼴이 빠진다. GameBuilder를 사용할 때도 먼저 호환 콘텐츠를 준비해야 한다.

MSVC 런타임 선택, 콘텐츠 팩과 엔진 런타임 파일의 배치 계약은 [GameEngine](https://github.com/JHPark0906/GameEngine)의 빌드 문서를 따른다. 게임 자산을 새로 배포할 때는 각 자산의 사용 조건을 별도로 확인한다. 이 저장소는 실행 가능한 게임 패키지를 제공하지 않는다.

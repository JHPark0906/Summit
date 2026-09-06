# 검증 결과

2026-09-06에 코드 공개 묶음과 별도의 로컬 콘텐츠 환경을 검증했다. 공개 묶음에는 `Content/`, 실행 파일, 빌드 로그와 이전 Git 이력을 포함하지 않는다. 빌드·콘텐츠 준비 방법은 [Distribution.md](Distribution.md)를 따른다.

## 코드 공개 묶음

`Source/`, `Tests/`, CMake와 문서의 40개 파일을 새 디렉터리에 복사하고 파일별 SHA-256을 확인했다. 아래 GitHub 소스를 함께 받아 Windows x64에서 구성했다. 이 검증 문서는 검증 후 추가했으며, 나머지 파일은 빌드한 묶음과 동일하다.

| 항목 | 검증 환경 |
| --- | --- |
| GameEngine | [`ab17edd`](https://github.com/JHPark0906/GameEngine/commit/ab17eddcd853e32270599e7c74db0214aa6b959a) |
| ServerCore | [`9cc9091`](https://github.com/JHPark0906/ServerCore/commit/9cc909169f9bd7c9d7d7a536b2a821942a7ea37f) |
| 생성기 | Visual Studio 18 2026, x64 |
| 도구 | CMake 4.2.3-msvc3, MSVC 19.50.35728, Windows SDK 10.0.26100 |

- `Summit`, `GameEditor`, `GameBuilder`의 Debug와 Release 빌드가 모두 통과했다. 컴파일 경고는 0건이었다.
- 콘텐츠가 없는 새 캐시에서 `SUMMIT_BUILD_TESTS`의 기본값은 OFF였으며, SummitTests 타깃은 생성되지 않았다. ServerCore 경로를 별도로 지정하지 않아도 형제 디렉터리의 코덱 헤더를 찾았다.
- `SUMMIT_BUILD_TESTS=ON`을 명시한 별도 구성은 예상대로 실패했다. 누락된 프로젝트·씬·글꼴 경로와 콘텐츠 복원 또는 OFF 구성 안내를 확인했다.
- 생성된 빌드 타깃에서 `Source/Bootstrap/SummitBootstrap.cpp`는 Summit에만 한 번 포함됐다. 공유 컴포넌트와 Editor에는 포함되지 않았다.
- 빌드 후 원본 입력 파일의 해시가 유지됐다. 자동 생성된 컴포넌트 스키마는 공개 파일에 포함하지 않는다.

코드 전용 빌드 성공은 게임 플레이나 콘텐츠 회귀 테스트의 성공을 의미하지 않는다. 실행에는 호환 씬·글꼴·자산을 별도로 준비해야 한다.

## 로컬 콘텐츠 회귀

같은 Summit 코드와 비공개 로컬 콘텐츠를 사용하는 작업 환경에서 Summit, GameEditor, SummitTests를 Debug·Release로 다시 빌드한 뒤 두 CTest 항목을 실행했다. 모든 빌드가 끝난 뒤 순서대로 테스트했으며 컴파일 경고는 0건이었다.

| 구성 | Summit.CharacterProfiles | Summit.ChatVisuals | CTest 총 시간 |
| --- | --- | --- | --- |
| Debug | 통과 | 통과 | 23.71초 |
| Release | 통과 | 통과 | 17.71초 |

총 4건이 통과했다. 네트워크 회귀는 테스트가 만든 로컬 피어를 사용하고, 그래픽 회귀는 D3D11·D3D12 및 UI 배율 1·2를 검사한다. 위 시간은 회귀 검사 실행 시간이며 렌더러 성능 비교나 외부 서버 처리량 측정값이 아니다. 사용한 콘텐츠는 이 저장소에서 배포하지 않는다.

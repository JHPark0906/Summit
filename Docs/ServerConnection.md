# 서버 연결과 프로토콜

접속과 메시지 처리는 [MultiplayerController](../Source/Network/MultiplayerController.cpp)가 담당한다. 클라이언트는 Windows IPv4 비차단 소켓을 사용하며, ServerCore에서 공유하는 TCP 프레임·UDP 데이터그램 코덱만 포함한다. 서버 라이브러리를 실행하거나 링크하는 코드는 아니다.

## 접속 대상 설정

씬에 `MultiplayerController`를 배치하고 Inspector 또는 컴포넌트 API로 설정한다. 공개본에는 접속 대상이 저장된 씬이나 운영 서버 주소가 포함되지 않는다.

| 속성 | C++ 초기값 | 설정 방법 |
| --- | --- | --- |
| `serverAddress` | `127.0.0.1` | 접속할 서버의 IPv4 주소 |
| `serverPort` | `17150` | 호환 서버가 실제로 수신하는 TCP 포트 |

[헤더의 기본값](../Source/Network/MultiplayerController.h)보다 씬에 직렬화한 값이 우선한다. 예를 들어 서버를 17890으로 구동했다면 클라이언트 포트도 17890으로 지정한다. 씬을 변경한 뒤에는 콘텐츠 스테이징을 다시 수행한다. IPv6·DNS 이름 접속은 현재 구현 범위에 포함되지 않는다.

최초 프로필을 확정한 뒤 접속을 시도한다. 연결에 실패하거나 끊어지면 로컬 플레이와 로컬 채팅을 유지한다. 온라인 프로필 변경은 서버 승인 뒤 반영하며 닉네임 거절은 프로필 UI에 표시한다.

## TCP 제어 연결

TCP는 **4바이트 little-endian JSON 본문 길이 + UTF-8 JSON 봉투**다. 클라이언트 봉투는 `type`과 `body`로 구성되며, 응답의 거절 이유는 `error`에서 읽는다. 코덱은 [ServerCore](https://github.com/JHPark0906/ServerCore)의 `include/ServerCore/Protocol/FrameCodec.h`를 사용한다.

현재 가입 요청은 schema 6과 `movementTransport: "udp"`를 보낸다. `JoinAccepted`의 schema도 6이어야 한다. 주요 흐름은 다음과 같다.

| 메시지 | 클라이언트 동작 |
| --- | --- |
| `Join` / `JoinAccepted` / `JoinRejected` | 닉네임·캐릭터 가입 요청과 승인/거절 |
| `SetProfile` / `ProfileChanged` / `ProfileRejected` | 온라인 프로필 변경 |
| `DirectoryPage` / `DirectoryAck` / `DirectoryReady` | 최대 32개 단위의 전역 명단과 페이지 확인 |
| `VisibilityEnter` / `VisibilityExit` | 가시 범위의 원격 객체 생성·제거 |
| `Chat` / `ChatMessage` / `ChatRejected` | 채팅 전송, 서버가 보낸 메시지 표시, 거절 안내 |
| `ServerNotice`, `PlayerJoined`, `PlayerLeft` | 공지·입퇴장 표시와 명단 갱신 |

전체 프로필 명단과 현재 표시하는 원격 객체는 구분한다. 가시 범위를 나간 객체는 제거해도 전역 프로필을 유지하며, 실제 퇴장은 두 상태를 정리한다. 초기 명단 동기화는 입장 알림으로 중복 표시하지 않는다.

채팅 입력은 줄바꿈·제어 문자를 제한하고 UTF-8 512바이트 이하를 받는다. 클라이언트의 온라인 전송 간격은 최소 0.5초이며 서버도 독립적으로 요청을 검증한다. 바이트 제한은 모든 문자에 같은 글자 수를 보장하지 않는다.

## UDP 이동 협상

`JoinAccepted.udp`가 있으면 응답의 포트와 128비트 토큰을 읽고, 연결된 TCP 서버의 IPv4를 UDP 목적지로 사용한다. UDP 항목이 없는 schema 6 서버와는 TCP 이동 경로를 사용한다. **UDP를 협상한 뒤에는 차단·유실을 이유로 이동을 TCP로 자동 우회하지 않는다.**

데이터그램은 `SMU1`, 토큰, big-endian uint64 패킷 순번으로 된 28바이트 머리와 JSON 봉투를 합해 최대 1,200바이트다. 형식은 ServerCore의 `DatagramCodec.h`를 공유한다.

- `UdpHello`는 500ms 간격으로 재시도한다. 응답이 3초 이상 없으면 이동 연결 안내를 표시하며 TCP 채팅은 유지한다.
- UDP 이동 중에도 TCP `Heartbeat`를 5초마다 보낸다.
- 로컬 `PlayerState`는 50ms 간격을 기준으로 보낸다. 긴 프레임 뒤에는 누적 상태를 몰아서 보내지 않고 최신 상태 하나를 사용한다.
- 서버의 `StateBatch`를 검증하고 플레이어별 상태 revision `r`로 오래된 상태를 거른다. `r`은 정확한 uint64를 담는 10진 문자열이며 TCP `VisibilityEnter`가 생성·재진입 기준을 제공한다.
- 패킷 도착 순서만으로 서로 다른 플레이어의 새 상태를 함께 버리지 않는다. 수신 `q`는 진단값이며 표시·상태 순서 비교에는 사용하지 않는다.

네트워크 경로에서 TCP 접속 포트와 협상된 UDP 포트가 모두 도달 가능해야 한다. 서버의 실제 주소·포트·방화벽 설정은 운영 환경에서 정한다. 토큰은 세션 식별 수단이며 TLS/DTLS나 메시지 인증을 제공하지 않는다.

## 검증 범위

[ChatNetworkTests.cpp](../Tests/ChatNetworkTests.cpp)는 로컬 피어로 가입·명단·가시성·채팅, UDP 재시도·잘못된 입력·순서 변경·복구·종료를 검사한다. 씬과 글꼴 등을 사용하는 콘텐츠 회귀이므로 공개본만으로 실행하지 않는다. 준비 방법은 [빌드와 콘텐츠 준비](Distribution.md)에 있다.

테스트와 클라이언트의 송신 주기는 실제 서버의 처리량, 외부 인터넷 지연 또는 특정 동시 접속 수를 보장하지 않는다. 이동·충돌은 각 클라이언트가 계산하며 서버 권위 물리와 부정행위 방지는 이 코드의 기능이 아니다.

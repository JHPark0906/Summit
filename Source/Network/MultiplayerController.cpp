#include "MultiplayerController.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ServerCore/Protocol/FrameCodec.h"
#include "ServerCore/Protocol/DatagramCodec.h"

#include "Core/Json.h"
#include "Math/Vector.h"
#include "Platform/IInput.h"
#include "Runtime/Button.h"
#include "Runtime/AudioSource.h"
#include "Runtime/ComponentType.h"
#include "Runtime/GameObject.h"
#include "Runtime/InputField.h"
#include "Runtime/Input.h"
#include "Runtime/RuntimeContext.h"
#include "Runtime/PropertyDescriptor.h"
#include "Runtime/Rigidbody2D.h"
#include "Runtime/Scene.h"
#include "Runtime/SpriteAnimator.h"
#include "Runtime/SpriteRenderer.h"
#include "Runtime/TextRenderer.h"
#include "Runtime/Transform.h"

#include "../Gameplay/PlayerController.h"
#include "../Gameplay/CharacterProfile.h"
#include "../UI/ChatController.h"
#include "../UI/SpeechBubble.h"
#include "Diagnostics/Debug.h"

namespace Summit
{

namespace Runtime = GameEngine::Runtime;
namespace Core = GameEngine::Core;
namespace Math = GameEngine::Math;
namespace FrameCodec = ServerCore::Protocol::FrameCodec;
namespace DatagramCodec = ServerCore::Protocol::DatagramCodec;

namespace
{
    constexpr float StateSendInterval = 0.05f;
    constexpr auto UdpHelloInterval = std::chrono::milliseconds(500);
    constexpr auto UdpResponseTimeout = std::chrono::seconds(3);
    constexpr auto TcpHeartbeatInterval = std::chrono::seconds(5);
    constexpr float ConnectTimeoutSeconds = 3.0f;
    constexpr float MinimumBlendSeconds = 0.025f;
    constexpr float MaximumBlendSeconds = 0.15f;
    constexpr float MaximumExtrapolationSeconds = 0.10f;
    constexpr std::size_t MaximumNicknameBytes = 48;
    constexpr std::size_t MaximumChatBytes = 512;
    constexpr std::array<std::string_view, SelectableCharacterCount> CharacterButtons{
        "CharacterAButton", "CharacterBButton", "CharacterCButton", "CharacterDButton", "CharacterEButton" };
    constexpr std::array<std::string_view, SelectableCharacterCount> CharacterNames{
        "캐릭터 A", "캐릭터 B", "캐릭터 C", "캐릭터 D", "캐릭터 E" };
    // JSON 숫자는 double이므로 이 범위를 넘는 서버 ID는 10진 문자열로 받아야 정밀도를 잃지 않는다.
    constexpr double MaximumExactJsonInteger = 9007199254740991.0;

    constexpr std::string_view NetworkStatusObject = "NetworkStatusText";
    constexpr std::string_view ReconnectButtonObject = "ReconnectButton";
    constexpr std::string_view NicknameInputObject = "NicknameInput";
    constexpr std::string_view NicknameSubmitButtonObject = "NicknameSubmitButton";

    std::span<const Runtime::PropertyDescriptor> MultiplayerControllerProperties()
    {
        static const Runtime::PropertyDescriptor properties[] = {
            Runtime::MakeProperty<MultiplayerController>(
                "serverAddress", "Server IPv4 Address",
                &MultiplayerController::GetServerAddress,
                &MultiplayerController::SetServerAddress),
            Runtime::MakeProperty<MultiplayerController>(
                "serverPort", "Server Port",
                &MultiplayerController::GetServerPort,
                &MultiplayerController::SetServerPort),
            Runtime::MakeProperty<MultiplayerController>(
                "nickname", "Nickname",
                &MultiplayerController::GetNickname,
                &MultiplayerController::SetNickname),
        };
        return properties;
    }

    [[nodiscard]] bool IsNicknameUsable(const std::string_view nickname)
    {
        return !nickname.empty() && nickname.size() <= MaximumNicknameBytes &&
            nickname.find_first_not_of(' ') != std::string_view::npos &&
            std::none_of(nickname.begin(), nickname.end(),
                [](unsigned char ch) { return ch < 32 || ch == 127; });
    }

    [[nodiscard]] bool IsWouldBlock(const int error)
    {
        return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
    }

    [[nodiscard]] Runtime::GameObject* FindGameObject(
        Runtime::Scene* const scene, const std::string_view name)
    {
        return scene ? scene->FindGameObject(std::string(name)) : nullptr;
    }

    [[nodiscard]] ChatController* FindChatController(Runtime::Scene* scene)
    {
        auto* manager = FindGameObject(scene, "NetworkManager");
        return manager ? manager->GetComponent<ChatController>() : nullptr;
    }

    [[nodiscard]] bool IsChatTextUsable(const std::string_view text)
    {
        return !text.empty() && text.size() <= MaximumChatBytes &&
            text.find_first_not_of(' ') != std::string_view::npos &&
            std::none_of(text.begin(), text.end(), [](const unsigned char ch) { return ch < 32 || ch == 127; });
    }

    [[nodiscard]] std::optional<float> ReadFiniteFloat(
        const Core::Json& object, const std::string& name)
    {
        const Core::Json* const value = object.Find(name);
        if (!value || !value->IsNumber())
        {
            return std::nullopt;
        }
        const double number = value->AsNumber();
        if (!std::isfinite(number) || number < -(std::numeric_limits<float>::max)() ||
            number > (std::numeric_limits<float>::max)())
        {
            return std::nullopt;
        }
        return static_cast<float>(number);
    }

    [[nodiscard]] std::optional<std::uint64_t> ReadUnsignedInteger(const Core::Json* const value)
    {
        if (!value)
        {
            return std::nullopt;
        }
        if (value->IsString())
        {
            const std::string& text = value->AsString();
            std::uint64_t result = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
            return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
                ? std::optional<std::uint64_t>{ result }
                : std::nullopt;
        }
        if (!value->IsNumber())
        {
            return std::nullopt;
        }
        const double number = value->AsNumber();
        if (!std::isfinite(number) || number < 0.0 || std::floor(number) != number ||
            number > MaximumExactJsonInteger)
        {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(number);
    }

    [[nodiscard]] PlayerController::AnimationState ParseAnimationState(
        const Core::Json& body)
    {
        const Core::Json* const value = body.Find("s");
        if (!value || !value->IsString())
        {
            return PlayerController::AnimationState::Idle;
        }
        const std::string& state = value->AsString();
        if (state == "run" || state == "walk")
        {
            return PlayerController::AnimationState::Walk;
        }
        if (state == "jump" || state == "fall")
        {
            return PlayerController::AnimationState::Jump;
        }
        return PlayerController::AnimationState::Idle;
    }

    [[nodiscard]] PlayerController::Character ParseCharacter(const Core::Json& body)
    {
        const std::optional<std::uint64_t> value = ReadUnsignedInteger(body.Find("c"));
        if (!value || *value >= CharacterCount)
        {
            return PlayerController::Character::CharacterA;
        }
        return static_cast<PlayerController::Character>(*value);
    }

    [[nodiscard]] const char* AnimationStateName(const PlayerController::AnimationState state)
    {
        switch (state)
        {
        case PlayerController::AnimationState::Walk:
            return "run";
        case PlayerController::AnimationState::Jump:
            return "jump";
        case PlayerController::AnimationState::Idle:
        default:
            return "idle";
        }
    }
}

// 소켓 폴링과 프레임 콜백은 모두 이 컴포넌트의 게임 스레드 Update 안에서 실행된다.
class MultiplayerStateData
{
public:
    struct PlayerProfile
    {
        std::string name;
        PlayerController::Character character = PlayerController::Character::CharacterA;
    };

    struct RemotePlayer
    {
        // Scene 소유의 비소유 포인터다. VisibilityEnter에서만 만들고 가시 범위 이탈/퇴장 시 제거한다.
        Runtime::GameObject* object = nullptr;
        Math::Vector2 blendStart;
        Math::Vector2 targetPosition;
        Math::Vector2 velocity;
        float blendElapsed = 0.0f;
        float blendDuration = StateSendInterval;
        float secondsSincePacket = StateSendInterval;
        std::uint64_t lastServerTimestamp = 0;
        std::uint64_t lastRevision = 0;
        PlayerController::AnimationState animationState =
            PlayerController::AnimationState::Idle;
        bool facingLeft = false;
        bool hasState = false;
    };

    WSADATA winsockData{};
    bool winsockStarted = false;
    SOCKET socket = INVALID_SOCKET;
    SOCKET udpSocket = INVALID_SOCKET;
    sockaddr_in udpEndpoint{};
    DatagramCodec::Token udpToken{};
    bool udpNegotiated = false;
    bool udpReady = false;
    std::uint64_t udpSendSequence = 0;
    std::uint64_t stateSequence = 0;
    std::chrono::steady_clock::time_point udpStarted;
    std::chrono::steady_clock::time_point udpLastHello;
    std::chrono::steady_clock::time_point udpLastReceive;
    std::chrono::steady_clock::time_point tcpLastHeartbeat;
    MultiplayerController::ConnectionState connectionState =
        MultiplayerController::ConnectionState::OfflineSinglePlayer;
    // 연결·승인 제한 시간은 게임 deltaTime과 독립적인 실제 경과 시간으로 판단한다.
    std::chrono::steady_clock::time_point connectStarted;
    std::chrono::steady_clock::time_point joinStarted;
    std::vector<std::byte> frameStorage = std::vector<std::byte>(
        FrameCodec::HeaderSize + FrameCodec::DefaultMaxBodySize);
    // Reader는 바로 위 고정 크기 저장소를 빌린다. 연결 중 저장소의 크기나 주소를 바꾸지 않는다.
    std::unique_ptr<FrameCodec::Reader> frameReader;
    std::deque<std::vector<std::byte>> outgoingFrames;
    // 논블로킹 send가 일부만 썼을 때 큐 맨 앞 프레임에서 재개할 바이트 위치다.
    std::size_t outgoingOffset = 0;
    std::vector<std::string> incomingFrames;
    bool receiveCallbackFailed = false;
    float stateSendAccumulator = 0.0f;
    std::uint64_t selfId = 0;
    // 키는 서버 세션 ID다. 바뀔 수 있는 닉네임이나 로컬 GameObject ID로 참가자를 식별하지 않는다.
    // 전역 명단은 거리에 관계없이 유지하고, 가시 객체와 보간 상태는 별도로 보관한다.
    std::unordered_map<std::uint64_t, PlayerProfile> profiles;
    std::unordered_map<std::uint64_t, RemotePlayer> visibleRemotePlayers;
    bool directoryReady = false;
    std::string statusDetail;
    // 실제 캐릭터가 tdw여도 일반 선택은 별도로 보존한다.
    unsigned int preferredCharacter = 0;
    // 서버에 실제 보낸 일반 선택이다. 승인 시 이 값을 확정하여 뒤늦은 UI 초안과 섞이지 않게 한다.
    unsigned int requestedCharacter = 0;
    bool profileConfigured = false;
    bool profileOpen = true;
    bool profilePending = false;
    // 프로필 창 안에서만 편집한다. 취소 후 다시 열면 preferredCharacter에서 복원한다.
    unsigned int draftCharacter = 0;
    std::string profileError;
    std::chrono::steady_clock::time_point profileStarted;
    std::chrono::steady_clock::time_point lastChatSent;
    bool hasSentChat = false;
    bool localChatNoticeShown = false;
    float bgmUnmutedVolume = 0.25f;
};

namespace
{
    void CloseSocket(MultiplayerStateData& state)
    {
        if (state.udpSocket != INVALID_SOCKET) ::closesocket(state.udpSocket);
        state.udpSocket = INVALID_SOCKET;
        state.udpNegotiated = false;
        state.udpReady = false;
        state.udpToken = {};
        state.udpSendSequence = 0;
        state.stateSequence = 0;
        if (state.socket != INVALID_SOCKET)
        {
            ::shutdown(state.socket, SD_BOTH);
            ::closesocket(state.socket);
            state.socket = INVALID_SOCKET;
        }
        state.frameReader.reset();
        state.outgoingFrames.clear();
        state.outgoingOffset = 0;
        state.incomingFrames.clear();
        state.receiveCallbackFailed = false;
        state.stateSendAccumulator = 0.0f;
        state.selfId = 0;
        state.directoryReady = false;
        state.hasSentChat = false;
    }

    [[nodiscard]] bool QueueMessage(
        MultiplayerStateData& state, const std::string_view type, Core::Json body)
    {
        try
        {
            Core::Json::Object envelope;
            envelope.emplace("type", Core::Json(std::string(type)));
            envelope.emplace("body", std::move(body));
            const std::string json = Core::Json(std::move(envelope)).Dump();
            const auto* const bytes = reinterpret_cast<const std::byte*>(json.data());
            const std::span<const std::byte> bodyBytes(bytes, json.size());
            std::vector<std::byte> framed(FrameCodec::HeaderSize + bodyBytes.size());
            const FrameCodec::EncodeResult encoded = FrameCodec::EncodeTo(bodyBytes, framed);
            if (!encoded.IsOk())
            {
                return false;
            }
            state.outgoingFrames.push_back(std::move(framed));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    [[nodiscard]] bool OpenUdpSocket(MultiplayerStateData& state)
    {
        state.udpSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (state.udpSocket == INVALID_SOCKET) return false;
        u_long nonBlocking = 1;
        if (::ioctlsocket(state.udpSocket, FIONBIO, &nonBlocking) == 0 &&
            ::connect(state.udpSocket, reinterpret_cast<const sockaddr*>(&state.udpEndpoint),
                sizeof(state.udpEndpoint)) == 0) return true;
        ::closesocket(state.udpSocket);
        state.udpSocket = INVALID_SOCKET;
        return false;
    }

    [[nodiscard]] bool SendUdpMessage(
        MultiplayerStateData& state, const std::string_view type, Core::Json body)
    {
        if (state.udpSocket == INVALID_SOCKET ||
            state.udpSendSequence == (std::numeric_limits<std::uint64_t>::max)()) return false;
        try
        {
            Core::Json::Object envelope;
            envelope.emplace("type", Core::Json(std::string(type)));
            envelope.emplace("body", std::move(body));
            const std::string json = Core::Json(std::move(envelope)).Dump();
            std::array<std::byte, DatagramCodec::MaximumDatagramBytes> bytes{};
            const auto size = DatagramCodec::Encode(bytes, state.udpToken, ++state.udpSendSequence,
                std::as_bytes(std::span(json.data(), json.size())));
            return size != 0 && ::send(state.udpSocket, reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(size), 0) == static_cast<int>(size);
        }
        catch (...)
        {
            return false;
        }
    }

    [[nodiscard]] bool IsUdpStateValid(const Core::Json& state)
    {
        if (!state.IsObject()) return false;
        const auto id = ReadUnsignedInteger(state.Find("id"));
        const auto x = ReadFiniteFloat(state, "x");
        const auto y = ReadFiniteFloat(state, "y");
        const auto facing = ReadFiniteFloat(state, "f");
        const auto character = ReadUnsignedInteger(state.Find("c"));
        const auto* revision = state.Find("r");
        const auto* animation = state.Find("s");
        if (!id || *id == 0 || !x || !y || std::abs(*x) > 1.0e6f || std::abs(*y) > 1.0e6f ||
            !ReadFiniteFloat(state, "vx") || !ReadFiniteFloat(state, "vy") ||
            !ReadUnsignedInteger(state.Find("t")) || !facing ||
            !character || *character >= CharacterCount || !revision || !revision->IsString() ||
            !ReadUnsignedInteger(revision) || !animation || !animation->IsString()) return false;
        // 서버는 유한한 방향값과 32바이트 이하의 임의 애니메이션 이름을 중계한다.
        // 표시에서는 방향의 부호를 쓰고 모르는 이름은 기존 idle 대체 규칙을 따른다.
        if (animation->AsString().size() > 32) return false;
        const auto* sequence = state.Find("q");
        if (!sequence) return true;
        if (sequence->IsString()) return ReadUnsignedInteger(sequence).has_value();
        if (!sequence->IsNumber()) return false;
        // q는 표시/순서 비교에 쓰지 않는 진단값이다. 서버 numeric uint64를 double로 읽은 뒤
        // uint64로 되돌리면 UINT64_MAX도 거절된다. 정확한 순서 비교는 문자열 r만 담당한다.
        const double diagnostic = sequence->AsNumber();
        return std::isfinite(diagnostic) && diagnostic >= 0.0 && std::floor(diagnostic) == diagnostic;
    }

    [[nodiscard]] bool QueueJoin(
        MultiplayerStateData& state, const std::string_view nickname, const unsigned int character)
    {
        Core::Json::Object body;
        body.emplace("schemaVersion", Core::Json(6.0));
        body.emplace("movementTransport", Core::Json(std::string("udp")));
        body.emplace("name", Core::Json(std::string(nickname)));
        body.emplace("c", Core::Json(static_cast<double>(character)));
        const bool queued = QueueMessage(state, "Join", Core::Json(std::move(body)));
        if (queued) state.requestedCharacter = character;
        return queued;
    }

    void StoreFrame(
        void* const context, const std::span<const std::byte> body) noexcept
    {
        // 프레이머가 다음 수신에 버퍼를 재사용하기 전에 복사한다. JSON 처리와 장면 변경은 이후 단계다.
        auto& state = *static_cast<MultiplayerStateData*>(context);
        try
        {
            state.incomingFrames.emplace_back(
                reinterpret_cast<const char*>(body.data()), body.size());
        }
        catch (...)
        {
            state.receiveCallbackFailed = true;
        }
    }

    [[nodiscard]] bool FlushOutgoing(MultiplayerStateData& state)
    {
        while (!state.outgoingFrames.empty())
        {
            const std::vector<std::byte>& frame = state.outgoingFrames.front();
            const std::size_t remaining = frame.size() - state.outgoingOffset;
            const int sendSize = static_cast<int>((std::min)(
                remaining, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
            const int sent = ::send(
                state.socket,
                reinterpret_cast<const char*>(frame.data() + state.outgoingOffset),
                sendSize,
                0);
            if (sent == SOCKET_ERROR)
            {
                return IsWouldBlock(::WSAGetLastError());
            }
            if (sent <= 0)
            {
                return false;
            }
            state.outgoingOffset += static_cast<std::size_t>(sent);
            if (state.outgoingOffset == frame.size())
            {
                state.outgoingFrames.pop_front();
                state.outgoingOffset = 0;
            }
        }
        return true;
    }

    [[nodiscard]] PlayerController* FindLocalPlayerController(Runtime::Scene* const scene)
    {
        Runtime::GameObject* const player = FindGameObject(scene, "Player");
        return player ? player->GetComponent<PlayerController>() : nullptr;
    }

    void ApplyRemoteAppearance(
        MultiplayerStateData::RemotePlayer& remote, const MultiplayerStateData::PlayerProfile& profile,
        PlayerController& localController)
    {
        if (!remote.object)
        {
            return;
        }
        Runtime::GameObject& visual = PlayerController::FindVisualObject(*remote.object);
        Runtime::SpriteRenderer* const renderer = visual.GetComponent<Runtime::SpriteRenderer>();
        Runtime::SpriteAnimator* const animator = visual.GetComponent<Runtime::SpriteAnimator>();
        if (!renderer || !animator)
        {
            return;
        }

        PlayerController::AnimationState effectiveState = remote.animationState;
        const GameEngine::Assets::AssetReference* atlas =
            &localController.GetAtlas(profile.character, effectiveState);
        if (!atlas->IsValid() && effectiveState != PlayerController::AnimationState::Idle)
        {
            effectiveState = PlayerController::AnimationState::Idle;
            atlas = &localController.GetAtlas(profile.character, effectiveState);
        }

        float frameRate = localController.GetIdleFrameRate();
        if (effectiveState == PlayerController::AnimationState::Walk)
        {
            frameRate = localController.GetWalkFrameRate();
        }
        else if (effectiveState == PlayerController::AnimationState::Jump)
        {
            frameRate = localController.GetJumpFrameRate();
        }

        renderer->SetSprite(*atlas);
        renderer->SetFrame(0);
        renderer->SetFlipX(PlayerController::IsSpriteFlipped(profile.character, remote.facingLeft));
        const float scale = PlayerController::GetVisualScale(profile.character);
        visual.GetTransform().SetScale({ scale, scale, 1.0f });
        visual.GetTransform().SetPosition(
            { 0, localController.GetVisualOffsetY(profile.character, effectiveState), 0 });
        animator->SetFirstFrame(0);
        animator->SetFrameCount(0);
        animator->SetFrameRate(frameRate);
        animator->SetLooping(true);
        animator->SetPingPong(profile.character == PlayerController::Character::Tdw &&
            effectiveState == PlayerController::AnimationState::Idle);
        animator->SetPlaying(true);
        animator->Restart();
    }

    [[nodiscard]] Runtime::GameObject* CreateRemoteObject(
        Runtime::Scene& scene,
        const std::uint64_t id,
        MultiplayerStateData::RemotePlayer& remote, const MultiplayerStateData::PlayerProfile& profile,
        PlayerController& localController)
    {
        // 원격은 수신 상태를 그리는 객체다. 로컬 입력·물리·점프 AudioSource를 붙이지 않는다.
        auto object = std::make_unique<Runtime::GameObject>(
            scene.GetRuntimeContext(), "RemotePlayer-" + std::to_string(id));
        Runtime::GameObject* const raw = scene.AddGameObject(std::move(object));
        if (!raw)
        {
            return nullptr;
        }

        Runtime::GameObject* const visual = scene.CreateGameObject("Visual");
        if (!visual || !visual->GetTransform().SetParent(&raw->GetTransform()))
        {
            if (visual)
            {
                scene.RemoveGameObject(visual->GetInstanceId());
            }
            scene.RemoveGameObject(raw->GetInstanceId());
            return nullptr;
        }
        Runtime::SpriteRenderer* const renderer = visual->AddComponent<Runtime::SpriteRenderer>();
        Runtime::SpriteAnimator* const animator = visual->AddComponent<Runtime::SpriteAnimator>();
        if (!renderer || !animator)
        {
            scene.RemoveGameObject(raw->GetInstanceId());
            return nullptr;
        }
        renderer->SetSortingOrder(9);
        renderer->SetCastShadows(false);
        renderer->SetReceiveShadows(false);
        if (Runtime::GameObject* const local = FindGameObject(&scene, "Player"))
        {
            if (const Runtime::SpriteRenderer* const localRenderer =
                    PlayerController::FindVisualObject(*local).GetComponent<Runtime::SpriteRenderer>())
            {
                renderer->SetSize(localRenderer->GetSize());
            }
        }
        remote.object = raw;
        ApplyRemoteAppearance(remote, profile, localController);
        localController.UpdateNameplate(*raw, profile.name);
        return raw;
    }

    void RemoveVisiblePlayer(MultiplayerStateData& state, Runtime::Scene* scene, const std::uint64_t id)
    {
        const auto found = state.visibleRemotePlayers.find(id);
        if (found == state.visibleRemotePlayers.end()) return;
        if (scene && found->second.object)
            static_cast<void>(scene->RemoveGameObject(found->second.object->GetInstanceId()));
        state.visibleRemotePlayers.erase(found);
    }

    void SetInteractiveObjectActive(Runtime::GameObject* const object, const bool active)
    {
        if (!object)
        {
            return;
        }
        if (!active)
        {
            if (Runtime::Button* const button = object->GetComponent<Runtime::Button>())
            {
                button->SetInteractable(false);
            }
            if (Runtime::InputField* const input = object->GetComponent<Runtime::InputField>())
            {
                input->SetInteractable(false);
            }
            object->SetActive(false);
            return;
        }

        object->SetActive(true);
        if (Runtime::Button* const button = object->GetComponent<Runtime::Button>())
        {
            button->SetInteractable(true);
        }
        if (Runtime::InputField* const input = object->GetComponent<Runtime::InputField>())
        {
            input->SetInteractable(true);
        }
    }
}

MultiplayerController::MultiplayerController() = default;
MultiplayerController::~MultiplayerController() = default;

const Runtime::ComponentType& MultiplayerController::StaticType()
{
    static const Runtime::ComponentType type{
        "MultiplayerController", &Runtime::MonoBehaviour::StaticType(),
        &MultiplayerControllerProperties,
        &Runtime::MakeComponentInstance<MultiplayerController> };
    return type;
}

void MultiplayerController::SetServerAddress(std::string address)
{
    mServerAddress = address.empty() ? "127.0.0.1" : std::move(address);
}

void MultiplayerController::SetServerPort(const int port)
{
    mServerPort = port >= 1 && port <= 65535 ? port : 17150;
}

void MultiplayerController::SetNickname(std::string nickname)
{
    mNickname = std::move(nickname);
}

MultiplayerController::ConnectionState MultiplayerController::GetConnectionState() const
{
    return mState ? mState->connectionState : ConnectionState::OfflineSinglePlayer;
}

void MultiplayerController::Start()
{
    // 저장된 닉네임이 있어도 세션 진입 시 프로필 창에서 이름과 캐릭터를 함께 확정한다.
    mState = std::make_unique<MultiplayerStateData>();
    if (const auto* controller = FindLocalPlayerController(GetScene()))
    {
        const auto selected = static_cast<unsigned int>(controller->GetSelectedCharacter());
        mState->preferredCharacter = selected < SelectableCharacterCount ? selected : 0;
    }
    OpenProfile();
    RefreshUi();
}

void MultiplayerController::Update(const float deltaTime)
{
    if (!mState)
    {
        return;
    }

    if (auto* chat = FindChatController(GetScene()))
    {
        chat->SetAvailable(mState->profileConfigured && !mState->profileOpen && !mState->profilePending &&
            (mState->connectionState == ConnectionState::OfflineSinglePlayer ||
                mState->connectionState == ConnectionState::Multiplayer));
        chat->TickInput();
    }
    // 채팅이 소비한 Esc를 먼저 확정해야 같은 키가 프로필 창까지 열지 않는다.
    HandleUi();
    if (mState->connectionState == ConnectionState::Connecting)
    {
        PollConnection();
    }
    else if (mState->socket != INVALID_SOCKET)
    {
        PollSocket();
    }

    ProcessIncomingFrames();
    PollUdp();
    if (mState->profilePending && mState->connectionState == ConnectionState::Multiplayer &&
        std::chrono::duration<float>(std::chrono::steady_clock::now() -
            mState->profileStarted).count() >= ConnectTimeoutSeconds)
    {
        CloseConnection();
        mState->profileError = "서버 응답이 없어 연결을 종료했습니다. 변경 내용을 다시 저장해 주세요.";
    }
    SendLocalPlayerState(deltaTime);
    HandleChatSubmission();
    UpdateRemotePlayers(deltaTime);
    RefreshUi();
}

void MultiplayerController::OnDestroy()
{
    if (!mState)
    {
        return;
    }
    CloseSocket(*mState);
    if (mState->winsockStarted)
    {
        ::WSACleanup();
        mState->winsockStarted = false;
    }
    mState.reset();
}

void MultiplayerController::BeginConnect()
{
    if (!mState || !mState->profileConfigured)
    {
        return;
    }
    if (!mState->winsockStarted)
    {
        if (::WSAStartup(MAKEWORD(2, 2), &mState->winsockData) != 0)
        {
            mState->connectionState = ConnectionState::OfflineSinglePlayer;
            mState->statusDetail = "Winsock을 시작하지 못했습니다.";
            return;
        }
        mState->winsockStarted = true;
    }
    if (!IsNicknameUsable(mNickname))
    {
        mState->connectionState = ConnectionState::NicknameRejected;
        mState->statusDetail = "닉네임은 UTF-8 기준 1~48바이트여야 합니다.";
        return;
    }

    CloseConnection();
    mState->socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mState->socket == INVALID_SOCKET)
    {
        mState->statusDetail = "소켓을 만들지 못해 싱글플레이로 진행합니다.";
        return;
    }

    u_long nonBlocking = 1;
    if (::ioctlsocket(mState->socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        CloseSocket(*mState);
        mState->statusDetail = "논블로킹 소켓 설정에 실패했습니다.";
        return;
    }

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(static_cast<u_short>(mServerPort));
    if (::inet_pton(AF_INET, mServerAddress.c_str(), &endpoint.sin_addr) != 1)
    {
        CloseSocket(*mState);
        mState->statusDetail = "서버 주소는 IPv4 형식이어야 합니다.";
        return;
    }

    mState->connectionState = ConnectionState::Connecting;
    mState->statusDetail.clear();
    mState->connectStarted = std::chrono::steady_clock::now();
    const int result = ::connect(
        mState->socket, reinterpret_cast<const sockaddr*>(&endpoint),
        static_cast<int>(sizeof(endpoint)));
    if (result == 0)
    {
        mState->frameReader = std::make_unique<FrameCodec::Reader>(
            std::span<std::byte>(mState->frameStorage));
        mState->connectionState = ConnectionState::AwaitingJoin;
        mState->joinStarted = std::chrono::steady_clock::now();
        if (!QueueJoin(*mState, mNickname, mState->preferredCharacter))
        {
            CloseConnection();
            mState->statusDetail = "입장 메시지를 만들지 못했습니다.";
        }
        return;
    }

    if (!IsWouldBlock(::WSAGetLastError()))
    {
        CloseConnection();
        mState->statusDetail = "서버에 연결하지 못해 싱글플레이로 진행합니다.";
    }
}

void MultiplayerController::PollConnection()
{
    if (!mState || mState->socket == INVALID_SOCKET)
    {
        return;
    }

    const float elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - mState->connectStarted).count();
    if (elapsed >= ConnectTimeoutSeconds)
    {
        CloseConnection();
        mState->statusDetail = "서버 연결 시간이 초과되어 싱글플레이로 진행합니다.";
        return;
    }

    fd_set writable;
    fd_set exceptional;
    FD_ZERO(&writable);
    FD_ZERO(&exceptional);
    FD_SET(mState->socket, &writable);
    FD_SET(mState->socket, &exceptional);
    timeval timeout{};
    const int ready = ::select(0, nullptr, &writable, &exceptional, &timeout);
    if (ready == SOCKET_ERROR)
    {
        CloseConnection();
        mState->statusDetail = "서버 연결 확인에 실패했습니다.";
        return;
    }
    if (ready == 0)
    {
        return;
    }

    // 쓰기 가능 알림은 연결 성공뿐 아니라 실패도 뜻한다. SO_ERROR까지 확인한 뒤 Join을 보낸다.
    int socketError = 0;
    int errorLength = static_cast<int>(sizeof(socketError));
    if (::getsockopt(
            mState->socket, SOL_SOCKET, SO_ERROR,
            reinterpret_cast<char*>(&socketError), &errorLength) == SOCKET_ERROR ||
        socketError != 0 || FD_ISSET(mState->socket, &exceptional))
    {
        CloseConnection();
        mState->statusDetail = "서버에 연결하지 못해 싱글플레이로 진행합니다.";
        return;
    }

    mState->frameReader = std::make_unique<FrameCodec::Reader>(
        std::span<std::byte>(mState->frameStorage));
    mState->connectionState = ConnectionState::AwaitingJoin;
    mState->joinStarted = std::chrono::steady_clock::now();
    if (!QueueJoin(*mState, mNickname, mState->preferredCharacter))
    {
        CloseConnection();
        mState->statusDetail = "입장 메시지를 만들지 못했습니다.";
    }
}

void MultiplayerController::PollSocket()
{
    if (!mState || mState->socket == INVALID_SOCKET || !mState->frameReader)
    {
        return;
    }

    if (mState->connectionState == ConnectionState::AwaitingJoin)
    {
        const float elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - mState->joinStarted).count();
        if (elapsed >= ConnectTimeoutSeconds)
        {
            CloseConnection();
            mState->statusDetail = "서버 입장 응답이 없어 싱글플레이로 진행합니다.";
            return;
        }
    }

    if (!FlushOutgoing(*mState))
    {
        const bool preserve = mState->connectionState == ConnectionState::NicknameRejected;
        CloseConnection(preserve);
        mState->statusDetail = preserve
            ? "연결이 끊겼습니다. 이름을 바꾸고 다시 입장해 주세요."
            : "서버 연결이 끊겨 싱글플레이로 전환했습니다.";
        return;
    }

    std::array<std::byte, 4096> received{};
    for (;;)
    {
        const int count = ::recv(
            mState->socket, reinterpret_cast<char*>(received.data()),
            static_cast<int>(received.size()), 0);
        if (count > 0)
        {
            const FrameCodec::AppendResult appended = mState->frameReader->Append(
                std::span<const std::byte>(received.data(), static_cast<std::size_t>(count)),
                mState.get(), &StoreFrame);
            if (!appended.IsOk() || mState->receiveCallbackFailed)
            {
                CloseConnection();
                mState->statusDetail = "서버 메시지 형식이 잘못되어 연결을 닫았습니다.";
                return;
            }
            continue;
        }
        if (count == 0)
        {
            ProcessIncomingFrames();
            if (mState->socket == INVALID_SOCKET)
            {
                return;
            }
            const bool preserve = mState->connectionState == ConnectionState::NicknameRejected;
            CloseConnection(preserve);
            mState->statusDetail = preserve
                ? "연결이 끊겼습니다. 이름을 바꾸고 다시 입장해 주세요."
                : "서버 연결이 끊겨 싱글플레이로 전환했습니다.";
            return;
        }
        if (IsWouldBlock(::WSAGetLastError()))
        {
            return;
        }

        ProcessIncomingFrames();
        if (mState->socket == INVALID_SOCKET)
        {
            return;
        }
        const bool preserve = mState->connectionState == ConnectionState::NicknameRejected;
        CloseConnection(preserve);
        mState->statusDetail = preserve
            ? "연결이 끊겼습니다. 이름을 바꾸고 다시 입장해 주세요."
            : "서버 연결이 끊겨 싱글플레이로 전환했습니다.";
        return;
    }
}

void MultiplayerController::PollUdp()
{
    if (!mState || !mState->udpNegotiated || mState->connectionState != ConnectionState::Multiplayer) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - mState->tcpLastHeartbeat >= TcpHeartbeatInterval)
    {
        mState->tcpLastHeartbeat = now;
        if (!QueueMessage(*mState, "Heartbeat", Core::Json(Core::Json::Object{})))
        {
            CloseConnection();
            mState->statusDetail = "서버 연결 확인 메시지를 보내지 못했습니다.";
            return;
        }
    }
    if (now - mState->udpLastHello >= UdpHelloInterval)
    {
        mState->udpLastHello = now;
        if (mState->udpSocket != INVALID_SOCKET || OpenUdpSocket(*mState))
            static_cast<void>(SendUdpMessage(*mState, "UdpHello", Core::Json(Core::Json::Object{})));
    }

    // 연결한 TCP peer의 IPv4와 협상 포트만 받는 connected UDP 소켓이다.
    // 한 프레임의 수신량을 제한하고, 오래된 데이터그램을 신뢰성 큐에 재전송하지 않는다.
    std::array<std::byte, DatagramCodec::MaximumDatagramBytes + 1> bytes{};
    for (unsigned int packetIndex = 0; mState->udpSocket != INVALID_SOCKET && packetIndex < 128; ++packetIndex)
    {
        const int size = ::recv(mState->udpSocket, reinterpret_cast<char*>(bytes.data()),
            static_cast<int>(bytes.size()), 0);
        if (size == SOCKET_ERROR)
        {
            const int error = ::WSAGetLastError();
            if (IsWouldBlock(error)) break;
            if (error == WSAEMSGSIZE || error == WSAECONNRESET) continue;
            ::closesocket(mState->udpSocket);
            mState->udpSocket = INVALID_SOCKET;
            mState->udpReady = false;
            break;
        }
        const auto packet = DatagramCodec::Decode(std::span(bytes.data(), static_cast<std::size_t>(size)));
        if (!packet || packet->token != mState->udpToken) continue;
        try
        {
            const auto envelope = Core::Json::ParseBytes(packet->payload);
            const auto* type = envelope.Find("type");
            const auto* body = envelope.Find("body");
            if (!type || !type->IsString() || !body || !body->IsObject()) continue;
            if (type->AsString() == "UdpReady")
            {
                mState->udpReady = true;
                mState->udpLastReceive = now;
                continue;
            }
            if (type->AsString() != "StateBatch") continue;
            const auto* states = body->Find("states");
            if (!states || !states->IsArray() || states->Size() > 32) continue;
            // 전체 배열을 먼저 검증한다. 뒤 항목 하나가 잘못돼도 앞 캐릭터의 위치/r을 바꾸면 안 된다.
            if (!std::all_of(states->AsArray().begin(), states->AsArray().end(), &IsUdpStateValid)) continue;
            for (const auto& state : states->AsArray()) ApplyRemoteState(state, false);
            // 패킷 순번으로 전체 배열을 버리지 않는다. 서로 다른 ID가 든 역순 패킷도 각 r이 최신일 수 있다.
            mState->udpLastReceive = now;
        }
        catch (const Core::JsonError&)
        {
            // 손상되거나 오래된 UDP는 TCP 세션/프로필/채팅을 종료시키지 않는다.
        }
    }
    if (now - mState->udpLastReceive >= UdpResponseTimeout) mState->udpReady = false;
    mState->statusDetail = mState->udpReady ? ""
        : now - mState->udpStarted >= UdpResponseTimeout
            ? "이동 연결(UDP) 응답이 없습니다. 재연결 중이며 채팅은 계속 사용할 수 있습니다."
            : "이동 연결(UDP)을 확인하고 있습니다.";
}

void MultiplayerController::ApplyRemoteProfile(const Core::Json& profile)
{
    const auto id = ReadUnsignedInteger(profile.Find("id"));
    const auto character = ReadUnsignedInteger(profile.Find("c"));
    const auto* name = profile.Find("name");
    if (!id || *id == 0 || !character || *character >= CharacterCount || !name || !name->IsString() ||
        !IsNicknameUsable(name->AsString()) ||
        !IsProfileCharacterValid(name->AsString(), static_cast<unsigned int>(*character)))
        throw Core::JsonError("invalid player profile");
    if (*id == mState->selfId) return;
    auto& approved = mState->profiles[*id];
    approved.name = name->AsString();
    approved.character = ParseCharacter(profile);
    const auto visible = mState->visibleRemotePlayers.find(*id);
    if (auto* controller = FindLocalPlayerController(GetScene()); controller &&
        visible != mState->visibleRemotePlayers.end() && visible->second.object)
    {
        ApplyRemoteAppearance(visible->second, approved, *controller);
        controller->UpdateNameplate(*visible->second.object, approved.name);
    }
}

void MultiplayerController::ApplyRemoteState(const Core::Json& state, const bool entering)
{
    if (!state.IsObject()) throw Core::JsonError("invalid visible player state");
    const auto id = ReadUnsignedInteger(state.Find("id"));
    const auto x = ReadFiniteFloat(state, "x");
    const auto y = ReadFiniteFloat(state, "y");
    const auto vx = ReadFiniteFloat(state, "vx");
    const auto vy = ReadFiniteFloat(state, "vy");
    const auto timestamp = ReadUnsignedInteger(state.Find("t"));
    const auto character = ReadUnsignedInteger(state.Find("c"));
    const auto facing = ReadFiniteFloat(state, "f");
    const auto* animation = state.Find("s");
    const auto revision = ReadUnsignedInteger(state.Find("r"));
    if (!id || *id == 0 || !x || !y || !vx || !vy || !timestamp || !facing ||
        !character || *character >= CharacterCount || !animation || !animation->IsString() ||
        (mState->udpNegotiated && !revision))
        throw Core::JsonError("invalid visible player state");
    // q는 진단용 클라이언트 순번, r은 순서가 바뀐 UDP 상태를 거르는 서버 revision이다.
    // r은 10진 문자열로 수신하여 uint64 전체 범위를 정확히 비교한다.
    if (*id == mState->selfId) return;
    if (entering)
    {
        // 재진입 스냅샷은 이전 위치/보간/말풍선을 이어받지 않는다.
        RemoveVisiblePlayer(*mState, GetScene(), *id);
        ApplyRemoteProfile(state);
        mState->visibleRemotePlayers.try_emplace(*id);
    }
    const auto profile = mState->profiles.find(*id);
    const auto visible = mState->visibleRemotePlayers.find(*id);
    if (profile == mState->profiles.end() || visible == mState->visibleRemotePlayers.end()) return;
    auto& remote = visible->second;
    if (!entering && revision && *revision <= remote.lastRevision) return;
    auto* localController = FindLocalPlayerController(GetScene());
    if (!localController || !GetScene()) return;
    // 일반 상태는 VisibilityExit 뒤 늦게 와도 객체를 다시 만들 수 없다.
    if (!remote.object && (!entering || !CreateRemoteObject(
        *GetScene(), *id, remote, profile->second, *localController))) return;
    const bool firstState = !remote.hasState;
    const Math::Vector3 current = remote.object->GetTransform().GetPosition();
    remote.targetPosition = { *x, *y };
    remote.blendStart = firstState
        ? remote.targetPosition
        : Math::Vector2{ current.GetX(), current.GetY() };
    remote.velocity = { *vx, *vy };
    remote.blendElapsed = 0.0f;

    if (timestamp && remote.lastServerTimestamp != 0 && *timestamp > remote.lastServerTimestamp)
    {
        // 서버 시각은 밀리초다. 두 패킷의 차이만 쓰므로 클라이언트 시계와 동기화할 필요가 없다.
        remote.blendDuration = (std::clamp)(
            static_cast<float>(*timestamp - remote.lastServerTimestamp) / 1000.0f,
            MinimumBlendSeconds, MaximumBlendSeconds);
    }
    else
    {
        remote.blendDuration = (std::clamp)(
            remote.secondsSincePacket, MinimumBlendSeconds, MaximumBlendSeconds);
    }
    if (firstState)
    {
        remote.blendDuration = MinimumBlendSeconds;
        remote.object->GetTransform().SetPosition({ *x, *y, current.GetZ() });
    }
    if (timestamp)
    {
        remote.lastServerTimestamp = *timestamp;
    }
    remote.secondsSincePacket = 0.0f;
    if (revision) remote.lastRevision = *revision;


    const PlayerController::AnimationState nextAnimation = ParseAnimationState(state);
    const bool nextFacingLeft = facing && *facing < 0.0f;
    const bool appearanceChanged = !remote.hasState ||
        remote.animationState != nextAnimation;

    remote.animationState = nextAnimation;
    remote.facingLeft = nextFacingLeft;
    if (appearanceChanged)
    {
        ApplyRemoteAppearance(remote, profile->second, *localController);
    }
    else if (Runtime::SpriteRenderer* const renderer =
                 PlayerController::FindVisualObject(*remote.object).GetComponent<Runtime::SpriteRenderer>())
    {
        renderer->SetFlipX(PlayerController::IsSpriteFlipped(profile->second.character, remote.facingLeft));
    }
    remote.hasState = true;
}

void MultiplayerController::ProcessIncomingFrames()
{
    if (!mState || mState->incomingFrames.empty())
    {
        return;
    }

    // 처리 중 CloseConnection이 수신 큐를 비워도 현재 순회 중인 프레임은 무효화되지 않는다.
    std::vector<std::string> frames;
    frames.swap(mState->incomingFrames);
    for (const std::string& frame : frames)
    {
        try
        {
            const Core::Json envelope = Core::Json::Parse(frame);
            const Core::Json* const typeValue = envelope.Find("type");
            if (!typeValue || !typeValue->IsString())
            {
                throw Core::JsonError("network envelope needs a type");
            }
            const std::string& type = typeValue->AsString();
            const Core::Json* const body = envelope.Find("body");

            if (type == "JoinAccepted")
            {
                if (!body || !body->IsObject())
                    throw Core::JsonError("JoinAccepted needs a body");
                const auto id = ReadUnsignedInteger(body->Find("id"));
                const auto version = ReadUnsignedInteger(body->Find("schemaVersion"));
                const auto character = ReadUnsignedInteger(body->Find("c"));
                const auto* name = body->Find("name");
                const auto* players = body->Find("players");
                const auto throughId = ReadUnsignedInteger(body->Find("directoryThroughId"));
                if (!id || *id == 0 || !version || *version != 6 || !character || *character >= CharacterCount ||
                    !name || !name->IsString() || !IsNicknameUsable(name->AsString()) ||
                    !IsProfileCharacterValid(name->AsString(), static_cast<unsigned int>(*character)) ||
                    !players || !players->IsArray() || !players->AsArray().empty() || !throughId)
                    throw Core::JsonError("invalid JoinAccepted profile");
                if (const auto* udp = body->Find("udp"))
                {
                    const auto port = ReadUnsignedInteger(udp->Find("port"));
                    const auto* token = udp->Find("token");
                    const auto parsed = token && token->IsString()
                        ? DatagramCodec::TokenFromHex(token->AsString()) : std::nullopt;
                    int peerSize = sizeof(mState->udpEndpoint);
                    if (!udp->IsObject() || !port || *port == 0 || *port > 65535 || !parsed ||
                        ::getpeername(mState->socket, reinterpret_cast<sockaddr*>(&mState->udpEndpoint),
                            &peerSize) != 0 || mState->udpEndpoint.sin_family != AF_INET)
                        throw Core::JsonError("invalid UDP negotiation");
                    mState->udpEndpoint.sin_port = ::htons(static_cast<u_short>(*port));
                    mState->udpToken = *parsed;
                    mState->udpNegotiated = true;
                    mState->udpStarted = std::chrono::steady_clock::now();
                    mState->udpLastReceive = mState->udpStarted;
                    mState->tcpLastHeartbeat = mState->udpStarted;
                    mState->udpLastHello = mState->udpStarted - UdpHelloInterval;
                }
                mState->selfId = *id;
                mState->directoryReady = false;
                ApplyLocalProfile(name->AsString(), static_cast<unsigned int>(*character),
                    mState->requestedCharacter);
                mState->profilePending = false;
                mState->profileOpen = false;
                mState->profileError.clear();
                mState->connectionState = ConnectionState::Multiplayer;
                mState->statusDetail.clear();
                mState->localChatNoticeShown = false;
                if (auto* chat = FindChatController(GetScene()))
                    chat->AddSystemMessage("서버 채팅에 연결되었습니다.");
                for (const auto& profile : players->AsArray()) ApplyRemoteProfile(profile);
                continue;
            }
            if (type == "JoinRejected" || type == "ProfileRejected")
            {
                std::string code = "unknown";
                if (const auto* error = envelope.Find("error"); error && error->IsObject())
                    if (const auto* value = error->Find("code"); value && value->IsString())
                        code = value->AsString();
                mState->profilePending = false;
                if (type == "JoinRejected")
                {
                    if (code != "name_duplicate" && code != "name_invalid" && code != "character_invalid")
                    {
                        CloseConnection();
                        mState->statusDetail = "서버 입장이 거절되었습니다: " + code;
                        return;
                    }
                    mState->connectionState = ConnectionState::NicknameRejected;
                    if (!mState->profileOpen) OpenProfile();
                }
                mState->profileOpen = true;
                mState->profileError = code == "name_duplicate" ? "이미 사용 중인 닉네임입니다."
                    : code == "character_invalid" ? "캐릭터를 다시 선택해 주세요."
                    : "사용할 수 없는 닉네임입니다.";
                continue;
            }
            if (type == "PlayerJoined" || type == "ProfileChanged")
            {
                if (!body || !body->IsObject() || mState->connectionState != ConnectionState::Multiplayer)
                    continue;
                const auto id = ReadUnsignedInteger(body->Find("id"));
                if (type == "ProfileChanged" && id && *id == mState->selfId)
                {
                    const auto character = ReadUnsignedInteger(body->Find("c"));
                    const auto* name = body->Find("name");
                    if (!character || *character >= CharacterCount || !name || !name->IsString() ||
                        !IsNicknameUsable(name->AsString()) ||
                        !IsProfileCharacterValid(name->AsString(), static_cast<unsigned int>(*character)))
                        throw Core::JsonError("invalid approved profile");
                    ApplyLocalProfile(name->AsString(), static_cast<unsigned int>(*character),
                        mState->requestedCharacter);
                    mState->profilePending = false;
                    mState->profileOpen = false;
                    mState->profileError.clear();
                }
                else ApplyRemoteProfile(*body);
                continue;
            }
            if (type == "ServerNotice")
            {
                if (mState->connectionState != ConnectionState::Multiplayer) continue;
                if (!body || !body->IsObject()) throw Core::JsonError("ServerNotice needs a body");
                const auto* kind = body->Find("kind");
                const auto* message = body->Find("text");
                const auto timestamp = ReadUnsignedInteger(body->Find("t"));
                if (!kind || !kind->IsString() || !message || !message->IsString() || !timestamp ||
                    !IsChatTextUsable(message->AsString()))
                    throw Core::JsonError("invalid server notice");
                ChatController::NoticeKind noticeKind;
                if (kind->AsString() == "announcement") noticeKind = ChatController::NoticeKind::Announcement;
                else if (kind->AsString() == "join") noticeKind = ChatController::NoticeKind::Join;
                else if (kind->AsString() == "leave") noticeKind = ChatController::NoticeKind::Leave;
                else throw Core::JsonError("invalid server notice kind");
                if (const auto* value = body->Find("id"))
                {
                    const auto id = ReadUnsignedInteger(value);
                    if (!id || *id == 0) throw Core::JsonError("invalid server notice id");
                }
                if (const auto* name = body->Find("name"); name &&
                    (!name->IsString() || !IsNicknameUsable(name->AsString())))
                    throw Core::JsonError("invalid server notice name");
                // 명단 재전송과 실제 입퇴장은 다르다. 서버의 별도 알림만 기록하고 말풍선은 만들지 않는다.
                // 퇴장 후에는 해당 ID가 명단에 없어도 서버가 보낸 당시 이름과 문장을 그대로 보존한다.
                if (auto* chat = FindChatController(GetScene())) chat->AddServerNotice(noticeKind, message->AsString());
                continue;
            }
            if (type == "ChatMessage")
            {
                if (mState->connectionState != ConnectionState::Multiplayer) continue;
                if (!body || !body->IsObject()) throw Core::JsonError("ChatMessage needs a body");
                const auto id = ReadUnsignedInteger(body->Find("id"));
                const auto timestamp = ReadUnsignedInteger(body->Find("t"));
                const auto* name = body->Find("name");
                const auto* message = body->Find("text");
                if (!id || *id == 0 || !timestamp || !name || !name->IsString() ||
                    !IsNicknameUsable(name->AsString()) || !message || !message->IsString() ||
                    !IsChatTextUsable(message->AsString()))
                    throw Core::JsonError("invalid chat message");
                // 디렉터리가 아직 페이지로 오는 동안에도 서버 승인 채팅은 먼저 도착할 수 있다.
                // 명단을 임의 생성하지 않고 기록만 남긴다. 동기화 후에는 전역 명단으로 발신자를 확인한다.
                if (mState->directoryReady && *id != mState->selfId && !mState->profiles.contains(*id)) continue;
                // 기록에는 발신 당시 이름을 보존하되 말풍선은 세션 ID로 현재 캐릭터에 붙인다.
                if (auto* chat = FindChatController(GetScene()))
                    chat->AddMessage(name->AsString(), message->AsString());
                if (*id == mState->selfId)
                {
                    if (auto* player = FindGameObject(GetScene(), "Player"))
                        SpeechBubble::ShowMessage(*player, message->AsString());
                }
                else
                {
                    const auto visible = mState->visibleRemotePlayers.find(*id);
                    if (visible != mState->visibleRemotePlayers.end() && visible->second.object)
                        SpeechBubble::ShowMessage(*visible->second.object, message->AsString());
                }
                continue;
            }
            if (type == "ChatRejected")
            {
                if (mState->connectionState != ConnectionState::Multiplayer) continue;
                std::string code;
                if (const auto* error = envelope.Find("error"); error && error->IsObject())
                    if (const auto* value = error->Find("code"); value && value->IsString())
                        code = value->AsString();
                if (auto* chat = FindChatController(GetScene()))
                    chat->AddSystemMessage(code == "chat_rate_limited"
                        ? "잠시 후 다시 보내 주세요. 채팅은 0.5초 간격으로 보낼 수 있습니다."
                        : "메시지를 보내지 못했습니다. 내용과 길이를 확인해 주세요.");
                continue;
            }
            if (type == "PlayerLeft")
            {
                const auto id = body && body->IsObject() ? ReadUnsignedInteger(body->Find("id")) : std::nullopt;
                if (!id) continue;
                RemoveVisiblePlayer(*mState, GetScene(), *id);
                mState->profiles.erase(*id);
                continue;
            }
            if (mState->connectionState != ConnectionState::Multiplayer) continue;
            if (type == "DirectoryPage")
            {
                const auto* players = body && body->IsObject() ? body->Find("players") : nullptr;
                const auto* cursor = body && body->IsObject() ? body->Find("cursor") : nullptr;
                if (!players || !players->IsArray() || players->AsArray().size() > 32 ||
                    !cursor || !cursor->IsString() || !ReadUnsignedInteger(cursor))
                    throw Core::JsonError("invalid directory page");
                // 페이지 사이에 live 프로필/퇴장 알림도 온다. 페이지를 받을 때 기존 명단을 비우지 않는다.
                for (const auto& profile : players->AsArray()) ApplyRemoteProfile(profile);
                Core::Json::Object ack;
                ack.emplace("cursor", Core::Json(cursor->AsString()));
                if (!QueueMessage(*mState, "DirectoryAck", Core::Json(std::move(ack))))
                    throw Core::JsonError("directory acknowledgement could not be queued");
                continue;
            }
            if (type == "DirectoryReady")
            {
                mState->directoryReady = true;
                continue;
            }
            if (type == "VisibilityEnter" || type == "StateBatch")
            {
                const bool entering = type == "VisibilityEnter";
                if (!entering && mState->udpNegotiated) continue;
                const auto* states = body && body->IsObject() ? body->Find(entering ? "players" : "states") : nullptr;
                if (!states || !states->IsArray() || states->AsArray().size() > 32)
                    throw Core::JsonError("invalid visibility batch");
                for (const auto& state : states->AsArray()) ApplyRemoteState(state, entering);
                continue;
            }
            if (type == "VisibilityExit")
            {
                const auto* ids = body && body->IsObject() ? body->Find("ids") : nullptr;
                if (!ids || !ids->IsArray() || ids->AsArray().size() > 32)
                    throw Core::JsonError("invalid visibility exit");
                for (const auto& value : ids->AsArray())
                {
                    const auto id = ReadUnsignedInteger(&value);
                    if (!id || *id == 0) throw Core::JsonError("invalid visibility exit id");
                    // 화면에서 사라지는 것과 게임에서 퇴장하는 것은 다르다. 전역 프로필/채팅 기록은 남는다.
                    RemoveVisiblePlayer(*mState, GetScene(), *id);
                }
                continue;
            }
            // VisibilityReady는 초기 가시 집합 완료 경계다. 빈 집합도 정상이며 UI/객체 생성은 Enter가 결정한다.
        }
        catch (const Core::JsonError& error)
        {
            GameEngine::Diagnostics::Debug::LogError("Summit server message rejected: ", error.what());
            CloseConnection();
            mState->statusDetail = "서버 메시지를 읽지 못해 싱글플레이로 전환했습니다.";
            return;
        }
    }
}

void MultiplayerController::SendLocalPlayerState(const float deltaTime)
{
    if (!mState || mState->connectionState != ConnectionState::Multiplayer)
    {
        return;
    }
    mState->stateSendAccumulator += (std::max)(deltaTime, 0.0f);
    if (mState->stateSendAccumulator < StateSendInterval)
    {
        return;
    }
    // 긴 프레임 뒤에도 오래된 상태를 연속 전송하지 않고 최신 상태 하나와 남은 시간만 유지한다.
    mState->stateSendAccumulator = std::fmod(mState->stateSendAccumulator, StateSendInterval);

    Runtime::GameObject* const player = FindGameObject(GetScene(), "Player");
    PlayerController* const controller =
        player ? player->GetComponent<PlayerController>() : nullptr;
    Runtime::Rigidbody2D* const body =
        player ? player->GetComponent<Runtime::Rigidbody2D>() : nullptr;
    Runtime::SpriteRenderer* const renderer =
        player ? PlayerController::FindVisualObject(*player).GetComponent<Runtime::SpriteRenderer>() : nullptr;
    if (!player || !controller || !body || !renderer)
    {
        return;
    }

    const Math::Vector3 position = player->GetTransform().GetWorldPosition();
    const Math::Vector2 velocity = body->GetVelocity();
    PlayerController::AnimationState animationState = PlayerController::AnimationState::Idle;
    if (!body->IsGrounded() || std::abs(velocity.GetY()) > 0.01f)
    {
        animationState = PlayerController::AnimationState::Jump;
    }
    else if (std::abs(velocity.GetX()) > 0.01f)
    {
        animationState = PlayerController::AnimationState::Walk;
    }

    Core::Json::Object stateBody;
    stateBody.emplace("x", Core::Json(static_cast<double>(position.GetX())));
    stateBody.emplace("y", Core::Json(static_cast<double>(position.GetY())));
    stateBody.emplace("vx", Core::Json(static_cast<double>(velocity.GetX())));
    stateBody.emplace("vy", Core::Json(static_cast<double>(velocity.GetY())));
    stateBody.emplace("f", Core::Json(controller->IsFacingLeft() ? -1.0 : 1.0));
    stateBody.emplace("s", Core::Json(std::string(AnimationStateName(animationState))));
    stateBody.emplace("c", Core::Json(static_cast<double>(
        static_cast<unsigned char>(controller->GetSelectedCharacter()))));
    if (mState->udpNegotiated)
    {
        // UDP는 최신 상태만 보낸다. WouldBlock/유실 시 다음 50ms 상태로 대체하며 TCP로 우회하지 않는다.
        if (mState->udpReady && mState->stateSequence != (std::numeric_limits<std::uint64_t>::max)())
        {
            stateBody.emplace("q", Core::Json(std::to_string(++mState->stateSequence)));
            static_cast<void>(SendUdpMessage(*mState, "PlayerState", Core::Json(std::move(stateBody))));
        }
        return;
    }
    if (!QueueMessage(*mState, "PlayerState", Core::Json(std::move(stateBody))))
    {
        CloseConnection();
        mState->statusDetail = "플레이어 상태를 보내지 못해 싱글플레이로 전환했습니다.";
    }
}

void MultiplayerController::HandleChatSubmission()
{
    auto* chat = FindChatController(GetScene());
    if (!mState || !chat) return;
    auto submitted = chat->ConsumeSubmittedMessage();
    if (!submitted || !mState->profileConfigured || mState->profileOpen || mState->profilePending) return;
    if (!IsChatTextUsable(*submitted))
    {
        chat->AddSystemMessage("채팅은 줄바꿈 없이 한글 170자 / 영문 512자 이내로 입력해 주세요.");
        return;
    }
    if (mState->connectionState == ConnectionState::OfflineSinglePlayer)
    {
        if (!mState->localChatNoticeShown)
        {
            chat->AddSystemMessage("싱글플레이 중입니다. 채팅은 이 화면에만 표시됩니다.");
            mState->localChatNoticeShown = true;
        }
        chat->AddMessage(mNickname, *submitted);
        if (auto* player = FindGameObject(GetScene(), "Player"))
            SpeechBubble::ShowMessage(*player, *submitted);
        return;
    }
    if (mState->connectionState != ConnectionState::Multiplayer) return;
    const auto now = std::chrono::steady_clock::now();
    if (mState->hasSentChat && now - mState->lastChatSent < std::chrono::milliseconds(500))
    {
        chat->AddSystemMessage("잠시 후 다시 보내 주세요. 채팅은 0.5초 간격으로 보낼 수 있습니다.");
        return;
    }
    Core::Json::Object body;
    body.emplace("text", Core::Json(std::move(*submitted)));
    if (!QueueMessage(*mState, "Chat", Core::Json(std::move(body))))
    {
        CloseConnection();
        chat->AddSystemMessage("메시지를 보내지 못했습니다. 서버 연결을 확인해 주세요.");
        return;
    }
    // 승인된 서버 반송만 기록한다. 로컬 선표시와 서버 반송이 중복되지 않는다.
    mState->lastChatSent = now;
    mState->hasSentChat = true;
}

void MultiplayerController::UpdateRemotePlayers(const float deltaTime)
{
    if (!mState || mState->connectionState != ConnectionState::Multiplayer)
    {
        return;
    }
    const float elapsed = (std::max)(deltaTime, 0.0f);
    for (auto& [id, remote] : mState->visibleRemotePlayers)
    {
        (void)id;
        if (!remote.object || !remote.hasState)
        {
            continue;
        }
        remote.blendElapsed += elapsed;
        remote.secondsSincePacket += elapsed;

        Math::Vector2 position = remote.targetPosition;
        if (remote.blendElapsed < remote.blendDuration)
        {
            const float t = remote.blendDuration > 0.0f
                ? remote.blendElapsed / remote.blendDuration
                : 1.0f;
            position = Math::Vector2::Lerp(remote.blendStart, remote.targetPosition, t);
        }
        else
        {
            // 새 패킷이 늦어져도 0.1초 이상 추측해 이동시키지 않는다. 다음 상태가 오면 다시 보간한다.
            const float extrapolation = (std::min)(
                remote.blendElapsed - remote.blendDuration, MaximumExtrapolationSeconds);
            position = remote.targetPosition + remote.velocity * extrapolation;
        }

        const float z = remote.object->GetTransform().GetPosition().GetZ();
        remote.object->GetTransform().SetPosition({ position.GetX(), position.GetY(), z });
    }
}

void MultiplayerController::OpenProfile()
{
    if (!mState) return;
    if (auto* chat = FindChatController(GetScene())) chat->CancelEditing();
    mState->profileOpen = true;
    mState->draftCharacter = mState->preferredCharacter;
    mState->profileError.clear();
    if (auto* object = FindGameObject(GetScene(), NicknameInputObject))
    {
        if (auto* input = object->GetComponent<Runtime::InputField>())
            input->SetText(mNickname);
    }
}

void MultiplayerController::ApplyLocalProfile(const std::string& nickname,
    const unsigned int character, const unsigned int preferredCharacter)
{
    mNickname = nickname;
    mState->preferredCharacter = preferredCharacter < SelectableCharacterCount ? preferredCharacter : 0;
    mState->profileConfigured = true;
    if (auto* controller = FindLocalPlayerController(GetScene()))
    {
        controller->SetSelectedCharacter(static_cast<PlayerController::Character>(character));
        controller->UpdateNameplate(*controller->GetGameObject(), nickname);
    }
}

void MultiplayerController::HandleUi()
{
    if (!mState) return;
    if (mState->profileOpen) HandleAudioUi();
    Runtime::Scene* const scene = GetScene();
    const auto clicked = [scene](const std::string_view name)
    {
        auto* object = FindGameObject(scene, name);
        auto* button = object ? object->GetComponent<Runtime::Button>() : nullptr;
        return button && button->WasClickedThisFrame();
    };
    const auto& inputState = GetRuntimeContext()->GetInput();
    if (inputState.GetKeyDown(GameEngine::Platform::Key::Escape) &&
        !inputState.GetState().WasKeyHandledByIme(GameEngine::Platform::Key::Escape))
    {
        if (const auto* chat = FindChatController(scene); chat && chat->WasInputConsumedThisFrame()) return;
        if (!mState->profileOpen) OpenProfile();
        else if (mState->profileConfigured && !mState->profilePending)
            mState->profileOpen = false;
        return;
    }
    if (!mState->profileOpen)
    {
        if (clicked(ReconnectButtonObject)) BeginConnect();
        return;
    }
    if (mState->profilePending) return;
    if (mState->profileConfigured && clicked("ProfileCancelButton"))
    {
        mState->profileOpen = false;
        return;
    }
    if (mState->connectionState == ConnectionState::Connecting ||
        mState->connectionState == ConnectionState::AwaitingJoin) return;

    auto* inputObject = FindGameObject(scene, NicknameInputObject);
    auto* input = inputObject ? inputObject->GetComponent<Runtime::InputField>() : nullptr;
    if (!input) return;
    const std::string proposed = input->GetText();
    if (!UsesEasterEggCharacter(proposed))
    {
        for (unsigned int index = 0; index < CharacterButtons.size(); ++index)
            if (clicked(CharacterButtons[index])) mState->draftCharacter = index;
    }
    if (!(clicked(NicknameSubmitButtonObject) || input->WasSubmittedThisFrame())) return;
    if (!IsNicknameUsable(proposed))
    {
        mState->profileError = "닉네임을 입력해 주세요. 한글 16자 / 영문 48자 이내입니다.";
        return;
    }
    mState->profileError.clear();
    if (mState->connectionState == ConnectionState::Multiplayer ||
        (mState->connectionState == ConnectionState::NicknameRejected && mState->socket != INVALID_SOCKET))
    {
        bool queued = false;
        if (mState->connectionState == ConnectionState::NicknameRejected)
        {
            queued = QueueJoin(*mState, proposed, mState->draftCharacter);
            mState->connectionState = ConnectionState::AwaitingJoin;
            mState->joinStarted = std::chrono::steady_clock::now();
        }
        else
        {
            Core::Json::Object body;
            body.emplace("name", Core::Json(proposed));
            body.emplace("c", Core::Json(static_cast<double>(mState->draftCharacter)));
            queued = QueueMessage(*mState, "SetProfile", Core::Json(std::move(body)));
            if (queued) mState->requestedCharacter = mState->draftCharacter;
        }
        if (!queued)
        {
            CloseConnection();
            mState->profileError = "변경 요청을 보내지 못했습니다. 다시 적용해 주세요.";
            return;
        }
        mState->profilePending = true;
        // 온라인 외형과 닉네임은 서버 승인에서만 바꾼다. 거절되면 이전 확정 프로필을 유지한다.
        mState->profileStarted = std::chrono::steady_clock::now();
        return;
    }
    const bool firstProfile = !mState->profileConfigured;
    ApplyLocalProfile(proposed, ResolveProfileCharacter(proposed, mState->draftCharacter),
        mState->draftCharacter);
    mState->profileOpen = false;
    if (mState->connectionState == ConnectionState::NicknameRejected) CloseConnection();
    if (firstProfile) BeginConnect();
}

void MultiplayerController::HandleAudioUi()
{
    // BGM 설정은 로컬에 즉시 적용한다. 프로필 저장/취소와 무관하며 볼륨 변경은 재생 위치를 리셋하지 않는다.
    auto* object = FindGameObject(GetScene(), "BgmAudio");
    auto* audio = object ? object->GetComponent<Runtime::AudioSource>() : nullptr;
    if (!mState || !audio) return;
    const auto clicked = [this](const std::string_view name)
    {
        const auto* target = FindGameObject(GetScene(), name);
        const auto* button = target ? target->GetComponent<Runtime::Button>() : nullptr;
        return button && button->IsActiveAndEnabled() && button->WasClickedThisFrame();
    };
    if (clicked("BgmMuteButton"))
    {
        if (audio->GetVolume() > 0)
        {
            mState->bgmUnmutedVolume = audio->GetVolume();
            audio->SetVolume(0);
        }
        else audio->SetVolume(mState->bgmUnmutedVolume > 0 ? mState->bgmUnmutedVolume : 0.25f);
    }
    const int direction = (clicked("BgmVolumeUpButton") ? 1 : 0) -
        (clicked("BgmVolumeDownButton") ? 1 : 0);
    if (direction != 0)
    {
        const float previous = audio->GetVolume();
        const float next = std::clamp(previous + direction * 0.1f, 0.0f, 1.0f);
        // 0으로 내린 뒤에도 음소거 해제는 마지막으로 들리던 음량으로 돌아간다.
        if (previous > 0) mState->bgmUnmutedVolume = previous;
        audio->SetVolume(next);
        if (next > 0) mState->bgmUnmutedVolume = next;
    }
}

void MultiplayerController::RefreshUi()
{
    if (!mState) return;
    Runtime::Scene* const scene = GetScene();
    const auto setText = [scene](const std::string_view name, const std::string& value)
    {
        if (auto* object = FindGameObject(scene, name))
            if (auto* text = object->GetComponent<Runtime::TextRenderer>()) text->SetText(value);
    };
    const ConnectionState state = mState->connectionState;
    const bool open = mState->profileOpen;
    const bool busy = mState->profilePending || state == ConnectionState::Connecting ||
        state == ConnectionState::AwaitingJoin;
    const auto* bgmObject = FindGameObject(scene, "BgmAudio");
    const auto* bgm = bgmObject ? bgmObject->GetComponent<Runtime::AudioSource>() : nullptr;
    if (bgm)
    {
        const float volume = bgm->GetVolume();
        setText("BgmVolumeText", std::to_string(static_cast<int>(std::lround(volume * 100))) + "%");
        setText("BgmMuteButton", volume <= 0 ? "소리 켜기" : "음소거");
        for (const auto name : { "BgmVolumeDownButton", "BgmVolumeUpButton", "BgmMuteButton" })
            if (auto* target = FindGameObject(scene, name))
                if (auto* button = target->GetComponent<Runtime::Button>()) button->SetInteractable(open);
    }
    SetInteractiveObjectActive(FindGameObject(scene, "ProfilePanel"), open);
    SetInteractiveObjectActive(FindGameObject(scene, ReconnectButtonObject),
        !open && mState->profileConfigured &&
        (state == ConnectionState::OfflineSinglePlayer || state == ConnectionState::NicknameRejected));
    for (const auto name : { NicknameInputObject, NicknameSubmitButtonObject,
            std::string_view("ProfileCancelButton") })
    {
        auto* object = FindGameObject(scene, name);
        const bool visible = open && (name != "ProfileCancelButton" || mState->profileConfigured);
        SetInteractiveObjectActive(object, visible);
        const bool enabled = visible && (name == "ProfileCancelButton" ? !mState->profilePending : !busy);
        if (object)
        {
            if (auto* button = object->GetComponent<Runtime::Button>()) button->SetInteractable(enabled);
            if (auto* input = object->GetComponent<Runtime::InputField>()) input->SetInteractable(enabled);
        }
    }
    const auto* inputObject = FindGameObject(scene, NicknameInputObject);
    const auto* nicknameInput = inputObject ? inputObject->GetComponent<Runtime::InputField>() : nullptr;
    const bool automaticCharacter = nicknameInput && UsesEasterEggCharacter(nicknameInput->GetText());
    for (unsigned int index = 0; index < CharacterButtons.size(); ++index)
    {
        auto* object = FindGameObject(scene, CharacterButtons[index]);
        SetInteractiveObjectActive(object, open);
        if (auto* button = object ? object->GetComponent<Runtime::Button>() : nullptr)
            button->SetInteractable(open && !busy && !automaticCharacter);
        std::string label(CharacterNames[index]);
        if (!automaticCharacter && mState->draftCharacter == index) label += " [선택]";
        setText(CharacterButtons[index], label);
    }
    setText("ProfileCharacterLabel", automaticCharacter ? "특별한 캐릭터가 자동 선택됩니다." : "캐릭터");
    setText("NicknameSubmitButton", busy ? "기다리는 중..." : mState->profileConfigured ? "적용" : "시작");
    setText("ProfileErrorText", mState->profileError);
    auto* chat = FindChatController(scene);
    if (chat)
        chat->SetAvailable(mState->profileConfigured && !open && !busy &&
            (state == ConnectionState::OfflineSinglePlayer || state == ConnectionState::Multiplayer));
    if (auto* controller = FindLocalPlayerController(scene))
        controller->SetGameplayInputEnabled(mState->profileConfigured && !open &&
            (!chat || (!chat->IsEditing() && !chat->WasInputConsumedThisFrame())));

    std::string status;
    switch (state)
    {
    case ConnectionState::Connecting: status = "서버 연결 중..."; break;
    case ConnectionState::AwaitingJoin: status = "서버 입장 중..."; break;
    case ConnectionState::NicknameRejected: status = "입장 대기 | Esc로 프로필 수정"; break;
    case ConnectionState::Multiplayer: status = "멀티플레이 | " + mNickname; break;
    default:
        status = mState->profileConfigured ? "싱글플레이 | " + mNickname : "닉네임과 캐릭터를 설정해 주세요.";
        break;
    }
    setText(NetworkStatusObject, status);
    setText("NetworkDetailText", open ? "" : mState->statusDetail);
}
void MultiplayerController::CloseConnection(const bool preserveNicknamePrompt)
{
    if (!mState)
    {
        return;
    }
    if (mState->profilePending)
    {
        mState->profileOpen = true;
        mState->profileError = "서버 연결이 끊겼습니다. 변경 내용을 다시 적용해 주세요.";
    }
    if (mState->connectionState == ConnectionState::Multiplayer)
    {
        if (auto* chat = FindChatController(GetScene()))
            chat->AddSystemMessage("서버 연결이 끊겼습니다. 이후 채팅은 이 화면에만 표시됩니다.");
        mState->localChatNoticeShown = true;
    }
    mState->profilePending = false;
    CloseSocket(*mState);
    if (Runtime::Scene* const scene = GetScene())
    {
        for (auto& [id, remote] : mState->visibleRemotePlayers)
        {
            (void)id;
            if (remote.object)
            {
                scene->RemoveGameObject(remote.object->GetInstanceId());
            }
        }
    }
    mState->visibleRemotePlayers.clear();
    mState->profiles.clear();
    mState->connectionState = preserveNicknamePrompt
        ? ConnectionState::NicknameRejected
        : ConnectionState::OfflineSinglePlayer;
}

}

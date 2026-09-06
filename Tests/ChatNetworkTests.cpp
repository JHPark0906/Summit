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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Assets/AssetDatabase.h"
#include "Core/Json.h"
#include "Gameplay/PlayerController.h"
#include "Network/MultiplayerController.h"
#include "Platform/DirectoryContentSource.h"
#include "Platform/IAudioOutput.h"
#include "Platform/IInput.h"
#include "Platform/ITextMeasure.h"
#include "Runtime/Game.h"
#include "Runtime/AudioSource.h"
#include "Runtime/GameObject.h"
#include "Runtime/Input.h"
#include "Runtime/InputField.h"
#include "Runtime/RectTransform.h"
#include "Runtime/Scene.h"
#include "Runtime/SceneManager.h"
#include "Runtime/SpriteAnimator.h"
#include "Runtime/SpriteRenderer.h"
#include "Runtime/TextRenderer.h"
#include "Runtime/Transform.h"
#include "Serialization/SceneSerializer.h"
#include "UI/ChatController.h"
#include "UI/SpeechBubble.h"

namespace
{
namespace Runtime = GameEngine::Runtime;
namespace Platform = GameEngine::Platform;
namespace Serialization = GameEngine::Serialization;
using Json = GameEngine::Core::Json;
using ConnectionState = Summit::MultiplayerController::ConnectionState;

void Require(const bool condition, const std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

struct WinsockLifetime
{
    WinsockLifetime()
    {
        WSADATA data{};
        Require(::WSAStartup(MAKEWORD(2, 2), &data) == 0, "chat TCP fixture could not initialize Winsock");
    }
    ~WinsockLifetime() { ::WSACleanup(); }
};

struct Socket
{
    SOCKET value = INVALID_SOCKET;
    Socket() = default;
    ~Socket() { Close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    void Close()
    {
        if (value != INVALID_SOCKET) ::closesocket(value);
        value = INVALID_SOCKET;
    }
};

// The peer and the real game client are pumped on the same test thread. No worker,
// fixed developer port, external server process, or timing-based response script is needed.
class LoopbackPeer
{
public:
    LoopbackPeer()
    {
        mListener.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Require(mListener.value != INVALID_SOCKET, "chat TCP fixture could not create its listener");
        const BOOL exclusive = TRUE;
        Require(::setsockopt(mListener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0,
            "chat TCP fixture could not reserve its port exclusively");
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        endpoint.sin_port = 0;
        Require(::bind(mListener.value, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0 &&
            ::listen(mListener.value, 1) == 0, "chat TCP fixture could not bind an ephemeral loopback port");
        int size = sizeof(endpoint);
        Require(::getsockname(mListener.value, reinterpret_cast<sockaddr*>(&endpoint), &size) == 0,
            "chat TCP fixture could not read its port");
        mPort = ::ntohs(endpoint.sin_port);
        SetNonBlocking(mListener.value);
    }

    [[nodiscard]] int Port() const { return mPort; }
    [[nodiscard]] std::size_t StateCount() const { return mStateCount; }

    void Pump()
    {
        if (mPeer.value == INVALID_SOCKET)
        {
            mPeer.value = ::accept(mListener.value, nullptr, nullptr);
            if (mPeer.value == INVALID_SOCKET)
            {
                Require(::WSAGetLastError() == WSAEWOULDBLOCK, "chat TCP fixture accept failed");
                return;
            }
            SetNonBlocking(mPeer.value);
            const BOOL noDelay = TRUE;
            Require(::setsockopt(mPeer.value, IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char*>(&noDelay), sizeof(noDelay)) == 0,
                "chat TCP fixture could not disable Nagle buffering");
        }
        while (!mOutgoing.empty())
        {
            const int sent = ::send(mPeer.value, mOutgoing.data(), static_cast<int>(mOutgoing.size()), 0);
            if (sent == SOCKET_ERROR)
            {
                Require(::WSAGetLastError() == WSAEWOULDBLOCK, "chat TCP fixture send failed");
                break;
            }
            Require(sent > 0, "chat TCP fixture send made no progress");
            mOutgoing.erase(0, static_cast<std::size_t>(sent));
        }
        std::array<char, 8192> bytes{};
        for (;;)
        {
            const int received = ::recv(mPeer.value, bytes.data(), static_cast<int>(bytes.size()), 0);
            if (received == SOCKET_ERROR)
            {
                Require(::WSAGetLastError() == WSAEWOULDBLOCK, "chat TCP fixture receive failed");
                break;
            }
            Require(received > 0, "the real chat client unexpectedly closed its socket");
            mIncoming.append(bytes.data(), static_cast<std::size_t>(received));
        }
        while (mIncoming.size() >= 4)
        {
            std::uint32_t length = 0;
            for (unsigned int index = 0; index < 4; ++index)
                length |= static_cast<std::uint32_t>(static_cast<unsigned char>(mIncoming[index])) << (index * 8);
            Require(length > 0 && length <= 65536, "the real client sent an invalid frame length");
            if (mIncoming.size() - 4 < length) break;
            mFrames.push_back(Json::Parse(std::string_view(mIncoming).substr(4, length)));
            if (mFrames.back().At("type").AsString() == "PlayerState") ++mStateCount;
            mIncoming.erase(0, 4 + length);
        }
    }

    void Send(const std::string_view json)
    {
        Require(json.size() <= 65536, "chat TCP fixture response exceeds the wire frame limit");
        const auto length = static_cast<std::uint32_t>(json.size());
        for (unsigned int index = 0; index < 4; ++index)
            mOutgoing.push_back(static_cast<char>((length >> (index * 8)) & 0xff));
        mOutgoing.append(json);
    }

    [[nodiscard]] std::optional<Json> Take(const std::string_view expectedType)
    {
        while (!mFrames.empty())
        {
            Json value = std::move(mFrames.front());
            mFrames.pop_front();
            const auto& type = value.At("type").AsString();
            if (type == "PlayerState" && expectedType != "PlayerState") continue;
            Require(type == expectedType, "the real client sent an unexpected control message: " + type);
            return value;
        }
        return std::nullopt;
    }

    void Disconnect() { mPeer.Close(); }

private:
    static void SetNonBlocking(const SOCKET socket)
    {
        u_long enabled = 1;
        Require(::ioctlsocket(socket, FIONBIO, &enabled) == 0,
            "chat TCP fixture could not make its socket nonblocking");
    }
    WinsockLifetime mWinsock;
    Socket mListener;
    Socket mPeer;
    int mPort = 0;
    std::string mIncoming;
    std::string mOutgoing;
    std::deque<Json> mFrames;
    std::size_t mStateCount = 0;
};

// Deliberately writes/reads the documented wire bytes independently of DatagramCodec.
class UdpPeer
{
public:
    static constexpr std::string_view TokenHex = "00112233445566778899aabbccddeeff";
    UdpPeer()
    {
        mSocket.value = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        Require(mSocket.value != INVALID_SOCKET, "could not create UDP test peer");
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        Require(::bind(mSocket.value, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0,
            "could not bind UDP test peer");
        int size = sizeof(endpoint);
        Require(::getsockname(mSocket.value, reinterpret_cast<sockaddr*>(&endpoint), &size) == 0,
            "could not read UDP test port");
        mPort = ::ntohs(endpoint.sin_port);
        u_long enabled = 1;
        Require(::ioctlsocket(mSocket.value, FIONBIO, &enabled) == 0, "UDP test socket is blocking");
    }
    int Port() const { return mPort; }
    std::size_t HelloCount() const { return mHelloCount; }
    std::size_t StateCount() const { return mStateCount; }
    const Json& LastState() const { return mLastState; }
    void Pump()
    {
        std::array<char, 1201> bytes{};
        for (;;)
        {
            int peerSize = sizeof(mClient);
            const int size = ::recvfrom(mSocket.value, bytes.data(), static_cast<int>(bytes.size()), 0,
                reinterpret_cast<sockaddr*>(&mClient), &peerSize);
            if (size == SOCKET_ERROR)
            {
                Require(::WSAGetLastError() == WSAEWOULDBLOCK, "UDP test peer receive failed");
                return;
            }
            Require(size > 28 && size <= 1200 && std::string_view(bytes.data(), 4) == "SMU1",
                "client did not send a bounded SMU1 UDP datagram");
            for (unsigned int index = 0; index < 16; ++index)
                Require(static_cast<unsigned char>(bytes[4 + index]) == index * 17,
                    "client sent the wrong negotiated UDP token");
            std::uint64_t sequence = 0;
            for (unsigned int index = 20; index < 28; ++index)
                sequence = (sequence << 8) | static_cast<unsigned char>(bytes[index]);
            Require(sequence > mLastSequence, "upstream UDP packet sequence was reused or reordered");
            mLastSequence = sequence;
            const auto envelope = Json::Parse(std::string_view(bytes.data() + 28, static_cast<std::size_t>(size) - 28));
            const auto& type = envelope.At("type").AsString();
            if (type == "UdpHello") ++mHelloCount;
            else
            {
                Require(type == "PlayerState", "reliable control traffic leaked onto UDP");
                const auto q = std::stoull(envelope.At("body").At("q").AsString());
                Require(q > mLastStateSequence, "client state q did not increase exactly");
                mLastStateSequence = q;
                mLastState = envelope.At("body");
                ++mStateCount;
            }
        }
    }
    void Send(const std::string_view json, const std::uint64_t sequence,
        const bool wrongToken = false, const bool foreignPort = false)
    {
        Require(mClient.sin_port != 0, "UDP test response requires an observed Hello endpoint");
        std::string bytes = "SMU1";
        for (unsigned int index = 0; index < 16; ++index)
            bytes.push_back(static_cast<char>(index * 17));
        for (unsigned int index = 0; index < 8; ++index)
            bytes.push_back(static_cast<char>((sequence >> ((7 - index) * 8)) & 0xff));
        bytes.append(json);
        if (wrongToken) bytes[4] ^= 1;
        Socket foreign;
        if (foreignPort) foreign.value = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const SOCKET sender = foreignPort ? foreign.value : mSocket.value;
        Require(::sendto(sender, bytes.data(), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&mClient), sizeof(mClient)) == static_cast<int>(bytes.size()),
            "UDP test peer could not send its datagram");
    }
private:
    WinsockLifetime mWinsock;
    Socket mSocket;
    sockaddr_in mClient{};
    int mPort = 0;
    std::uint64_t mLastSequence = 0;
    std::uint64_t mLastStateSequence = 0;
    std::size_t mHelloCount = 0;
    std::size_t mStateCount = 0;
    Json mLastState;
};

struct Fixture
{
    Platform::DirectoryContentSource content{ std::filesystem::path(SUMMIT_TEST_CONTENT_DIRECTORY) };
    Runtime::Game game{ nullptr, nullptr };
    Runtime::Scene* scene = nullptr;

    explicit Fixture(const int port)
    {
        Require(game.GetAssetDatabase().Refresh(content), "chat TCP test could not register production assets");
        std::vector<std::byte> bytes;
        Require(content.Read("Scenes/InGame.scene", bytes), "chat TCP test could not read the production scene");
        auto loaded = Serialization::SceneSerializer::LoadFromBytes(
            bytes, "Scenes/InGame.scene", game.GetRuntimeContext());
        Require(loaded != nullptr, "chat TCP test could not load the production scene");
        scene = loaded.get();
        auto& network = Component<Summit::MultiplayerController>("NetworkManager");
        network.SetServerAddress("127.0.0.1");
        network.SetServerPort(port);
        Require(game.GetSceneManager().AddScene(std::move(loaded)) != 0, "chat TCP scene could not be adopted");
        game.SetRenderSurfaceSize(1280, 720);
        game.SetRenderAspectRatio(1280.0f / 720.0f);
        Step();
    }

    template<class T> T& Component(const char* name)
    {
        auto* object = scene->FindGameObject(name);
        auto* component = object ? object->GetComponent<T>() : nullptr;
        Require(component != nullptr, "chat TCP fixture is missing component on " + std::string(name));
        return *component;
    }

    void Step(Platform::InputState state = {})
    {
        state.hasFocus = true;
        game.GetInput().BeginFrameWithState(state);
        game.Update(0);
    }

    void Press(const Platform::Key key)
    {
        Platform::InputState state;
        state.SetKey(key, true);
        Step(state);
        Step();
        Step(); // Consume events produced by the input-field pass.
    }

    void Click(const char* name)
    {
        const auto& rect = Component<Runtime::RectTransform>(name).GetResolvedRect();
        Require(!rect.IsEmpty(), "chat TCP fixture clicked an unlaid-out control");
        Platform::InputState state;
        state.cursor = { static_cast<int>(rect.GetCenterX()), static_cast<int>(rect.GetCenterY()) };
        Step(state);
        state.SetMouseButton(Platform::MouseButton::Left, true);
        Step(state);
        state.TakeAccumulated();
        state.SetMouseButton(Platform::MouseButton::Left, false);
        Step(state);
        state.TakeAccumulated();
        Step(state);
    }

    void SubmitChat(const std::string& text)
    {
        Press(Platform::Key::Enter);
        Require(Component<Summit::ChatController>("NetworkManager").IsEditing(), "chat did not open from Enter");
        Component<Runtime::InputField>("ChatInput").SetText(text);
        Step();
        Press(Platform::Key::Enter);
        Require(!Component<Summit::ChatController>("NetworkManager").IsEditing(), "chat submission did not close input");
    }

    [[nodiscard]] std::size_t CountRow(const std::string_view text) const
    {
        std::size_t result = 0;
        for (const auto& [id, object] : scene->GetGameObjects())
        {
            (void)id;
            const auto* renderer = object->GetComponent<Runtime::TextRenderer>();
            if (object->GetName() == "ChatMessage" && renderer && renderer->GetText() == text) ++result;
        }
        return result;
    }

    template<class Predicate> void Wait(LoopbackPeer* peer, Predicate condition)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do
        {
            Step();
            // A deliberate protocol-rejection check may already have closed the client socket.
            if (condition()) return;
            if (peer) peer->Pump();
            if (condition()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("chat TCP fixture timed out waiting for the real client");
    }

    [[nodiscard]] Json Read(LoopbackPeer& peer, const std::string_view type)
    {
        std::optional<Json> value;
        Wait(&peer, [&] { value = peer.Take(type); return value.has_value(); });
        return std::move(*value);
    }
};

const Runtime::GameObject* BubbleRoot(const Runtime::GameObject& player)
{
    for (const auto* child : player.GetTransform().GetChildren())
        if (child->GetGameObject()->GetComponent<Summit::SpeechBubble>()) return child->GetGameObject();
    return nullptr;
}

const Runtime::TextRenderer* BubbleText(const Runtime::GameObject& player)
{
    if (const auto* root = BubbleRoot(player))
        for (const auto* child : root->GetTransform().GetChildren())
            if (child->GetGameObject()->GetName() == "SpeechBubbleText")
                return child->GetGameObject()->GetComponent<Runtime::TextRenderer>();
    return nullptr;
}

std::size_t SpeechObjectCount(const Runtime::Scene& scene)
{
    std::size_t count = 0;
    for (const auto& [id, object] : scene.GetGameObjects())
    {
        static_cast<void>(id);
        const auto& name = object->GetName();
        if (name == "SpeechBubble" || name == "SpeechBubbleText" || name == "SpeechBubbleTail") ++count;
    }
    return count;
}

void RealClientUsesAuthoritativeChatAndRetainsHistoryAfterDisconnect()
{
    LoopbackPeer peer;
    Fixture fixture(peer.Port());
    auto& network = fixture.Component<Summit::MultiplayerController>("NetworkManager");
    auto& chat = fixture.Component<Summit::ChatController>("NetworkManager");
    fixture.Component<Runtime::InputField>("NicknameInput").SetText("LocalChat");
    fixture.Step();
    fixture.Click("NicknameSubmitButton");
    const Json join = fixture.Read(peer, "Join");
    Require(join.At("body").At("schemaVersion").AsNumber() == 6 &&
        join.At("body").At("name").AsString() == "LocalChat", "real profile UI did not send a v6 Join");
    peer.Send(R"({"type":"JoinAccepted","body":{"schemaVersion":6,"id":"101","name":"LocalChat","c":0,"players":[],"capacity":16,"directoryThroughId":"202"}})");
    fixture.Wait(&peer, [&] { return network.GetConnectionState() == ConnectionState::Multiplayer && chat.IsAvailable(); });

    peer.Send(R"({"type":"ChatMessage","body":{"id":"202","name":"RemoteChat","text":"during directory sync","t":0}})");
    peer.Send(R"({"type":"DirectoryPage","body":{"players":[{"id":"202","name":"RemoteChat","c":2}],"cursor":"202"}})");
    const Json ack = fixture.Read(peer, "DirectoryAck");
    Require(ack.At("body").At("cursor").AsString() == "202" &&
        fixture.CountRow("RemoteChat: during directory sync") == 1,
        "directory paging lost early global chat or did not acknowledge the exact cursor");
    peer.Send(R"({"type":"DirectoryReady","body":{}})");
    peer.Send(R"({"type":"VisibilityReady","body":{}})");

    auto& localPlayer = *fixture.scene->FindGameObject("Player");
    const auto spawnPosition = localPlayer.GetTransform().GetPosition();
    Require(BubbleRoot(localPlayer) == nullptr, "system join messages must not create character speech");
    const auto rowsBeforeNotices = chat.GetMessageCount();
    peer.Send(R"({"type":"PlayerJoined","body":{"id":"202","name":"RemoteChat","c":2}})");
    peer.Send(R"({"type":"PlayerJoined","body":{"id":"606","name":"RosterOnly","c":1}})");
    peer.Send(R"({"type":"ServerNotice","body":{"kind":"announcement","text":"  서버 공지: 함께 즐겨 주세요.  ","t":0}})");
    peer.Send(R"({"type":"ServerNotice","body":{"kind":"join","id":"101","name":"LocalChat","text":"LocalChat님이 입장했습니다.","t":1}})");
    peer.Send(R"({"type":"PlayerLeft","body":{"id":"606"}})");
    peer.Send(R"({"type":"ServerNotice","body":{"kind":"leave","id":"606","name":"RosterOnly","text":"RosterOnly님이 퇴장했습니다.","t":2}})");
    fixture.Wait(&peer, [&] { return fixture.CountRow("RosterOnly님이 퇴장했습니다.") == 1; });
    Require(chat.GetMessageCount() == rowsBeforeNotices + 3 &&
        fixture.CountRow("  서버 공지: 함께 즐겨 주세요.  ") == 1 &&
        fixture.CountRow("LocalChat님이 입장했습니다.") == 1 && SpeechObjectCount(*fixture.scene) == 0,
        "notices changed their text, created speech, or roster replay invented join/leave notifications");
    for (const auto& [id, object] : fixture.scene->GetGameObjects())
    {
        static_cast<void>(id);
        const auto* row = object->GetComponent<Runtime::TextRenderer>();
        if (object->GetName() != "ChatMessage" || !row) continue;
        const auto color = row->GetColor();
        if (row->GetText() == "  서버 공지: 함께 즐겨 주세요.  ")
            Require(color.g > color.r && color.g > color.b, "server announcement is not green");
        if (row->GetText() == "LocalChat님이 입장했습니다." || row->GetText() == "RosterOnly님이 퇴장했습니다.")
            Require(color.r > color.b && color.g > color.b && color.g > .7f,
                "server join/leave notice is not yellow");
    }
    peer.Send(R"({"type":"ChatMessage","body":{"id":"202","name":"RemoteChat","text":"before first position","t":998}})");
    fixture.Wait(&peer, [&] { return fixture.CountRow("RemoteChat: before first position") == 1; });
    Require(fixture.scene->FindGameObject("RemotePlayer-202") == nullptr,
        "speech before the first state invented a remote player at the origin");

    peer.Send(R"({"type":"VisibilityEnter","body":{"players":[{"id":"202","x":3,"y":4,"vx":0,"vy":1,"f":1,"s":"jump","c":2,"t":999,"name":"RemoteChat","q":0}]}})");
    fixture.Wait(&peer, [&] { return fixture.scene->FindGameObject("RemotePlayer-202") != nullptr; });
    auto* remote = fixture.scene->FindGameObject("RemotePlayer-202");
    Require(BubbleRoot(*remote) == nullptr,
        "far-player speech was replayed when the player became visible later");
    for (const auto& [id, object] : fixture.scene->GetGameObjects())
    {
        (void)id;
        for (auto* transform = &object->GetTransform(); transform; transform = transform->GetParent())
        {
            if (transform->GetGameObject() == remote)
                Require(object->GetComponent<Runtime::AudioSource>() == nullptr,
                    "a relayed remote jump inherited a local AudioSource");
        }
    }

    const auto before = chat.GetMessageCount();
    const std::string original = "  한글 👋  ";
    fixture.Press(Platform::Key::Enter);
    fixture.Component<Runtime::InputField>("ChatInput").SetText("  한");
    Platform::InputState imeEnter;
    imeEnter.compositionId = 1;
    imeEnter.committedCompositionId = 1;
    imeEnter.imeEnterCompositionId = 1;
    imeEnter.typedText = "글 👋  ";
    imeEnter.SetKey(Platform::Key::Enter, true);
    imeEnter.MarkKeyHandledByIme(Platform::Key::Enter);
    fixture.Step(imeEnter);
    fixture.Step();
    fixture.Step();
    const Json submitted = fixture.Read(peer, "Chat");
    Require(submitted.At("body").At("text").AsString() == original && submitted.At("body").Size() == 1,
        "real chat input changed Unicode/spaces or supplied client-owned identity fields");
    Require(chat.GetMessageCount() == before, "online chat was displayed before the server accepted it");
    Require(BubbleRoot(localPlayer) == nullptr, "local speech was displayed before the server approved the message");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"999","name":"LocalChat","text":"unknown speaker","t":999}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"101","name":"LocalChat","text":"  한글 👋  ","t":1000}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"202","name":"RemoteChat","text":"remote message","t":1001}})");
    fixture.Wait(&peer, [&] { return chat.GetMessageCount() == before + 2; });
    Require(fixture.CountRow("LocalChat: " + original) == 1 && fixture.CountRow("RemoteChat: remote message") == 1,
        "server self echo was duplicated or an approved remote nickname was lost");
    Require(BubbleText(localPlayer) && BubbleText(localPlayer)->GetText() == original &&
        BubbleText(*remote)->GetText() == "remote message" && fixture.CountRow("LocalChat: unknown speaker") == 0,
        "approved speech was not mapped to the authoritative local/remote player id");
    const auto remoteBubbleId = BubbleRoot(*remote)->GetInstanceId();
    const auto remoteBubbleTextId = BubbleText(*remote)->GetGameObject()->GetInstanceId();

    // A server rejection is a non-terminal outcome. The rejected draft must not enter history.
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    fixture.SubmitChat("rejected draft");
    const Json rejected = fixture.Read(peer, "Chat");
    Require(rejected.At("body").At("text").AsString() == "rejected draft", "second chat request was not sent");
    peer.Send(R"({"type":"ChatRejected","error":{"code":"chat_rate_limited"}})");
    fixture.Wait(&peer, [&] { return chat.GetMessageCount() == before + 3; });
    Require(network.GetConnectionState() == ConnectionState::Multiplayer &&
        fixture.CountRow("LocalChat: rejected draft") == 0,
        "ChatRejected disconnected the real client or recorded rejected content");
    Require(fixture.CountRow("잠시 후 다시 보내 주세요. 채팅은 0.5초 간격으로 보낼 수 있습니다.") == 1,
        "ChatRejected did not show the rate-limit feedback");
    Require(BubbleText(localPlayer)->GetText() == original,
        "a rejected draft or its system feedback replaced approved character speech");

    fixture.Press(Platform::Key::Escape);
    fixture.Component<Runtime::InputField>("NicknameInput").SetText("RenamedLocal");
    fixture.Step();
    fixture.Click("NicknameSubmitButton");
    const Json profile = fixture.Read(peer, "SetProfile");
    Require(profile.At("body").At("name").AsString() == "RenamedLocal" && network.GetNickname() == "LocalChat",
        "profile UI applied a new name before server approval");
    peer.Send(R"({"type":"ProfileChanged","body":{"id":"101","name":"RenamedLocal","c":0}})");
    peer.Send(R"({"type":"ProfileChanged","body":{"id":"202","name":"RenamedRemote","c":2}})");
    // Chat history must use the message's approved name snapshot, not the current roster's name.
    peer.Send(R"({"type":"ChatMessage","body":{"id":"202","name":"RemoteChat","text":"older name snapshot","t":1002}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"101","name":"RenamedLocal","text":"new name snapshot","t":1003}})");
    fixture.Wait(&peer, [&] { return network.GetNickname() == "RenamedLocal" && chat.GetMessageCount() == before + 5; });
    Require(fixture.CountRow("LocalChat: " + original) == 1 && fixture.CountRow("RemoteChat: remote message") == 1 &&
        fixture.CountRow("RemoteChat: older name snapshot") == 1 && fixture.CountRow("RenamedLocal: new name snapshot") == 1,
        "profile changes rewrote history or chat used a roster nickname instead of the server snapshot");
    Require(BubbleRoot(*remote)->GetInstanceId() == remoteBubbleId &&
        BubbleText(*remote)->GetText() == "older name snapshot" &&
        BubbleText(localPlayer)->GetText() == "new name snapshot",
        "nickname changes broke speech ownership or duplicated its child objects");

    peer.Send(R"({"type":"PlayerJoined","body":{"id":"303","name":"LateState","c":1}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"303","name":"LateState","text":"expires before position","t":1004}})");
    fixture.Wait(&peer, [&] { return fixture.CountRow("LateState: expires before position") == 1; });
    fixture.scene->FindGameObject("NetworkManager")->Update(6.1f);
    peer.Send(R"({"type":"VisibilityEnter","body":{"players":[{"id":"303","x":5,"y":4,"vx":0,"vy":0,"f":0,"s":"idle","c":1,"t":1005,"name":"LateState","q":0}]}})");
    fixture.Wait(&peer, [&] { return fixture.scene->FindGameObject("RemotePlayer-303") != nullptr; });
    Require(BubbleRoot(*fixture.scene->FindGameObject("RemotePlayer-303")) == nullptr,
        "expired pending speech reappeared when a delayed first state arrived");
    peer.Send(R"({"type":"PlayerLeft","body":{"id":"202"}})");
    fixture.Wait(&peer, [&] { return fixture.scene->FindGameObject("RemotePlayer-202") == nullptr; });
    Require(fixture.scene->GetGameObject(remoteBubbleId) == nullptr &&
        fixture.scene->GetGameObject(remoteBubbleTextId) == nullptr,
        "a remote departure left its speech bubble in the scene");

    // Send() appends frames to one buffer; Pump() writes the entire batch before the
    // client next updates. A just-created bubble and even its player may still be pending.
    peer.Send(R"({"type":"PlayerJoined","body":{"id":"404","name":"ImmediateLeft","c":0}})");
    peer.Send(R"({"type":"VisibilityEnter","body":{"players":[{"id":"404","x":7,"y":4,"vx":0,"vy":0,"f":0,"s":"idle","c":0,"t":1006,"name":"ImmediateLeft","q":0}]}})");
    fixture.Wait(&peer, [&] { return fixture.scene->FindGameObject("RemotePlayer-404") != nullptr; });
    Require(BubbleRoot(*fixture.scene->FindGameObject("RemotePlayer-404")) == nullptr,
        "new remote fixture already has speech before its first message");
    const auto speechBeforeLeave = SpeechObjectCount(*fixture.scene);
    peer.Send(R"({"type":"ChatMessage","body":{"id":"404","name":"ImmediateLeft","text":"first speech then left","t":1007}})");
    peer.Send(R"({"type":"PlayerLeft","body":{"id":"404"}})");
    fixture.Wait(&peer, [&] { return fixture.scene->FindGameObject("RemotePlayer-404") == nullptr; });
    Require(fixture.CountRow("ImmediateLeft: first speech then left") == 1 &&
        SpeechObjectCount(*fixture.scene) == speechBeforeLeave,
        "same-batch first speech and departure left pending bubble children orphaned");

    peer.Send(R"({"type":"PlayerJoined","body":{"id":"505","name":"PendingPlayer","c":1}})");
    peer.Send(R"({"type":"VisibilityEnter","body":{"players":[{"id":"505","x":9,"y":4,"vx":0,"vy":0,"f":0,"s":"idle","c":1,"t":1008,"name":"PendingPlayer","q":0}]}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"505","name":"PendingPlayer","text":"appeared and left together","t":1009}})");
    peer.Send(R"({"type":"PlayerLeft","body":{"id":"505"}})");
    fixture.Wait(&peer, [&]
    {
        return fixture.CountRow("PendingPlayer: appeared and left together") == 1 &&
            fixture.scene->FindGameObject("RemotePlayer-505") == nullptr;
    });
    Require(SpeechObjectCount(*fixture.scene) == speechBeforeLeave,
        "same-batch remote state, first speech and departure leaked a pending subtree");

    peer.Send(R"({"type":"VisibilityExit","body":{"ids":["303"]}})");
    peer.Send(R"({"type":"StateBatch","body":{"states":[{"id":"303","x":80,"y":40,"vx":1,"vy":0,"f":1,"s":"run","c":1,"t":1100,"q":0}]}})");
    peer.Send(R"({"type":"ProfileChanged","body":{"id":"303","name":"심심이심셔","c":5}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"303","name":"심심이심셔","text":"outside visibility","t":1101}})");
    fixture.Wait(&peer, [&] { return fixture.CountRow("심심이심셔: outside visibility") == 1; });
    Require(fixture.scene->FindGameObject("RemotePlayer-303") == nullptr,
        "an exited player's state/profile/chat recreated its visual or removed it from global chat");
    peer.Send(R"({"type":"VisibilityEnter","body":{"players":[{"id":"303","name":"심심이심셔","x":100,"y":100,"vx":0,"vy":0,"f":-1,"s":"idle","c":5,"t":2000,"q":42}]}})");
    fixture.Wait(&peer, [&] { return fixture.scene->FindGameObject("RemotePlayer-303") != nullptr; });
    auto* visibleTdw = fixture.scene->FindGameObject("RemotePlayer-303");
    const auto tdwId = visibleTdw->GetInstanceId();
    const auto& tdwVisual = Summit::PlayerController::FindVisualObject(*visibleTdw);
    Require(visibleTdw->GetTransform().GetPosition().GetX() == 100 &&
        visibleTdw->GetTransform().GetPosition().GetY() == 100 && BubbleRoot(*visibleTdw) == nullptr &&
        tdwVisual.GetComponent<Runtime::SpriteAnimator>()->IsPingPong() &&
        tdwVisual.GetTransform().GetScale().GetX() == 2 && !tdwVisual.GetComponent<Runtime::SpriteRenderer>()->IsFlippedX(),
        "AOI enter did not snap its full current profile/state or revived old far-player speech");
    peer.Send(R"({"type":"StateBatch","body":{"states":[{"id":"303","x":110,"y":100,"vx":0,"vy":0,"f":-1,"s":"idle","c":5,"t":2050,"q":18446744073709551615}]}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"303","name":"심심이심셔","text":"visible again","t":2051}})");
    fixture.Wait(&peer, [&] { return fixture.CountRow("심심이심셔: visible again") == 1; });
    Require(BubbleText(*visibleTdw) && BubbleText(*visibleTdw)->GetText() == "visible again",
        "visible global chat did not attach speech to the active server id");
    fixture.scene->FindGameObject("NetworkManager")->Update(.025f);
    Require(std::abs(visibleTdw->GetTransform().GetPosition().GetX() - 105) < .01f,
        "StateBatch did not preserve timestamp-based interpolation for visible players");
    peer.Send(R"({"type":"VisibilityExit","body":{"ids":["303"]}})");
    peer.Send(R"({"type":"StateBatch","body":{"states":[{"id":"303","x":120,"y":100,"vx":0,"vy":0,"f":1,"s":"run","c":5,"t":2100,"q":44}]}})");
    peer.Send(R"({"type":"VisibilityEnter","body":{"players":[{"id":"303","name":"Returned","x":-30,"y":12,"vx":0,"vy":0,"f":1,"s":"idle","c":1,"t":9000,"q":45}]}})");
    fixture.Wait(&peer, [&]
    {
        const auto* player = fixture.scene->FindGameObject("RemotePlayer-303");
        return player && player->GetInstanceId() != tdwId;
    });
    auto* reentered = fixture.scene->FindGameObject("RemotePlayer-303");
    Require(reentered->GetTransform().GetPosition().GetX() == -30 &&
        reentered->GetTransform().GetPosition().GetY() == 12 && BubbleRoot(*reentered) == nullptr &&
        !Summit::PlayerController::FindVisualObject(*reentered).GetComponent<Runtime::SpriteAnimator>()->IsPingPong(),
        "same-batch exit/re-entry retained stale position, interpolation, speech or tdw ping-pong");

    const auto speechBeforeExit = SpeechObjectCount(*fixture.scene);
    peer.Send(R"({"type":"VisibilityEnter","body":{"players":[{"id":"909","name":"BrieflyVisible","x":9,"y":4,"vx":0,"vy":0,"f":1,"s":"idle","c":0,"t":9100,"q":0}]}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"909","name":"BrieflyVisible","text":"exit in same batch","t":9101}})");
    peer.Send(R"({"type":"VisibilityExit","body":{"ids":["909"]}})");
    fixture.Wait(&peer, [&] { return fixture.CountRow("BrieflyVisible: exit in same batch") == 1; });
    Require(fixture.scene->FindGameObject("RemotePlayer-909") == nullptr &&
        SpeechObjectCount(*fixture.scene) == speechBeforeExit,
        "same-batch AOI enter/chat/exit left a pending visual or speech subtree behind");

    localPlayer.GetTransform().SetPosition({ 12345, -9, spawnPosition.GetZ() });
    fixture.scene->FindGameObject("NetworkManager")->Update(.05f);
    fixture.Wait(&peer, [&]
    {
        const auto state = peer.Take("PlayerState");
        return state && state->At("body").At("x").AsNumber() == 12345;
    });
    localPlayer.GetTransform().SetPosition({ 12345, -11, spawnPosition.GetZ() });
    fixture.game.Update(.05f);
    const Json respawn = fixture.Read(peer, "PlayerState");
    Require(std::abs(respawn.At("body").At("x").AsNumber() - spawnPosition.GetX()) < .01 &&
        std::abs(respawn.At("body").At("y").AsNumber() - spawnPosition.GetY()) < .01,
        "the first state after falling below the limit did not relay the initial spawn position");

    peer.Disconnect();
    fixture.Wait(nullptr, [&] { return network.GetConnectionState() == ConnectionState::OfflineSinglePlayer && chat.IsAvailable(); });
    Require(network.GetNickname() == "RenamedLocal" && fixture.CountRow("LocalChat: " + original) == 1,
        "disconnect discarded the committed profile or earlier chat history");
    fixture.SubmitChat("offline after disconnect");
    Require(fixture.CountRow("RenamedLocal: offline after disconnect") == 1 &&
        network.GetConnectionState() == ConnectionState::OfflineSinglePlayer &&
        BubbleText(localPlayer)->GetText() == "offline after disconnect",
        "disconnected chat did not fall back to exactly one local message");
}

void BudgetedStateBatchesPreserveRemotesAndBoundPrediction()
{
    LoopbackPeer peer;
    Fixture fixture(peer.Port());
    auto& network = fixture.Component<Summit::MultiplayerController>("NetworkManager");
    auto* manager = fixture.scene->FindGameObject("NetworkManager");
    fixture.Component<Runtime::InputField>("NicknameInput").SetText("BudgetLocal");
    fixture.Step();
    fixture.Click("NicknameSubmitButton");
    static_cast<void>(fixture.Read(peer, "Join"));
    peer.Send(R"({"type":"JoinAccepted","body":{"schemaVersion":6,"id":"101","name":"BudgetLocal","c":0,"players":[],"capacity":4,"directoryThroughId":"404"}})");
    peer.Send(R"({"type":"DirectoryPage","body":{"players":[{"id":"202","name":"Mover","c":0},{"id":"404","name":"Still","c":1}],"cursor":"404"}})");
    Require(fixture.Read(peer, "DirectoryAck").At("body").At("cursor").AsString() == "404",
        "budgeted-state fixture did not finish its profile directory");
    peer.Send(R"({"type":"DirectoryReady","body":{}})");

    unsigned int barrierNumber = 0;
    const auto receive = [&](const std::string_view message)
    {
        peer.Send(message);
        const auto barrier = "budget barrier " + std::to_string(++barrierNumber);
        peer.Send("{\"type\":\"ServerNotice\",\"body\":{\"kind\":\"announcement\",\"text\":\"" +
            barrier + "\",\"t\":1}}");
        // TCP order proves the preceding batch has been consumed, without advancing simulated time.
        fixture.Wait(&peer, [&] { return fixture.CountRow(barrier) == 1; });
    };
    const auto expectPosition = [](const Runtime::GameObject& object, const float x, const float y,
        const std::string_view reason)
    {
        const auto position = object.GetTransform().GetPosition();
        Require(std::abs(position.GetX() - x) < .01f && std::abs(position.GetY() - y) < .01f, reason);
    };
    receive(R"({"type":"VisibilityEnter","body":{"players":[{"id":"202","name":"Mover","c":0,"x":10,"y":20,"vx":2,"vy":-4,"f":1,"s":"walk","t":1000,"q":0},{"id":"404","name":"Still","c":1,"x":-5,"y":10,"vx":0,"vy":0,"f":1,"s":"idle","t":1000,"q":0}]}})");
    auto* mover = fixture.scene->FindGameObject("RemotePlayer-202");
    auto* still = fixture.scene->FindGameObject("RemotePlayer-404");
    Require(mover && still, "initial visibility snapshot did not create both budgeted remotes");
    const auto moverId = mover->GetInstanceId();
    const auto stillId = still->GetInstanceId();
    expectPosition(*mover, 10, 20, "the first visible snapshot must be applied immediately");

    // A very short server interval still interpolates; a batch is a subset, not a replacement roster.
    receive(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":12,"y":22,"vx":2,"vy":-4,"f":1,"s":"walk","t":1005,"q":8}]}})");
    manager->Update(.0125f);
    expectPosition(*mover, 11, 21, "sub-25ms timestamps must not turn a deferred state into an immediate teleport");
    expectPosition(*still, -5, 10, "an omitted stationary remote changed when another player was updated");
    manager->Update(.5f);
    expectPosition(*mover, 12.2f, 21.6f, "missing updates must extrapolate velocity for at most 100ms");
    manager->Update(2.0f);
    expectPosition(*mover, 12.2f, 21.6f, "a long scheduling gap kept extrapolating a moving remote indefinitely");

    receive(R"({"type":"StateBatch","body":{"states":[{"id":"404","c":1,"x":-3,"y":10,"vx":0,"vy":0,"f":1,"s":"idle","t":1050,"q":9}]}})");
    manager->Update(.025f);
    expectPosition(*still, -4, 10, "an independently scheduled remote lost its own timestamp interpolation");
    expectPosition(*mover, 12.2f, 21.6f, "a batch for another ID reset an omitted remote's prediction timer");
    receive(R"({"type":"StateBatch","body":{"states":[]}})");
    manager->Update(.025f);
    expectPosition(*still, -3, 10, "an empty tick batch reset an in-progress interpolation");
    Require(fixture.scene->GetGameObject(moverId) == mover && fixture.scene->GetGameObject(stillId) == still,
        "missing or empty StateBatch entries removed or recreated a visible player");

    // Resuming after seconds of starvation must blend from the displayed position, with a bounded catch-up.
    receive(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":30,"y":40,"vx":2,"vy":-4,"f":1,"s":"walk","t":4000,"q":100}]}})");
    manager->Update(.075f);
    expectPosition(*mover, 21.1f, 30.8f, "a long interval did not blend from the capped displayed position over 150ms");
    manager->Update(.075f);
    expectPosition(*mover, 30, 40, "a deferred state introduced seconds of additional interpolation delay");
    manager->Update(.4f);
    expectPosition(*mover, 30.2f, 39.6f, "prediction after a deferred state exceeded its distance bound");
    receive(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":30.15,"y":39.7,"vx":0,"vy":0,"f":1,"s":"idle","t":4050,"q":101}]}})");
    manager->Update(3.0f);
    expectPosition(*mover, 30.15f, 39.7f, "a delivered stop state retained velocity from an earlier moving state");
    expectPosition(*still, -3, 10, "an unchanged stationary remote drifted during a long update gap");
    const auto& local = fixture.Component<Summit::PlayerController>("Player");
    const auto& visual = Summit::PlayerController::FindVisualObject(*mover);
    Require(visual.GetComponent<Runtime::SpriteRenderer>()->GetSprite() == local.GetAtlas(
        Summit::PlayerController::Character::CharacterA, Summit::PlayerController::AnimationState::Idle),
        "the deferred stop state failed to replace the remote walking animation");

    receive(R"({"type":"VisibilityExit","body":{"ids":["202"]}})");
    receive(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":999,"y":999,"vx":2,"vy":-4,"f":1,"s":"walk","t":4100,"q":102}]}})");
    Require(fixture.scene->FindGameObject("RemotePlayer-202") == nullptr,
        "a delayed state recreated a player after its visibility exit");
    receive(R"({"type":"VisibilityEnter","body":{"players":[{"id":"202","name":"Mover","c":0,"x":-20,"y":8,"vx":0,"vy":0,"f":1,"s":"idle","t":9000,"q":200}]}})");
    mover = fixture.scene->FindGameObject("RemotePlayer-202");
    Require(mover && mover->GetInstanceId() != moverId, "visibility re-entry reused a removed remote object");
    expectPosition(*mover, -20, 8, "visibility re-entry interpolated through an obsolete deferred position");
    manager->Update(2.0f);
    expectPosition(*mover, -20, 8, "visibility re-entry retained an old velocity or prediction timer");
    Require(network.GetConnectionState() == ConnectionState::Multiplayer,
        "valid sparse state delivery incorrectly disconnected the client");
}

void NegotiatedUdpMovementSurvivesLossReorderingAndVisibilityBoundaries()
{
    LoopbackPeer peer;
    UdpPeer udp;
    Fixture fixture(peer.Port());
    auto& network = fixture.Component<Summit::MultiplayerController>("NetworkManager");
    auto* manager = fixture.scene->FindGameObject("NetworkManager");
    fixture.Component<Runtime::InputField>("NicknameInput").SetText("UdpLocal");
    fixture.Step();
    fixture.Click("NicknameSubmitButton");
    const auto join = fixture.Read(peer, "Join");
    Require(join.At("body").At("movementTransport").AsString() == "udp", "client did not request UDP movement");
    peer.Send("{\"type\":\"JoinAccepted\",\"body\":{\"schemaVersion\":6,\"id\":\"101\",\"name\":\"UdpLocal\",\"c\":0,\"players\":[],\"directoryThroughId\":\"404\",\"udp\":{\"port\":" +
        std::to_string(udp.Port()) + ",\"token\":\"" + std::string(UdpPeer::TokenHex) + "\"}}}");
    const auto wait = [&](auto&& condition, const std::chrono::milliseconds timeout = std::chrono::seconds(3))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do
        {
            fixture.Step();
            peer.Pump();
            manager->Update(.05f);
            udp.Pump();
            if (condition()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("UDP client regression timed out");
    };
    wait([&] { return udp.HelloCount() >= 2; }); // Drop the first Hello/Ready exchange.
    Require(udp.StateCount() == 0 && peer.StateCount() == 0,
        "movement started before UDP Ready or silently fell back to TCP");
    udp.Send(R"({"type":"UdpReady","body":{}})", 1);
    wait([&] { return udp.StateCount() >= 2; });
    const auto& localPosition = fixture.scene->FindGameObject("Player")->GetTransform().GetWorldPosition();
    Require(std::abs(udp.LastState().At("x").AsNumber() - localPosition.GetX()) < .01 &&
        std::abs(udp.LastState().At("y").AsNumber() - localPosition.GetY()) < .01,
        "UDP PlayerState did not contain the actual local player position");
    peer.Send(R"({"type":"DirectoryPage","body":{"players":[{"id":"202","name":"Mover","c":0},{"id":"404","name":"Barrier","c":1}],"cursor":"404"}})");
    Require(fixture.Read(peer, "DirectoryAck").At("body").At("cursor").AsString() == "404",
        "UDP negotiation broke reliable directory acknowledgement");
    peer.Send(R"({"type":"DirectoryReady","body":{}})");
    // A valid state received before reliable Enter must not create an object.
    udp.Send(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":999,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":1,"q":"1","r":"9007199254740993"}]}})", 3);
    wait([&] { return udp.StateCount() >= 3; });
    Require(fixture.scene->FindGameObject("RemotePlayer-202") == nullptr,
        "a pre-Enter UDP state created an unknown remote");
    peer.Send(R"({"type":"VisibilityEnter","body":{"players":[{"id":"202","name":"Mover","c":0,"x":10,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":100,"q":"1","r":"9007199254740993"},{"id":"404","name":"Barrier","c":1,"x":0,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":100,"q":"1","r":"1"}]}})");
    wait([&] { return fixture.scene->FindGameObject("RemotePlayer-202") != nullptr; });
    const auto position = [&](const char* objectName)
    {
        const auto* object = fixture.scene->FindGameObject(objectName);
        Require(object != nullptr, "UDP regression lost a visible object");
        return object->GetTransform().GetPosition().GetX();
    };
    Require(std::abs(position("RemotePlayer-202") - 10) < .01f, "Enter did not establish the exact revision baseline");
    std::uint64_t barrierRevision = 1;
    const auto barrier = [&]
    {
        const auto next = ++barrierRevision;
        udp.Send("{\"type\":\"StateBatch\",\"body\":{\"states\":[{\"id\":\"404\",\"c\":1,\"x\":" +
            std::to_string(next) + ",\"y\":0,\"vx\":0,\"vy\":0,\"f\":1,\"s\":\"idle\",\"t\":100,\"r\":\"" +
            std::to_string(next) + "\"}]}}", 10 + next);
        wait([&] { return std::abs(position("RemotePlayer-404") - static_cast<float>(next)) < .01f; });
    };
    // Newer packet first, then an older packet with another ID: per-packet ordering must not lose the mover.
    barrier();
    udp.Send(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":20,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":150,"r":"9007199254740994"}]}})", 2);
    wait([&] { return std::abs(position("RemotePlayer-202") - 20) < .01f; });
    // One greater-than-double-exact revision is enough; equal/older revisions cannot reset position.
    udp.Send(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":999,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":200,"r":"9007199254740994"}]}})", 999);
    udp.Send(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":998,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":200,"r":"9007199254740993"}]}})", 998);
    // A bad final entry rejects the whole datagram, including its otherwise valid first revision.
    udp.Send(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":997,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":200,"r":"9007199254740995"},{"id":"404","r":"oops"}]}})", 997);
    udp.Send("not JSON", 996);
    udp.Send(std::string(1200, 'x'), 995);
    const auto future = R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":996,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":200,"r":"9007199254740995"}]}})";
    udp.Send(future, 994, true);
    udp.Send(future, 993, false, true);
    barrier();
    Require(std::abs(position("RemotePlayer-202") - 20) < .01f,
        "duplicate, stale, malformed, oversized, wrong-token, or foreign-endpoint UDP changed state");
    // The real backend still emits numeric uint64 q and permits finite facing/arbitrary short animation.
    // Those diagnostic/display fields must not poison the whole batch or be mistaken for exact r ordering.
    ++barrierRevision;
    udp.Send("{\"type\":\"StateBatch\",\"body\":{\"states\":["
        "{\"id\":\"202\",\"c\":0,\"x\":25,\"y\":0,\"vx\":0,\"vy\":0,\"f\":0,\"s\":\"custom\",\"t\":175,\"q\":18446744073709551615,\"r\":\"9007199254740995\"},"
        "{\"id\":\"404\",\"c\":1,\"x\":" + std::to_string(barrierRevision) +
        ",\"y\":0,\"vx\":0,\"vy\":0,\"f\":-0.25,\"s\":\"\",\"t\":175,\"q\":9007199254740993,\"r\":\"" +
        std::to_string(barrierRevision) + "\"}]}}", 4);
    wait([&] { return std::abs(position("RemotePlayer-202") - 25) < .01f &&
        std::abs(position("RemotePlayer-404") - static_cast<float>(barrierRevision)) < .01f; });
    const auto* customRenderer = Summit::PlayerController::FindVisualObject(
        *fixture.scene->FindGameObject("RemotePlayer-202")).GetComponent<Runtime::SpriteRenderer>();
    Require(customRenderer->GetSprite() == fixture.Component<Summit::PlayerController>("Player").GetAtlas(
        Summit::PlayerController::Character::CharacterA, Summit::PlayerController::AnimationState::Idle),
        "a backend-approved custom animation failed to use the idle display fallback");
    udp.Send(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":30,"y":0,"vx":2,"vy":0,"f":1,"s":"run","t":200,"r":"9007199254740996"}]}})", 4);
    wait([&] { return position("RemotePlayer-202") > 30.1f; });
    manager->Update(2.0f); // Lose the first stop update and several subsequent movement packets.
    Require(std::abs(position("RemotePlayer-202") - 30.2f) < .01f, "UDP loss caused unbounded extrapolation");
    udp.Send(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":30.1,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":250,"r":"9007199254740997"}]}})", 5);
    wait([&] { return std::abs(position("RemotePlayer-202") - 30.1f) < .01f; });
    manager->Update(2.0f);
    Require(std::abs(position("RemotePlayer-202") - 30.1f) < .01f,
        "a periodic stationary correction failed to stop a remote after packet loss");
    peer.Send(R"({"type":"ProfileChanged","body":{"id":"202","name":"Renamed","c":2}})");
    peer.Send(R"({"type":"VisibilityExit","body":{"ids":["202"]}})");
    wait([&] { return fixture.scene->FindGameObject("RemotePlayer-202") == nullptr; });
    udp.Send(future, 6);
    barrier();
    Require(!fixture.scene->FindGameObject("RemotePlayer-202"), "UDP recreated a remote after Exit");
    peer.Send(R"({"type":"VisibilityEnter","body":{"players":[{"id":"202","name":"Renamed","c":2,"x":-20,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":400,"r":"18446744073709551614"}]}})");
    wait([&] { return fixture.scene->FindGameObject("RemotePlayer-202") != nullptr; });
    udp.Send(future, 7);
    barrier();
    Require(std::abs(position("RemotePlayer-202") + 20) < .01f, "old-incarnation UDP overrode a re-Enter baseline");
    udp.Send(R"({"type":"StateBatch","body":{"states":[{"id":"202","c":0,"x":-10,"y":0,"vx":0,"vy":0,"f":1,"s":"idle","t":450,"r":"18446744073709551615"}]}})", 8);
    wait([&] { return std::abs(position("RemotePlayer-202") + 10) < .01f; });
    auto* remote = fixture.scene->FindGameObject("RemotePlayer-202");
    const auto* renderer = Summit::PlayerController::FindVisualObject(*remote).GetComponent<Runtime::SpriteRenderer>();
    Require(renderer->GetSprite() == fixture.Component<Summit::PlayerController>("Player").GetAtlas(
        Summit::PlayerController::Character::CharacterC, Summit::PlayerController::AnimationState::Idle),
        "old UDP character data overrode the reliable approved profile");
    // Silence affects movement status, not the TCP session; an idle TCP heartbeat preserves server liveness.
    wait([&] { return fixture.Component<Runtime::TextRenderer>("NetworkDetailText").GetText().find("응답이 없습니다") != std::string::npos; },
        std::chrono::seconds(4));
    Require(network.GetConnectionState() == ConnectionState::Multiplayer, "UDP silence disconnected reliable chat");
    std::optional<Json> heartbeat;
    wait([&] { heartbeat = peer.Take("Heartbeat"); return heartbeat.has_value(); }, std::chrono::seconds(6));
    Require(heartbeat->At("body").IsObject() && peer.StateCount() == 0,
        "TCP heartbeat missing or UDP movement silently fell back to TCP");
    const auto beforeRecovery = udp.StateCount();
    udp.Send(R"({"type":"UdpReady","body":{}})", 1); // Ready itself is idempotent; downstream packet order is not global.
    wait([&] { return udp.StateCount() > beforeRecovery; });
    Require(fixture.Component<Runtime::TextRenderer>("NetworkDetailText").GetText().empty(),
        "valid UDP Ready did not recover the movement status");
    fixture.SubmitChat("UDP와 독립적인 채팅");
    Require(fixture.Read(peer, "Chat").At("body").At("text").AsString() == "UDP와 독립적인 채팅",
        "reliable chat stopped working after UDP loss and recovery");
    peer.Disconnect();
    fixture.Wait(nullptr, [&] { return network.GetConnectionState() == ConnectionState::OfflineSinglePlayer; });
    const auto afterClose = udp.StateCount();
    manager->Update(.1f);
    udp.Pump();
    Require(udp.StateCount() == afterClose && !fixture.scene->FindGameObject("RemotePlayer-202"),
        "UDP kept a stale session or visible object alive after TCP teardown");
}

void DirectoryPagesKeepLiveProfilesAndLeaveRemovesGlobalMembership()
{
    LoopbackPeer peer;
    Fixture fixture(peer.Port());
    auto& network = fixture.Component<Summit::MultiplayerController>("NetworkManager");
    fixture.Component<Runtime::InputField>("NicknameInput").SetText("DirectoryLocal");
    fixture.Step();
    fixture.Click("NicknameSubmitButton");
    static_cast<void>(fixture.Read(peer, "Join"));
    peer.Send(R"({"type":"JoinAccepted","body":{"schemaVersion":6,"id":"101","name":"DirectoryLocal","c":0,"players":[],"capacity":1000,"directoryThroughId":"404"}})");
    peer.Send(R"({"type":"DirectoryPage","body":{"players":[{"id":"202","name":"FirstPage","c":0}],"cursor":"202"}})");
    Require(fixture.Read(peer, "DirectoryAck").At("body").At("cursor").AsString() == "202",
        "the first directory page was not acknowledged");
    peer.Send(R"({"type":"PlayerJoined","body":{"id":"909","name":"JoinedDuringPages","c":1}})");
    peer.Send(R"({"type":"ProfileChanged","body":{"id":"202","name":"RenamedDuringPages","c":2}})");
    peer.Send(R"({"type":"DirectoryPage","body":{"players":[{"id":"404","name":"SecondPage","c":3}],"cursor":"404"}})");
    Require(fixture.Read(peer, "DirectoryAck").At("body").At("cursor").AsString() == "404",
        "a later directory page was not acknowledged independently");
    peer.Send(R"({"type":"DirectoryReady","body":{}})");
    peer.Send(R"({"type":"VisibilityReady","body":{}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"202","name":"RenamedDuringPages","text":"first page retained","t":1}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"909","name":"JoinedDuringPages","text":"live delta retained","t":2}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"404","name":"SecondPage","text":"last page retained","t":3}})");
    fixture.Wait(&peer, [&] { return fixture.CountRow("SecondPage: last page retained") == 1; });
    Require(fixture.CountRow("RenamedDuringPages: first page retained") == 1 &&
        fixture.CountRow("JoinedDuringPages: live delta retained") == 1 &&
        fixture.scene->FindGameObject("RemotePlayer-202") == nullptr &&
        fixture.scene->FindGameObject("RemotePlayer-909") == nullptr && SpeechObjectCount(*fixture.scene) == 0,
        "a directory page erased prior/live profiles or global chat created invisible-player visuals");
    peer.Send(R"({"type":"PlayerLeft","body":{"id":"909"}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"909","name":"JoinedDuringPages","text":"stale after departure","t":4}})");
    peer.Send(R"({"type":"ChatMessage","body":{"id":"404","name":"SecondPage","text":"departure barrier","t":5}})");
    fixture.Wait(&peer, [&] { return fixture.CountRow("SecondPage: departure barrier") == 1; });
    Require(fixture.CountRow("JoinedDuringPages: stale after departure") == 0,
        "PlayerLeft removed only the visible object and retained a departed global chat identity");

    peer.Send(R"({"type":"StateBatch","body":{"states":[{"id":"404","x":1,"y":2,"vx":0,"vy":0,"f":1,"s":"idle","c":3,"t":-1,"q":0}]}})");
    fixture.Wait(&peer, [&] { return network.GetConnectionState() == ConnectionState::OfflineSinglePlayer; });
    Require(fixture.scene->FindGameObject("RemotePlayer-404") == nullptr,
        "a malformed AOI state bypassed validation or created a remote without VisibilityEnter");
}
}

void RunChatNetworkTests()
{
    RealClientUsesAuthoritativeChatAndRetainsHistoryAfterDisconnect();
    DirectoryPagesKeepLiveProfilesAndLeaveRemovesGlobalMembership();
    BudgetedStateBatchesPreserveRemotesAndBoundPrediction();
    NegotiatedUdpMovementSurvivesLossReorderingAndVisibilityBoundaries();
}

#pragma once

#include <memory>
#include <string>

#include "Runtime/MonoBehaviour.h"

namespace GameEngine::Core { class Json; }

namespace Summit
{

class MultiplayerStateData;

/// <summary>
/// Summit의 최소 멀티플레이 클라이언트다. 연결되지 않은 동안에는 로컬 게임을 그대로 두고,
/// 연결된 동안에만 로컬 상태를 보내고 원격 플레이어를 표시한다.
/// </summary>
class MultiplayerController final : public GameEngine::Runtime::MonoBehaviour
{
public:
    enum class ConnectionState : unsigned char
    {
        // 접속 상태와 프로필 확정은 별개다. 싱글플레이도 최초 프로필을 저장해야 조작을 시작한다.
        OfflineSinglePlayer,
        Connecting,
        AwaitingJoin,
        NicknameRejected,
        Multiplayer,
    };

    MultiplayerController();
    ~MultiplayerController() override;

    [[nodiscard]] static const GameEngine::Runtime::ComponentType& StaticType();
    [[nodiscard]] const GameEngine::Runtime::ComponentType& GetComponentType() const override
    {
        return StaticType();
    }

    [[nodiscard]] const std::string& GetServerAddress() const { return mServerAddress; }
    void SetServerAddress(std::string address);

    [[nodiscard]] int GetServerPort() const { return mServerPort; }
    void SetServerPort(int port);

    [[nodiscard]] const std::string& GetNickname() const { return mNickname; }
    void SetNickname(std::string nickname);

    [[nodiscard]] ConnectionState GetConnectionState() const;

protected:
    void Start() override;
    void Update(float deltaTime) override;
    void OnDestroy() override;

private:
    void BeginConnect();
    void PollConnection();
    void PollSocket();
    void PollUdp();
    void ApplyRemoteProfile(const GameEngine::Core::Json& profile);
    void ApplyRemoteState(const GameEngine::Core::Json& state, bool entering);
    void ProcessIncomingFrames();
    void SendLocalPlayerState(float deltaTime);
    void HandleChatSubmission();
    void UpdateRemotePlayers(float deltaTime);
    void HandleUi();
    void HandleAudioUi();
    void OpenProfile();
    void ApplyLocalProfile(const std::string& nickname, unsigned int character,
        unsigned int preferredCharacter);
    void RefreshUi();
    void CloseConnection(bool preserveNicknamePrompt = false);

    std::string mServerAddress = "127.0.0.1";
    int mServerPort = 17150;
    std::string mNickname;
    // 소켓·프레이밍·승인 대기·원격 표시 상태를 소유한다. 장면의 GameObject 소유권은 Scene에 남는다.
    std::unique_ptr<MultiplayerStateData> mState;
};

}

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "CharacterProfile.h"
#include "Assets/AssetReference.h"
#include "Math/Vector.h"
#include "Runtime/MonoBehaviour.h"

namespace Summit
{

/// <summary>로컬 플레이어의 이동·점프를 처리하고, 맵 아래로 떨어지면 시작 위치로 복귀시킨다.</summary>
class PlayerController final : public GameEngine::Runtime::MonoBehaviour
{
public:
    /// <summary>프로필에서 고르는 캐릭터다. 숫자는 서버 프로토콜의 캐릭터 ID다.</summary>
    enum class Character : unsigned char
    {
        CharacterA = 0,
        CharacterB = 1,
        CharacterC = 2,
        CharacterD = 3,
        CharacterE = 4,
        Tdw = 5,
    };

    enum class AnimationState : unsigned char
    {
        Idle,
        Walk,
        Jump,
    };

    /// <summary>이 컴포넌트 클래스의 정체성이다.</summary>
    [[nodiscard]] static const GameEngine::Runtime::ComponentType& StaticType();
    [[nodiscard]] const GameEngine::Runtime::ComponentType& GetComponentType() const override
    {
        return StaticType();
    }

    /// <summary>초당 수평 이동 속도다.</summary>
    [[nodiscard]] float GetMoveSpeed() const { return mMoveSpeed; }
    void SetMoveSpeed(float moveSpeed);

    /// <summary>점프할 때 부여하는 위쪽 속도다.</summary>
    [[nodiscard]] float GetJumpSpeed() const { return mJumpSpeed; }
    void SetJumpSpeed(float jumpSpeed);

    /// <summary>월드 Y가 이 값보다 낮으면 시작 위치로 돌아간다. 가장 낮은 지형 아래에 둔다.</summary>
    [[nodiscard]] float GetRespawnY() const { return mRespawnY; }
    void SetRespawnY(float respawnY);

    /// <summary>현재 플레이어가 고른 캐릭터다.</summary>
    [[nodiscard]] Character GetSelectedCharacter() const { return mSelectedCharacter; }
    void SetSelectedCharacter(Character character);
    /// <summary>스프라이트 원본 방향과 독립적인 게임 내 이동 방향이다.</summary>
    [[nodiscard]] bool IsFacingLeft() const { return mFacingLeft; }
    void SetGameplayInputEnabled(bool enabled);
    void UpdateNameplate(GameEngine::Runtime::GameObject& player, const std::string& nickname) const;

    /// <summary>물리 루트와 분리된 캐릭터 그림을 찾는다.</summary>
    [[nodiscard]] static GameEngine::Runtime::GameObject& FindVisualObject(
        GameEngine::Runtime::GameObject& player);
    // 로컬과 원격이 동일한 그림 배율·방향 보정을 공유하며 물리 루트의 크기는 바꾸지 않는다.
    [[nodiscard]] static float GetVisualScale(Character character);
    [[nodiscard]] static bool IsSpriteFlipped(Character character, bool facingLeft);
    [[nodiscard]] float GetVisualOffsetY(Character character, AnimationState state) const;

    [[nodiscard]] const GameEngine::Assets::AssetReference& GetAtlas(
        Character character, AnimationState state) const;
    void SetAtlas(
        Character character, AnimationState state, GameEngine::Assets::AssetReference atlas);

    [[nodiscard]] float GetIdleFrameRate() const { return mIdleFrameRate; }
    void SetIdleFrameRate(float frameRate);

    [[nodiscard]] float GetWalkFrameRate() const { return mWalkFrameRate; }
    void SetWalkFrameRate(float frameRate);

    [[nodiscard]] float GetJumpFrameRate() const { return mJumpFrameRate; }
    void SetJumpFrameRate(float frameRate);

    void CollectAssetReferences(
        std::vector<GameEngine::Assets::AssetReference>& references) const override;

protected:
    void Start() override;
    void Update(float deltaTime) override;

private:
    struct CharacterAtlases
    {
        GameEngine::Assets::AssetReference idle;
        GameEngine::Assets::AssetReference walk;
        GameEngine::Assets::AssetReference jump;
    };

    [[nodiscard]] static std::size_t ToIndex(Character character);
    void ApplyAnimationState(AnimationState state);

    float mMoveSpeed = 5.0f;
    float mJumpSpeed = 8.0f;
    float mRespawnY = -10.0f;
    // Start에서 확정한 월드 좌표다. 이후 부모 이동이나 프로필 변경은 복귀 지점을 다시 정하지 않는다.
    GameEngine::Math::Vector3 mStartPosition;
    Character mSelectedCharacter = Character::CharacterA;
    std::array<CharacterAtlases, CharacterCount> mCharacterAtlases;
    float mIdleFrameRate = 6.0f;
    float mWalkFrameRate = 10.0f;
    float mJumpFrameRate = 8.0f;
    AnimationState mAnimationState = AnimationState::Idle;
    bool mAnimationDirty = true;
    bool mGameplayInputEnabled = false;
    bool mFacingLeft = false;
};

}

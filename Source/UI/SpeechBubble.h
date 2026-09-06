#pragma once

#include <cstddef>
#include <string_view>

#include "Runtime/MonoBehaviour.h"

namespace GameEngine::Runtime { class GameObject; }

namespace Summit
{
/// <summary>플레이어의 자식으로 붙어 마지막 승인된 채팅을 잠시 표시한다.</summary>
class SpeechBubble final : public GameEngine::Runtime::MonoBehaviour
{
public:
    static constexpr float DisplaySeconds = 6.0f;
    static constexpr std::size_t MaximumCodePoints = 48;

    [[nodiscard]] static const GameEngine::Runtime::ComponentType& StaticType();
    [[nodiscard]] const GameEngine::Runtime::ComponentType& GetComponentType() const override { return StaticType(); }

    /// <summary>기존 말풍선은 교체하고 시간을 갱신한다. 패널에 보관할 원문은 바꾸지 않는다.</summary>
    static SpeechBubble* ShowMessage(GameEngine::Runtime::GameObject& player, std::string_view message,
        float remainingSeconds = DisplaySeconds);
    [[nodiscard]] float GetRemainingSeconds() const { return mRemainingSeconds; }

protected:
    void Update(float deltaTime) override;

private:
    void UpdatePosition();
    // 자식 객체는 Scene 소유다. 메시지 교체·갱신 시 ID로 다시 조회해 삭제된 객체의 포인터를 보관하지 않는다.
    unsigned int mTextId = 0;
    unsigned int mTailId = 0;
    unsigned int mLayoutUpdatesToWait = 0;
    bool mMeasureUnwrapped = false;
    float mRemainingSeconds = 0;
};
}

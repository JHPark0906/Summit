#pragma once

#include "Math/Vector.h"
#include "Runtime/MonoBehaviour.h"

namespace Summit
{

/// <summary>직교 카메라의 직접 자식 스프라이트가 화면을 비율 유지하여 가득 채우게 한다.</summary>
class CameraBackgroundBehaviour final : public GameEngine::Runtime::MonoBehaviour
{
public:
    [[nodiscard]] static const GameEngine::Runtime::ComponentType& StaticType();
    [[nodiscard]] const GameEngine::Runtime::ComponentType& GetComponentType() const override
    {
        return StaticType();
    }

    /// <summary>배율 1인 스프라이트의 월드 크기다. 원본 픽셀 크기를 PPU로 나눈 값이다.</summary>
    [[nodiscard]] const GameEngine::Math::Vector2& GetNativeSize() const { return mNativeSize; }
    void SetNativeSize(const GameEngine::Math::Vector2& nativeSize);

protected:
    void Update(float deltaTime) override;

private:
    GameEngine::Math::Vector2 mNativeSize{ 20.48f, 10.24f };
};

}

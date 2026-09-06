#pragma once

#include "Math/Vector.h"
#include "Runtime/MonoBehaviour.h"

namespace Summit
{

/// <summary>직접 부모인 캐릭터 Transform을 기준으로 카메라 위치를 유지한다.</summary>
class CameraFollowBehaviour final : public GameEngine::Runtime::MonoBehaviour
{
public:
    /// <summary>이 컴포넌트 클래스의 정체성이다. 쿼리, 도구, 진단이 공유한다.</summary>
    [[nodiscard]] static const GameEngine::Runtime::ComponentType& StaticType();
    [[nodiscard]] const GameEngine::Runtime::ComponentType& GetComponentType() const override
    {
        return StaticType();
    }

    /// <summary>부모 캐릭터를 기준으로 한 카메라의 로컬 위치다.</summary>
    [[nodiscard]] const GameEngine::Math::Vector3& GetOffset() const { return mOffset; }
    void SetOffset(const GameEngine::Math::Vector3& offset) { mOffset = offset; }

protected:
    void Update(float deltaTime) override;

private:
    GameEngine::Math::Vector3 mOffset{ 0.0f, 0.0f, -5.0f };
};

}

#include "CameraBackgroundBehaviour.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "Runtime/Camera.h"
#include "Runtime/ComponentType.h"
#include "Runtime/GameObject.h"
#include "Runtime/PropertyDescriptor.h"
#include "Runtime/Transform.h"

namespace Summit
{

namespace Runtime = GameEngine::Runtime;

namespace
{
    std::span<const Runtime::PropertyDescriptor> CameraBackgroundProperties()
    {
        static const Runtime::PropertyDescriptor properties[] = {
            Runtime::MakeProperty<CameraBackgroundBehaviour>(
                "nativeSize", "Native Size", &CameraBackgroundBehaviour::GetNativeSize,
                &CameraBackgroundBehaviour::SetNativeSize),
        };
        return properties;
    }
}

const Runtime::ComponentType& CameraBackgroundBehaviour::StaticType()
{
    static const Runtime::ComponentType type{
        "CameraBackgroundBehaviour", &Runtime::MonoBehaviour::StaticType(),
        &CameraBackgroundProperties, &Runtime::MakeComponentInstance<CameraBackgroundBehaviour> };
    return type;
}

void CameraBackgroundBehaviour::SetNativeSize(const GameEngine::Math::Vector2& nativeSize)
{
    if (std::isfinite(nativeSize.GetX()) && nativeSize.GetX() > 0.0f &&
        std::isfinite(nativeSize.GetY()) && nativeSize.GetY() > 0.0f)
    {
        mNativeSize = nativeSize;
    }
}

void CameraBackgroundBehaviour::Update(const float /*deltaTime*/)
{
    Runtime::Transform* const transform = GetTransform();
    Runtime::Transform* const parent = transform ? transform->GetParent() : nullptr;
    Runtime::GameObject* const cameraObject = parent ? parent->GetGameObject() : nullptr;
    const Runtime::Camera* const camera =
        cameraObject ? cameraObject->GetComponent<Runtime::Camera>() : nullptr;
    if (!camera || camera->GetProjectionMode() != Runtime::Camera::ProjectionMode::Orthographic)
    {
        return;
    }

    const float height = camera->GetOrthographicSize() * 2.0f;
    const float width = height * camera->GetAspectRatio();
    // 큰 축의 배율을 택해 화면 전체를 덮는다. 비율 차이는 여백 대신 가장자리 잘림으로 처리한다.
    const float scale = (std::max)(width / mNativeSize.GetX(), height / mNativeSize.GetY());
    if (!std::isfinite(scale) || scale <= 0.0f)
    {
        return;
    }

    // 위치 복사로 따라가면 Update 뒤의 물리 이동보다 한 프레임 늦어진다. 카메라의 자식으로
    // 두면 렌더링 시점의 최종 이동·회전을 그대로 따르고 카메라 뷰에서도 항상 중앙에 놓인다.
    const float depth = camera->GetNearClipPlane() +
        (camera->GetFarClipPlane() - camera->GetNearClipPlane()) * 0.9f;
    transform->SetPosition({ 0.0f, 0.0f, depth });
    transform->SetRotation({ 0.0f, 0.0f, 0.0f });
    transform->SetScale({ scale, scale, 1.0f });
}

}

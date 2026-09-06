#include "CameraFollowBehaviour.h"

#include <span>

#include "Runtime/ComponentType.h"
#include "Runtime/PropertyDescriptor.h"
#include "Runtime/Transform.h"

namespace Summit
{

namespace Runtime = GameEngine::Runtime;

namespace
{
    std::span<const Runtime::PropertyDescriptor> CameraFollowProperties()
    {
        static const Runtime::PropertyDescriptor properties[] = {
            Runtime::MakeProperty<CameraFollowBehaviour>(
                "offset", "Offset", &CameraFollowBehaviour::GetOffset,
                &CameraFollowBehaviour::SetOffset),
        };
        return properties;
    }
}

const Runtime::ComponentType& CameraFollowBehaviour::StaticType()
{
    static const Runtime::ComponentType type{
        "CameraFollowBehaviour", &Runtime::MonoBehaviour::StaticType(),
        &CameraFollowProperties, &Runtime::MakeComponentInstance<CameraFollowBehaviour> };
    return type;
}

void CameraFollowBehaviour::Update(const float /*deltaTime*/)
{
    Runtime::Transform* const cameraTransform = GetTransform();
    if (!cameraTransform || !cameraTransform->GetParent())
    {
        return;
    }

    // 월드 위치를 복사하지 않아 물리 이동과 낙사 복귀가 부모 변환을 통해 같은 렌더 프레임에 반영된다.
    cameraTransform->SetPosition(mOffset);
}

}

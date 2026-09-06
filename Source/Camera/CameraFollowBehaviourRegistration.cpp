#include "CameraFollowBehaviour.h"

#include "Serialization/RuntimeComponentFactories.h"

namespace Summit
{

namespace
{
    const bool gCameraFollowBehaviourRegistered =
        GameEngine::Serialization::RegisterComponentType(CameraFollowBehaviour::StaticType());
}

}

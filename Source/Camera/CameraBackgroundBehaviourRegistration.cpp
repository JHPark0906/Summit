#include "CameraBackgroundBehaviour.h"

#include "Serialization/RuntimeComponentFactories.h"

namespace Summit
{

namespace
{
    const bool gCameraBackgroundBehaviourRegistered =
        GameEngine::Serialization::RegisterComponentType(CameraBackgroundBehaviour::StaticType());
}

}

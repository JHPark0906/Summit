#include "PlayerControllerFactory.h"

#include "PlayerController.h"
#include "Serialization/RuntimeComponentFactories.h"

namespace Summit
{

bool RegisterPlayerControllerFactory()
{
    return GameEngine::Serialization::RegisterComponentType(PlayerController::StaticType());
}

namespace
{
    const bool gPlayerControllerRegistered = RegisterPlayerControllerFactory();
}

}

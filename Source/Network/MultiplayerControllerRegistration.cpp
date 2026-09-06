#include "MultiplayerController.h"

#include "Serialization/RuntimeComponentFactories.h"

namespace Summit
{

namespace
{
    const bool gMultiplayerControllerRegistered =
        GameEngine::Serialization::RegisterComponentType(MultiplayerController::StaticType());
}

}
